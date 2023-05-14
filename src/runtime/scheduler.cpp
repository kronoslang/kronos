#include "scheduler.h"
#include <cmath> // kludge

namespace Kronos {
	namespace Runtime {

		static void StreamObjectTombstone(void*, void *, int) {}

		TimePointTy& VirtualTimePoint() {
			static thread_local TimePointTy vtp;
			return vtp;
		}

		TimingContextTy& TimingContext() {
			static thread_local TimingContextTy tc;
			return tc;
		}

		bool BlobRef::ByIdentity::operator()(const BlobRef& lhs, const BlobRef& rhs) const {
			return lhs.blob.get() < rhs.blob.get();
		}

		bool BlobRef::ByValue::less(const void* lData, size_t lSz, const void* rData, size_t rSz) const {
			if (lSz < rSz) return true;
			if (lSz > rSz) return false;
			if (lSz == 0) return false;
			return memcmp(lData, rData, lSz) < 0;
		}

		bool BlobRef::ByValue::operator()(const BlobRef& lhs, const BlobRef& rhs) const {
			return less(lhs.blob ? lhs.blob->data() : nullptr,
						lhs.blob ? lhs.blob->size() : 0ull,
						rhs.blob ? rhs.blob->data() : nullptr,
						rhs.blob ? rhs.blob->size() : 0ull);
		}

		bool BlobRef::ByValue::operator()(const BlobRef& lhs, const BlobView& rhs) const {
			return less(lhs.blob ? lhs.blob->data() : nullptr,
						lhs.blob ? lhs.blob->size() : 0ull,
						std::get<const void*>(rhs),
						std::get<size_t>(rhs));
		}

		bool BlobRef::ByValue::operator()(const BlobView& lhs, const BlobRef& rhs) const {
			return less(std::get<const void*>(lhs),
						std::get<size_t>(lhs),
						rhs.blob ? rhs.blob->data() : nullptr,
						rhs.blob ? rhs.blob->size() : 0ull);
		}

		BlobRef& BlobRef::operator=(const BlobView& rhs) {
			char *blobPtr = (char *)std::get<const void*>(rhs);
			blob = std::make_shared<Blob>(blobPtr, blobPtr + std::get<size_t>(rhs));
			return *this;
		}

		Scheduler::Scheduler(IEnvironment& env) 
			:env(env), pending(0), prerenderTarget(TimePointTy{}), didRenderUpTo(TimePointTy{}) {
		}

		void Scheduler::StartRealtimeThread(MicroSecTy tickInterval) {
			worker = IO::CreateTimer(this);
			this->tickInterval = std::chrono::duration_cast<MicroSecTy>(worker->GetPeriod());
		}

		Scheduler::~Scheduler() {
			StopRealtimeThread();
		}

		void Scheduler::StopRealtimeThread() {
			worker.reset();
		}

		void Scheduler::Schedule(const Event::Ref& e) {
			++pending;
			timeline.insert_into(e);
		}

		void Scheduler::Process(IO::TimePointTy processUpTo) {
			std::lock_guard<std::mutex> lg{ renderLock }; 
			decltype(timeline) events;
			while ((events = timeline.pop_up_to(processUpTo)).empty() == false) {
				auto expectedTimeline = timeline.identity();
				events.for_each([&](const Event::Ref& evt) {
					auto stamp = evt->timestamp;
					std::swap(stamp, VirtualTimePoint());
					evt->Fire(env);
					pending--;
					std::swap(stamp, VirtualTimePoint());
                    if (timeline.identity() != expectedTimeline) return false;
                    return true;
				});
			}
			didRenderUpTo.store(processUpTo, std::memory_order_release);
		}

		bool Scheduler::RenderEvents(IO::TimePointTy require, IO::TimePointTy speculate, bool block) {
			prerenderTarget.store(speculate, std::memory_order_release);
			if (didRenderUpTo.load() < require) {
				if (block) {
#ifndef NDEBUG
/*					std::clog << "Event queue starvation catchup: "
						<< (require - didRenderUpTo.load()).count() << "\n";*/
#endif
					ScriptContext sc{ SpeculativeScheduler };
					Process(require);
					return true;
				} else {
					return false;
				}
			}
			return true;
		}

		void Scheduler::DoWork() {
			ScriptContext sc{ SpeculativeScheduler };

			auto processUpto = IO::Now() + tickInterval;

			if (pending < 1) processUpto += std::chrono::milliseconds(50);

			auto target = prerenderTarget.load(std::memory_order_acquire);
			if (processUpto < target) processUpto = target;
			
			Process(processUpto);
		}
		
		void StreamSubject::StopCollectorThread() {
			runCollector.clear();
			if (collector.joinable()) collector.join();
		}

		void StreamSubject::StartCollectorThread() {
			StopCollectorThread();
			runCollector.test_and_set();
			collector = std::thread([this]() {
				using namespace std::chrono_literals;
				while (runCollector.test_and_set()) {
					SweepSchedule();
					std::this_thread::sleep_for(100ms);
				}
			});
		}


		StreamSubject::~StreamSubject() {
			StopCollectorThread();
			SweepSchedule();
			for (auto cur = subscriberList.next; cur; ) {
				auto next = cur->next;
				delete cur;
				cur = next;
			}
		}


		int StreamSubject::SweepSchedule() {
			auto hold = eventQueue; 
			// hang on to the version we're mutating to stop audio thread from having to delete
			eventQueue.remove_from([](const auto& evt) {
				return evt->kind == Event::Stale;
			});

			std::lock_guard<std::mutex> lg(subscriberLock);
			for (auto i = subscribers.begin();i != subscribers.end();) {
				if (i->second.callback == StreamObjectTombstone) {
					ObjectNode*  ptr = (ObjectNode*)i->second.slot;
					// deleting the instance will trigger removal from subscribers
					delete ptr;
					subscribers.erase(i++);
				} else {
					++i;
				}
			}
			
			return 0;
		}

	
		void StreamSubject::Subscribe(const Runtime::MethodKey& mk, const IO::ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) {
			std::unique_lock<std::mutex> lg(subscriberLock);
			auto sub = UnsafeSubscribe(mk, handle, instance, callback, slot);
			lg.unlock();

			auto on = std::make_unique<ObjectNode>(ObjectNode{ sub, instance });

			auto tp = VirtualTimePoint();
//			std::clog << "sub at " << tp.time_since_epoch().count() << "\n";

			eventQueue.insert_into(std::make_shared<Event>(
				Event::Subscribe,
				tp,
				0,
				BlobRef(),
				std::move(on)
			));
		}

		void StreamSubject::Unsubscribe(const Runtime::MethodKey&, krt_instance instance) {
			auto tp = VirtualTimePoint();
//			std::clog << "unsub at " << tp.time_since_epoch().count() << "\n";

			eventQueue.insert_into(std::make_shared<Event>(
				Event::Unsubscribe,
				tp,
				(int64_t)instance,
				BlobRef(),
				nullptr
			));
		}

		void StreamSubject::TimedDispatch(TimePointTy time, IObject* child, int sym, const void* data, size_t sz) {
			eventQueue.insert_into(std::make_shared<Event>(
				(Event::Kind)sym,
				time,
				(std::int64_t)child,
				BlobRef{ std::make_shared<Blob>((char*)data, (char*)data + sz) },
				nullptr
			));
		}
		using namespace std::chrono_literals;

		void StreamSubject::Fire(void* output, int numFrames) {
			using namespace std::chrono_literals;
			auto streamTime = IO::GetCurrentActivationTime(); 
			auto ticks_us = IO::GetCurrentActivationRate(); 
			auto upToSampleTime = Rendered + numFrames;

			if (ExpectedStreamTime != TimePointTy{}) {
				auto drift = streamTime - ExpectedStreamTime;
				if (drift > -1ms && drift < 1ms) {
					streamTime = ExpectedStreamTime;
				}
			}
            
			auto sliceDuration = std::chrono::microseconds((int)round(numFrames / ticks_us));
            ExpectedStreamTime = streamTime + sliceDuration;

			scriptExecutionEnvironment->RenderEvents(
				streamTime + sliceDuration, 
				streamTime + sliceDuration + sliceDuration, 
				true);

			ScriptContext context(RenderingStream);
            TimingContextTy old = Frozen;
            std::swap(TimingContext(), old);
            
			auto ts2StreamTime = [&](TimePointTy us) {
				auto nsOffset = us > streamTime ? us - streamTime : 0ns;
//				std::clog << us.time_since_epoch().count() << " <- " << streamTime.time_since_epoch().count() << "\n";
				auto smpOffset = int64_t(round(nsOffset.count() * ticks_us * 0.001));
				auto smpTime = Rendered + smpOffset;
				return smpTime;
			};

			char *outPtr = (char *)output;
			int64_t didRenderNow = 0;

			auto stepTo = [&](TimeTy to) {
				auto streamPos = Rendered + didRenderNow;
				if (to > streamPos) {
					auto toDo = to - streamPos;

					for (auto prev = &subscriberList; prev->next; prev = prev->next) {
						auto cur = prev->next;

						if (cur->subData->garbage) {
							// unlink and tombstone
							prev->next = cur->next;
                            cur->subData->slot = (const void**)cur;
                            // currently depends on strong memory ordering (like x86)
                            // to guarantee subData slot is observed by collector before
                            // tombstone. todo: write fence this
							cur->subData->callback = StreamObjectTombstone;
							if (!prev->next) break;
						} else {
							if (cur->subData->slot) {
								*cur->subData->slot = data;
							}

							if (cur->subData->callback) {
								cur->subData->callback(cur->instance, outPtr, (int)toDo);
							}

						}
					}

					outPtr += toDo * outputFrameSize;
					didRenderNow += toDo;
				}
			};


			for (bool loop = true; loop; ) {
				volatile auto eventQueueVersion = eventQueue.identity();
				
				loop = false;

				if (eventQueue.for_each([&](auto& evt) {
					if (evt->kind == Event::Stale) return true;

					loop = true;

					auto evtSampleTime = ts2StreamTime(evt->timestamp);

					if (evtSampleTime > upToSampleTime) {
						loop = false;
						return false;
					}

					stepTo(evtSampleTime);

					switch (evt->kind) {
					case Event::Subscribe:
							//std::clog << "audio sub " << evtSampleTime << "\n";
							evt->node->next = subscriberList.next;
							subscriberList.next = evt->node.release();
							break;
					case Event::Unsubscribe:
                        {
							//std::clog << "audio unsub " << evtSampleTime << "\n";
							auto i = subscribers.find((krt_instance)evt->param);
                            if (i != subscribers.end()) {
                                i->second.garbage = true;
                            } 
                        }
						break;
					case Event::Script:
						{
							auto stamp = evt->timestamp;
							std::swap(stamp, VirtualTimePoint());
							scriptExecutionEnvironment
								->Run(InteropTimestamp(VirtualTimePoint()), evt->param,
									  evt->data.blob->data(), evt->data.blob->size());
							std::swap(stamp, VirtualTimePoint());
						}
						break;
					default:
						assert(evt->kind >= Event::Dispatch);
						{
							auto stamp = evt->timestamp;
							std::swap(stamp, VirtualTimePoint());
							auto child = (IObject*)evt->param;
							int symIdx = (int)evt->kind;
//							float *tmp = (float*)evt->data.blob->data();
//							std::cout << symIdx << " <== " << *tmp << "\n";
							child->Dispatch(symIdx, evt->data.blob->data(), evt->data.blob->size(), nullptr);
							std::swap(stamp, VirtualTimePoint());
							break;
						}
					}

					evt->kind = Event::Stale;

					return eventQueueVersion == eventQueue.identity();
				})) break;			
			}
			stepTo(upToSampleTime);
			Rendered = upToSampleTime;
			std::swap(TimingContext(), old);
		}
	}
}
