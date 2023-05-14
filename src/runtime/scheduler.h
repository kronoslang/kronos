#pragma once

#include <memory>
#include <cstring>
#include "pcoll/treap.h"
#include "kronosrtxx.h"

namespace Kronos {
	namespace Runtime {
		using IO::TimePointTy;

		using MicroSecTy = std::chrono::microseconds;

		static int64_t InteropTimestamp(TimePointTy tp) {
			return tp.time_since_epoch().count();
		}

		using TimeTy = std::int64_t;
		struct TimePoint {
			TimePointTy timestamp;
			int64_t param;
			BlobRef data;
            
			struct Less {
				template <typename T>
				bool operator()(const std::shared_ptr<T>& a, const std::shared_ptr<T>& b) const {
					if (a->timestamp < b->timestamp) return true;
					if (a->timestamp > b->timestamp) return false;
					// this code is brittle in case of padding or other undefined bytes
					const auto cmpSz = ((char*)a.get() + sizeof(T)) - (char*)a->param;
					return memcmp(&a->param, &b->param, cmpSz) < 0;
				}
                
				template <typename T>
				bool operator()(const std::shared_ptr<T>& a, TimePointTy b) const {
					if (a->timestamp < b) return true;
					return false;
				}

				template <typename T>
				bool operator()(TimePointTy a, const std::shared_ptr<T>& b) const {
					if (a < b->timestamp) return true;
					return false;
				}
			};

			struct Hash {
				template <typename T>
				size_t operator()(const std::shared_ptr<T>& a) const {
					return hash_combine(a->timestamp.time_since_epoch().count(), a->param);
				}
			};
		};

		enum TimingContextTy {
			Realtime = 0,
			Frozen,
			SpeculativeScheduler,
			RenderingStream
		};

		TimingContextTy& TimingContext();
		TimePointTy& VirtualTimePoint();

		struct ScriptContext {
			ScriptContext(const ScriptContext&) = delete;
			TimingContextTy old;
			ScriptContext(TimingContextTy nc) :old(nc) {
				std::swap(old, TimingContext());
			}

			~ScriptContext() {
				std::swap(old, TimingContext());
			}
		};

		

		class Scheduler : public IO::ITimerCallback {
			IEnvironment& env;
		public:
			ALIGNED_NEW_DELETE(Scheduler)

			using InstanceTy = BlobRef;

			struct Event : public TimePoint {
				using Ref = std::shared_ptr<Event>;
				void Fire(IEnvironment& env) {
					env.Run(InteropTimestamp(timestamp), param, data.blob->data(), data.blob->size());
				}
				Event(TimePointTy timestamp, int64_t closureTy, BlobRef blob)
					:TimePoint({ timestamp,closureTy,blob }) {}
			};

			using TimeLineTy = pcoll::treap<Event::Ref, TimePoint::Less, TimePoint::Hash>;
			TimeLineTy timeline;

			std::unique_ptr<IO::ITimer> worker;

			TimePointTy scheduledTime;
			MicroSecTy tickInterval;

			std::atomic<int> pending;
			
			void Process(TimePointTy upTo);
			void DoWork();
			void Timer() override { DoWork(); }

			Scheduler(IEnvironment&);
			virtual ~Scheduler();

			void StartRealtimeThread(MicroSecTy interval);
			void StopRealtimeThread();

			void Schedule(const Event::Ref&);

			float Rate() {
				return 1000000.f;
			}

			int64_t SchedulerTime(TimePointTy tp) {
				return InteropTimestamp(TimePointTy(tp));
			}

			int64_t Now() {
                int64_t curtime;
				switch (TimingContext()) {
                    case Realtime:
                        curtime = SchedulerTime(IO::Now());
                        break;
                    default: {
                        curtime = InteropTimestamp(VirtualTimePoint());
                        break;
                    }
				}
                return curtime;
			}

			bool Pending() {
				return pending > 0;
			}

			std::atomic<TimePointTy> prerenderTarget, didRenderUpTo;
			std::mutex renderLock;

			bool RenderEvents(TimePointTy require, TimePointTy speculateUpTo, bool block);
		};

		class StreamSubject : public IO::Subject {
			struct ObjectNode {
				Subscription* subData = nullptr;
				krt_instance instance = nullptr;
				ObjectNode* next = nullptr;
                using URef = std::unique_ptr<ObjectNode>;
			};

			struct Event : public TimePoint {
				enum Kind {
					Stale = -4,
					Subscribe = -3,
					Unsubscribe = -2,
					Script = -1,
					Dispatch = 0
				} kind;

				using Ref = std::shared_ptr<Event>;

				std::unique_ptr<ObjectNode> node;

				Event(Kind kind, TimePointTy time, int64_t param, BlobRef blob, ObjectNode::URef node)
					:TimePoint(TimePoint{ time, param, std::move(blob) }),
					kind(kind), node(std::move(node)) {}
				Event() {}
			};

			pcoll::treap<Event::Ref, TimePoint::Less, TimePoint::Hash> eventQueue;

			ObjectNode subscriberList;

			IEnvironment* scriptExecutionEnvironment;

		protected:
			size_t outputFrameSize;
			size_t Rendered = 0;

			TimePointTy ExpectedStreamTime;

			std::thread collector;
			std::atomic_flag runCollector;
		public:
			StreamSubject(IEnvironment *scriptHost, size_t outputFrameSize)
				: scriptExecutionEnvironment(scriptHost)
				, outputFrameSize(outputFrameSize) {
				StartCollectorThread();
			}

			~StreamSubject();

			void Fire(void* output, int numFrames) override;
			void Subscribe(const Runtime::MethodKey&, const IO::ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) override;
			void Unsubscribe(const Runtime::MethodKey&, krt_instance) override;

			IEnvironment& Environment() {
				return *scriptExecutionEnvironment;
			}

			void StopCollectorThread();
			void StartCollectorThread();
			int SweepSchedule();

			bool Pending() const {
				return !eventQueue.empty();
			}

			void TimedDispatch(TimePointTy timePoint, IObject* target, int symbol, const void* data, size_t sz);

			TimeTy PresentationLatency() { return 0; }

			// ObjectNode subscriberList requires memory alignment
			ALIGNED_NEW_DELETE(StreamSubject)
		};
	}
};
