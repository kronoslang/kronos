#include "midi.h"
#include "audio.h"

#if HAS_O2
#include "o2driver.h"
#endif

#include <chrono>
#include <thread>
#include <unordered_set>

namespace Kronos {
	namespace IO {
		static MicroSecTy WallClock() {
			auto ts = std::chrono::high_resolution_clock::now();
			auto dur = ts.time_since_epoch();
			return std::chrono::duration_cast<MicroSecTy>(dur);
		}

		static thread_local struct {
			TimePointTy Time;
			double Rate = 0;
		} ThreadActivation;

		struct {
			int Priority = 0;
			RealtimeClock Now = WallClock;
			MicroSecTy Previous = WallClock();
			MicroSecTy Monotonic = MicroSecTy{ 0 };
			std::mutex lock;
		} CurrentClock;

		TimePointTy Now() {
			MicroSecTy NewOut;
			{
				std::lock_guard<std::mutex> lg{ CurrentClock.lock };
				auto Current = CurrentClock.Now();
				auto Delta = Current - CurrentClock.Previous;
				NewOut = CurrentClock.Monotonic += Delta;
				CurrentClock.Previous = Current;
			}
			return TimePointTy(NewOut);
		}

		TimePointTy Now(RealtimeClock forMaster, TimePointTy Current) {
			if (CurrentClock.Now == forMaster) {
				std::lock_guard<std::mutex> lg{ CurrentClock.lock };
				return CurrentClock.Monotonic + Current - CurrentClock.Previous;
			} else {
				return Now();
			}
		}

		TimePointTy& GetCurrentActivationTime() {
			return ThreadActivation.Time;
		}

		double& GetCurrentActivationRate() {
			return ThreadActivation.Rate;
		}

		void OverrideClock(RealtimeClock newClock, int priority) {
			std::lock_guard<std::mutex> lg{ CurrentClock.lock };
			if (CurrentClock.Priority < priority) {
				auto Current = CurrentClock.Now();
				CurrentClock.Monotonic += Current - CurrentClock.Previous;
				CurrentClock.Previous = newClock();
				CurrentClock.Now = newClock;
				CurrentClock.Priority = priority;				
			}
		}

		Subject::~Subject() {
			std::lock_guard<std::mutex> lg(subscriberLock);
		}

		Subject::Subscription* Subject::UnsafeSubscribe(const Runtime::MethodKey&, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) {
			if (slot) {
				*slot = data;
			}
			
			return &subscribers.emplace(instance, Subscription{
				callback,
				slot,
				handle
			}).first->second;
		}

		void Subject::Subscribe(const Runtime::MethodKey& mk, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) {
			std::lock_guard<std::mutex> lg(subscriberLock);
			UnsafeSubscribe(mk, handle, instance, callback, slot);
		}
			
		bool Subject::HasActiveSubjects() const {
			return !subscribers.empty();
		}

		void Subject::UnsafeUnsubscribe(const Runtime::MethodKey&, krt_instance inst) {
			subscribers.erase(inst);
		}

		void Subject::Unsubscribe(const Runtime::MethodKey& mk, krt_instance instance) {
			std::lock_guard<std::mutex> lg(subscriberLock);
			UnsafeUnsubscribe(mk, instance);
		}

		void Subject::Bind(const void *newValue) {
			data = newValue;
			std::lock_guard<std::mutex> lg(subscriberLock);
			for (auto &s : subscribers) {
				if (s.second.slot) s.second.slot[0] = data;
			}
		}

		void Subject::Fire(void *output, int numFrames) {
			std::lock_guard<std::mutex> lg(subscriberLock);
			for (auto &s : subscribers) {
				auto &sub(s.second);
				if (sub.slot) sub.slot[0] = data;
				if (sub.callback) {
					sub.callback(s.first, output, numFrames);
				}
			}
		}

		Runtime::MethodKey Subject::Id() const {
			return { "subject", "%0" };
		}

		void Broadcaster::Subscribe(const Runtime::MethodKey& subject, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) {
			std::lock_guard<std::mutex> lg(subscriberLock);
			auto symIdx = symbolTable.find(subject.name);

			if (symIdx == symbolTable.end()) {
				UnknownSubject(subject, handle, instance, callback, slot);
				return;
			}

			subjects[symIdx->second]->Subscribe(subject, handle, instance, callback, slot);
		}

		void Broadcaster::Unsubscribe(const Runtime::MethodKey& subject, krt_instance instance) {
			std::lock_guard<std::mutex> lg(subscriberLock);
			auto sti = symbolTable.find(subject.name);
			if (sti != symbolTable.end()) {
				auto f = subjects.find(sti->second);
				if (f != subjects.end()) {
					f->second->Unsubscribe(subject, instance);
				}
			}
		}

		void Broadcaster::Bind(int symIndex, const void* data) {
			std::lock_guard<std::mutex> lg(subscriberLock);
			*subjects[symIndex]->Slot() = data;
		}

		void Broadcaster::Dispatch(int symIndex, const void* arg, size_t argSz, void* res) {
			std::lock_guard<std::mutex> lg(subscriberLock);
			auto f = subjects.find(symIndex);
			if (f != subjects.end()) {
				auto sub = f->second.get();
				*sub->Slot() = arg;
				sub->Fire(nullptr, 1);
			}
		}

		bool Broadcaster::HasActiveSubjects() const {
			bool active = false;
			std::lock_guard<std::mutex> lg(subscriberLock);

			for (auto &s : subjects) {
				active |= s.second->HasActiveSubjects();
			}

			return active;
		}

		void Aggregator::Include(const Subject::Ref& sub) {
			subjects.push_back(sub);
		}

		bool Aggregator::HasActiveSubjects() const {
			for (auto &s : subjects) {
				if (s->HasActiveSubjects()) return true;
			}
			return false;
		}

		void Aggregator::Subscribe(const Runtime::MethodKey& mk, const ManagedRef& mr, krt_instance inst, krt_process_call proc, void const** slot) {
			for (auto &s : subjects) s->Subscribe(mk, mr, inst, proc, slot);
		}

		void Aggregator::Unsubscribe(const Runtime::MethodKey& mk, krt_instance inst) {
			for (auto &s : subjects) s->Unsubscribe(mk, inst);
		}

		class Registry : public Broadcaster, public IRegistry, public IConfiguringHierarchy, public IConfigurationDelegate {
			std::unordered_set<IConfigurationDelegate*> configDelegates;
			std::unordered_map<std::string, std::string> configSettings;
            std::unique_ptr<IHierarchy> genericSubject;
		public:
			Registry(std::initializer_list<Subject::Ref>&& subs) {
				for (auto &s : subs) {
					Register(s);
				}
			}

			Registry() = default;

			~Registry() {

			}

			int Index = 0;

			void Register(Subject::Ref sub) override {
				int myIdx = Index++;
				symbolTable.emplace(sub->Id(), myIdx);
				subjects.emplace(myIdx, sub);
			}

			void AddDelegate(IConfigurationDelegate& del) override {
				for (auto &cs : configSettings) {
					del.Set(cs.first, cs.second);
				}
				configDelegates.emplace(&del);
			}

			void RemoveDelegate(IConfigurationDelegate& del) override {
				configDelegates.erase(&del);
			}
            
            void SetGenericHandler(std::unique_ptr<IHierarchy> sub) {
                genericSubject = std::move(sub);
            }
            
            void UnknownSubject(const Runtime::MethodKey& mk, const ManagedRef& mr, krt_instance inst, krt_process_call proc, void const** slot) override {
                genericSubject->Subscribe(mk, mr, inst, proc, slot);
            }

			void Set(const std::string& key, const std::string& value) override {
				if (configSettings[key] != value) {
					configSettings[key] = value;
					for (auto &cd : configDelegates) cd->Set(key, value);
				}
			}
		};

		std::unique_ptr<IConfiguringHierarchy> CreateCompositeIO() {
			auto reg = std::make_unique<Registry>();

			Audio::Setup(*reg, reg.get());
			MIDI::Setup(*reg);

 #ifdef HAS_O2
            reg->SetGenericHandler(o2::Setup(*reg, reg.get()));
 #endif

            return std::move(reg);
		}
	}
}
