#include <functional>
#include <mutex>
#include <ostream>
#include <iomanip>
#include <cmath>
#include <iostream>
#include <limits>

#include "pcoll/hamt.h"
#include "Environment.h"
#include "kronos.h"
#include "config/system.h"

#include "paf/PAF.h"

#include <xmmintrin.h>

#ifdef HAVE_FMT
#include <fmt/ostream.h>
#endif

using std::isnan;
using std::isinf;

namespace Kronos {
	namespace Runtime {
		// Dan Bernstein's hash
		static size_t CstrHash(const char *s, size_t h = 5381) {
			if (s) for (int c; (c = *s++); h = ((h << 5) + h) ^ c);
			return h;
		}

		size_t MethodKey::Hash::operator()(const MethodKey& mk) const {
			return CstrHash(mk.name);
		}

		bool MethodKey::operator==(const MethodKey& mk) const {
			if (name != mk.name && strcmp(name, mk.name)) return false;
			if (signature && mk.signature) {
				if (strcmp(signature, mk.signature)) return false;
			}
			return true;
		}

		Instance::~Instance() {
//			Class()->destruct(instance, host);
			_mm_free(instance);
		}

		void Instance::Dispatch(int symIdx, const void* arg, size_t argSz, void* result) {
			if (symIdx < 0 || symIdx >= Class()->num_symbols) return;
			auto& sym(Class()->symbols[symIdx]);
			if (sym.slot_index >= 0) {
				*Class()->var(instance, sym.slot_index) = (void*)arg;
                if (sym.process) sym.process(instance, result, 1);
			}
		}

		void Instance::Bind(int index, const void* data) {
			if (index >= 0) {
				*Class()->var(instance, index) = (void*)data;
			}
		}

		int Instance::GetSymbolIndex(const MethodKey& mk) {
			for (int i = 0;i < Class()->num_symbols;++i) {
				if (mk == Class()->symbols[i]) {
					return i;
				}
			}
			return -1;
		}

		void Instance::UnsubscribeAll(ISubscriptionHost* host) {
			auto &c(*myClass);
			for (int i = 0; i < c->num_symbols; ++i) {
				host->Unsubscribe(c->symbols[i].sym, c->symbols[i].type_descriptor, instance);
			}
		}

		void Instance::EnumerateSymbols(const ObjectSymbolEnumeratorTy& e) const {
			auto &c(*myClass);
			for (int i = 0; i < c->num_symbols; ++i) {
				e(i, MethodKey(c->symbols[i]));
			}
		}

		template <typename T> void Consume(std::ostream& to, const void*& data) {
			to << *(T*)data;
			data = (const char*)data + sizeof(T);
		}

		template <typename T> void ConsumeFloat(std::ostream& to, const void*& data, bool handleNanInf) {
			const T* p = (const T*)data;
			if (handleNanInf) {
				if (isnan(*p)) {
					to << "\"NaN\"";
					data = p + 1;
					return;
				} else if (isinf(*p)) {
					to << "\"inf\"";
					data = p + 1;
					return;
				}
			}
		#ifdef HAVE_FMT
			fmt::print(to, "{}", *p);
		#else
			to.precision(std::numeric_limits<T>::max_digits10 - 1);
			to << *p;
		#endif
			data = p + 1;
		}

		const char* ToStream(std::ostream& os, const char* typeInfo, const void*& dataBlob, bool handleNanInf) {
			if (dataBlob) {
				for (;;) {
					switch (char c = *typeInfo++) {
					case '\0': return typeInfo;
					case '%':
						c = *typeInfo++;
						switch (c) {
						case 'f': ConsumeFloat<float>(os, dataBlob, handleNanInf); break;
						case 'd': ConsumeFloat<double>(os, dataBlob, handleNanInf);  break;
						case 'i': Consume<std::int32_t>(os, dataBlob);  break;
						case 'q': Consume<std::int64_t>(os, dataBlob);  break;
                        case '[':{
                            char *loopPoint;
                            for(auto loopCount = strtoull(typeInfo, &loopPoint, 10); loopCount; --loopCount)
                                typeInfo = ToStream(os, loopPoint + 1, dataBlob, handleNanInf);
                            break;
                        }
                        case ']': return typeInfo;
						case '%': os.put('%'); break;
						default:
							assert(0 && "Bad format string");
						}
						break;
					default:
						os.put(c);
						break;
					}
				}
			} else {
				for (;;) {
					switch (char c = *typeInfo++) {
					case '\0': return typeInfo;
					case '%':
						c = *typeInfo++;
						switch (c) {
						case 'f': os << "\"Float\""; break;
						case 'd': os << "\"Double\"";  break;
						case 'i': os << "\"Int32\"";  break;
						case 'q': os << "\"Int64\""; break;
                        case '[':{
                            char *loopPoint;
                            for(auto loopCount = strtoull(typeInfo, &loopPoint, 10); loopCount; --loopCount)
                                typeInfo = ToStream(os, loopPoint + 1, dataBlob, handleNanInf);
							break;
                        }
                        case ']': return typeInfo;
						case '%': os.put('%'); break;
                                
						default:
							assert(0 && "Bad format string");
						}
						break;
					default:
						os.put(c);
						break;
					}
				}
			}
            return typeInfo;
		}

		std::ostream& operator<<(std::ostream& os, const Value& v) {
            auto dataPtr = v.data;
			ToStream(os, v.descriptor, dataPtr, false);
			return os;
		}

		ClassCode::ClassCode(ClassCode::Data&& d) :classData(std::move(d)) {
		}

		void HierarchyBroadcaster::UnknownSubject(const MethodKey& mk, const IO::ManagedRef& r, krt_instance inst, krt_process_call proc, void const** slot) {
			auto heldName = stringStore.emplace(mk.name).first;
			auto heldSig = stringStore.emplace(mk.signature).first;
			MethodKey heldMk{ heldName->c_str(), heldSig->c_str() };

			auto symi = symbolTable.find(heldMk);
			if (symi == symbolTable.end()) {
				int symIdx = symbolCounter++;
				symbolTable.emplace(heldMk, symIdx);

				auto subject = MakeSubject(heldMk);
				subjects[symIdx] = subject;

				subject->Subscribe(mk, r, inst, proc, slot);

				ioParent->Subscribe(mk, subject, subject.get(), [](krt_instance ptr, void* output, int32_t numFrames) {
					auto sub = (IO::Subject*)ptr;
					sub->Fire(output, numFrames);
				}, subject->Slot());

				if (slot) *slot = *subject->Slot();
			}
		}

		Environment::Environment(IO::IHierarchy* parent, IBuilder& c, int64_t outFrameTy, size_t outFrameSz) 
			: builder(c), 
			  HierarchyBroadcaster(parent),
			  outFrameSz(outFrameSz) {
			world = (int64_t)((IEnvironment*)this);
		}

		thread_local Stack Environment::pseudoStack;

		void Environment::Connect(const ClassCode& c, krt_instance inst, IO::ManagedRef ref) {
			for (int i = 0; i < c.classData->num_symbols; ++i) {
				auto &sym{ c.classData->symbols[i] };

				if (!strcmp(sym.sym, "#:VM:World:World")) {
					*c.classData->var(inst, sym.slot_index) = GetHost();
				} else if (!strcmp(sym.sym, "arg")) {
				} else {
					void** slot = sym.slot_index >= 0 
						? c.classData->var(inst, sym.slot_index) 
						: nullptr;
					Subscribe(sym, ref, inst, sym.process, (const void**)slot);
				}
			}
		}

		Runtime::Instance::Ref Environment::BuildInstance(std::int64_t uid, const Runtime::BlobView& blob) {
			auto class_ = builder([this](Runtime::ClassCode& cc) mutable {
				this->Finalize(cc);
			}, 0, uid, OmitEvaluate | (deterministicBuild ? UserFlag1 : 0)).get();

			const int align = 32;

			size_t sz = (size_t)(*class_)->get_size();
			sz = (sz + align - 1) & -align;
			auto memory = _mm_malloc(sz + std::get<size_t>(blob), align);
			memset(memory, 0, sz + std::get<size_t>(blob));
			auto instanceMemory = memory;
			auto closureMemory = (char*)memory + sz;

			pcoll::detail::ref<Instance> metaData = new Instance(class_, instanceMemory, closureMemory);

			memcpy(metaData->Closure(), std::get<const void*>(blob), std::get<size_t>(blob));

			Connect(*class_, instanceMemory, metaData);

			(*class_)->construct(instanceMemory, closureMemory);
			pseudoStack.Push((int64_t)metaData->Id());
			return metaData;
		}

		void Environment::SetDeterministic(bool v) {
			deterministicBuild = v;
		}

		void Environment::Pop(int64_t sz, void* write) {
			if (sz) memcpy(write, pseudoStack.Data(), (size_t)sz);
		}

		void Environment::Push(int64_t sz, const void* data) {
			pseudoStack.Push(data, (size_t)sz);
		}

		IObject::Ref Environment::GetChild(std::int64_t id) {
			return instances.get((void*)id);
		}

		Scheduler& Environment::GetScheduler() {
			if (!scheduler) {
				using namespace std::chrono_literals;
				scheduler = std::make_unique<Scheduler>(*this);
				scheduler->StartRealtimeThread(1ms);
			}
			return *scheduler;
		}

		void Environment::Schedule(int64_t timestamp, int64_t closureTy, const void* closureData, size_t closureSz) {
			GetScheduler().Schedule(std::make_shared<Runtime::Scheduler::Event>(
				TimePointTy(MicroSecTy(timestamp)),
				closureTy,
				Runtime::BlobRef{ std::make_shared<Runtime::Blob>((char*)closureData, (char *)closureData + closureSz) }
			));
		}

		void Environment::Require(const krt_sym* sym) {
			auto symi = symbolTable.find(*sym);
			if (symi == symbolTable.end()) {
				auto heldName = stringStore.emplace(sym->sym).first;
				auto heldSig = stringStore.emplace(sym->type_descriptor).first;
				MethodKey heldMk{ heldName->c_str(), heldSig->c_str() };

				int symIdx = symbolCounter++;
				symbolTable.emplace(heldMk, symIdx);

				auto subject = MakeSubject(heldMk);
				subjects[symIdx] = subject;

				ioParent->Subscribe(heldMk, subject, subject.get(), [](krt_instance ptr, void* output, int32_t numFrames) {
					auto sub = (IO::Subject*)ptr;
					sub->Fire(output, numFrames);
				}, subject->Slot());
			}
		}

		void Environment::Finalize(Runtime::ClassCode& class_) {			
			for (int i = 0;i < class_->num_symbols;++i) {
				if (!strcmp(class_->symbols[i].sym, "#:VM:World:World")) {
					//class_->configure(class_->symbols[i].slot_index, GetHost());
				} else if (!strcmp(class_->symbols[i].sym, "arg")) {
				} else if ((class_->symbols[i].flags & KRT_FLAG_NO_DEFAULT) != 0) {
					const krt_sym* sym = class_->symbols + i;
					Require(sym);
					class_->configure(class_->symbols[i].slot_index, 
									  *subjects[GetSymbolIndex(*sym)]->Slot());
				}

				if (!strcmp(class_->symbols[i].sym, "audio")) {
					class_.hasStreamClock = true;
				}
			}
		}

		void Environment::Run(int64_t timestamp, int64_t closureTy, const void* closureArg, int64_t closureSz) {
			if (TimingContext() == Realtime) {
				// freeze realtime during the script
				auto dur = Now();
				auto usec = MicroSecTy(dur);
				VirtualTimePoint() = TimePointTy(usec);
				ScriptContext sc(Frozen);
				Run(timestamp, closureTy, closureArg, closureSz);
			} else {
				auto now = Now();                
				if (TimingContext() == Frozen || (timestamp && timestamp > now)) {
					Schedule(timestamp, closureTy, closureArg, (size_t)closureSz);
				} else {
					auto &class_ = *builder([this](ClassCode& cc) mutable {
						this->Finalize(cc);
					}, timestamp, closureTy, OmitReactiveDrivers).get().get();

					auto instanceMemory = alloca((size_t)class_->get_size());
					auto resultMemory = alloca((size_t)class_->result_type_size);
					Connect(class_, instanceMemory, {});
					class_->construct(instanceMemory, closureArg);
					class_->eval(instanceMemory, closureArg, resultMemory);
				}
			}
		}
    
        void Environment::Render(const char* audioFile, int64_t closureTy, const void* closureArg, float sampleRate, int64_t numFrames) {
            auto &class_ = *builder([this](ClassCode& cc) mutable {
                this->Finalize(cc);
            }, 0, closureTy, OmitEvaluate).get().get();

            std::vector<char> instanceMemory(class_->get_size());

            class_->construct(instanceMemory.data(), closureArg);
            
            const int64_t blockFrames = 4096;
            std::vector<float> input, output;
            
            krt_sym* audioDriver = nullptr;

            for (int i = 0; i < class_.classData->num_symbols; ++i) {
                auto &sym{ class_.classData->symbols[i] };
                if (!strcmp(sym.sym, "audio")) {
                    input.resize(blockFrames * sym.size / sizeof(float));
                    audioDriver = &sym;
                } else if (!strcmp(sym.sym, "#Rate{Audio}")) {
                    *class_->var(instanceMemory.data(), sym.slot_index) = &sampleRate;
                }
            }
            
            if (audioDriver) {
                auto numCh = (int)(class_->result_type_size / sizeof(float));
                output.resize(blockFrames * numCh);
                                
                PAF::AudioFileWriter writeFile(audioFile);
                writeFile->Set(PAF::SampleRate, (int)sampleRate);
                writeFile->Set(PAF::NumChannels, (int)numCh);

	            writeFile->TrySet(PAF::BitDepth, 24);
				writeFile->TrySet(PAF::BitRate, 128000 * numCh);
                
                while(numFrames > 0) {
                    auto todo = std::min(numFrames, blockFrames);
                    
                    if (input.size()) {
                        *class_->var(instanceMemory.data(), audioDriver->slot_index) = input.data();
                    }
                    
                    audioDriver->process(instanceMemory.data(), output.data(), (int32_t)todo);
                    writeFile(output.data(), (int)(todo * numCh));
                    numFrames -= todo;
                }
                writeFile.Close();
            }
        }

		int64_t Environment::Start(int64_t closureTy, const void* closureArg, size_t closureSz) {
			auto instRef = BuildInstance(closureTy, BlobView{ closureArg, closureSz });
			if (instRef.empty() == false) {

				instances.update_in(instRef->Id(), [&](const auto&) {
					return instRef;
				});
				int64_t key = (int64_t)instRef->Id();
				//			ToOut("out", "<< starting %q >>", &key, true);
				return key;
			} else {
				return 0ll;
			}
		}

		bool Environment::Stop(int64_t instanceId) {
			IObject::Ref outGoing;
			instances.update_in((void*)instanceId, [&outGoing](const pcoll::optional<IObject::Ref> oref) {
				outGoing = IObject::Ref();
				if (oref.has_value) outGoing = *oref; ;
				return pcoll::none();
			});
			if (outGoing.empty() == false) {
				outGoing->UnsubscribeAll(this);
			} 
			return !outGoing.empty();
		}
        
        int Environment::StopAll() {
            int count = 0;
            InstanceMapTy tmp;
            instances.swap(tmp);
            tmp.for_each([&](const auto&, const IObject::Ref& obj) {
                ++count;
                obj->UnsubscribeAll(this);
            });
			tmp = {};
            return count;
        }

		Environment::~Environment() {
			scheduler.reset();
			StopAll();
		}

		void Environment::Shutdown() {
			for (auto &sym : symbolTable) {
				ioParent->Unsubscribe(sym.first, subjects[sym.second].get());
			}

			scheduler.reset();
			StopAll();

			while (HasPendingEvents()) {
				using namespace std::chrono_literals;
				std::clog << "Waiting for pending events..." << std::endl;
				std::this_thread::sleep_for(50ms);
			}

			subjects.clear();
		}

		int64_t Environment::Now() {
			return GetScheduler().Now();
		}
		
		float Environment::SchedulerRate() { return GetScheduler().Rate(); }

		void Environment::Dispatch(int symIndex, const void* arg, size_t argSz, void *res) {
			Broadcaster::Dispatch(symIndex, arg, argSz, res);
		}

		void Environment::DispatchTo(IObject* child, int symIndex, const void* arg, size_t argSz, void *res) {
			if (res == nullptr && child->HasStreamClock()) {
				GetScheduler();
				audioHost->TimedDispatch(VirtualTimePoint(), child, symIndex, arg, argSz);
			} else {
				child->Dispatch(symIndex, arg, argSz, res);
				if (res) pseudoStack.Push(res, child->SizeOfOutput());
			}
		}

		bool Environment::HasPendingEvents() const {
			if (scheduler && scheduler->Pending()) return true;
			return false;
		}

		void Environment::Bind(int symIndex, const void* data) {
			Broadcaster::Bind(symIndex, data);
		}

		void Environment::UnsubscribeAll(ISubscriptionHost* host) {
			for (auto &s : symbolTable) {
				host->Unsubscribe(s.first.name, s.first.signature, this);
			}
		}

		void Environment::EnumerateSymbols(const ObjectSymbolEnumeratorTy& e) const {
			for (auto &s : symbolTable) {
				e(s.second, s.first);
			}
		}

		void Environment::EnumerateChildren(const ChildEnumerator& e) const {
			instances.for_each([&e](void* key, const IObject::Ref&) {
				e((int64_t)key);
			});
		}

		int Environment::GetSymbolIndex(const MethodKey& mk) {
			return Broadcaster::GetSymbolIndex(mk);
		}

		void *Environment::Id() const {
			return (void*)this;
		}

		IO::Subject::Ref Environment::MakeSubject(const MethodKey& sym) {
			if (!strcmp(sym.name, "audio")) {
				GetScheduler();
				audioSymbol = sym;
				return audioHost = new StreamSubject(this, (size_t)SizeOfOutput());
				audioHost->StartCollectorThread();
			} else {
				return HierarchyBroadcaster::MakeSubject(sym);
			}
		}
	}
}
