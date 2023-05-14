#pragma once

#include "kronosrt.h"
#include "IObject.h"
#include "driver/CmdLineOpts.h"

#include <chrono>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>


namespace Kronos {
	class Context;
	namespace IO {
		using MicroSecTy = std::chrono::microseconds;
		using TimePointTy = std::chrono::time_point<std::chrono::high_resolution_clock, MicroSecTy>;
		using DurationTy = std::chrono::high_resolution_clock::duration;

		using ManagedObject = Runtime::DisposableReferenceCounted;
		using ManagedRef = pcoll::detail::ref<ManagedObject>;

        void ListAudioDevices(std::ostream& os);

		class IConfigurationDelegate {
		public:
			virtual void Set(const std::string& key, const std::string& value) = 0;
		};

		class IHierarchy {
		public:
			virtual ~IHierarchy() { }
			virtual void Subscribe(const Runtime::MethodKey&, const ManagedRef&, krt_instance, krt_process_call, void const**) = 0;
			virtual void Unsubscribe(const Runtime::MethodKey&, krt_instance) = 0;
			virtual bool HasActiveSubjects() const = 0;
		};


		class ITimer {
		public:
			virtual ~ITimer() {};
			virtual DurationTy GetPeriod() = 0;
		};

		class ITimerCallback {
		public:
			virtual void Timer() = 0;
		};

		std::unique_ptr<ITimer> CreateTimer(ITimerCallback*);

		class Subject : public IHierarchy, public ManagedObject {
		protected:
			struct Subscription {
                // callback is used to tombstone object nodes in the scheduler
				volatile krt_process_call callback;
				void const** slot;
				ManagedRef handle;
				bool garbage = false;
			};
			using SubscriberMapTy = std::unordered_map<krt_instance, Subscription>;
			std::mutex subscriberLock;
			SubscriberMapTy subscribers;
			const void *data = nullptr;

			Subscription* UnsafeSubscribe(const Runtime::MethodKey&, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot);
			void UnsafeUnsubscribe(const Runtime::MethodKey&, krt_instance instance);

		public:
			~Subject();
			bool HasActiveSubjects() const override;
			void Subscribe(const Runtime::MethodKey&, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) override;
			void Unsubscribe(const Runtime::MethodKey&, krt_instance instance) override;
			virtual void Bind(const void *newValue);
			virtual void Fire(void *output, int numFrames);
			virtual Runtime::MethodKey Id() const;

			void const** Slot() {
				return &data;
			}

			using Ref = pcoll::detail::ref<Subject>;
		};

		class Broadcaster : public virtual IHierarchy, public Runtime::ISubscriptionHost {
		protected:
			int symbolCounter = 0;
			std::unordered_map<Runtime::MethodKey, int, Runtime::MethodKey::Hash> symbolTable;
			std::unordered_map<int, IO::Subject::Ref> subjects;
			mutable std::mutex subscriberLock;
		public:
			~Broadcaster() {
				std::lock_guard<std::mutex> lg(subscriberLock);
				subjects.clear();
			}
			bool HasActiveSubjects() const override;

			void Unsubscribe(const char* sym, const char* sig, void *inst) override {
				Unsubscribe({ sym,sig }, inst);
			}
			void Subscribe(const Runtime::MethodKey&, const ManagedRef&, krt_instance, krt_process_call, void const**) override;
			void Unsubscribe(const Runtime::MethodKey&, krt_instance) override;
			void Dispatch(int symIndex, const void* arg, size_t argSz, void* res);
			void Bind(int symIndex, const void* data);
			int GetSymbolIndex(const Runtime::MethodKey& name) {
				std::lock_guard<std::mutex> lg(subscriberLock);
				auto f = symbolTable.find(name);
				if (f == symbolTable.end()) return -1;
				return f->second;
			}

			virtual void UnknownSubject(const Runtime::MethodKey&, const ManagedRef&, krt_instance, krt_process_call, void const**) { }
		
		};

		class Aggregator : public Subject {
			Runtime::MethodKey id;
			std::vector<Subject::Ref> subjects;
		public:
			Aggregator(const Runtime::MethodKey& id) :id(id) {}
			void Include(const Subject::Ref&);
			void Subscribe(const Runtime::MethodKey&, const ManagedRef&, krt_instance, krt_process_call, void const**) override;
			void Unsubscribe(const Runtime::MethodKey&, krt_instance) override;
			bool HasActiveSubjects() const override;
			virtual Runtime::MethodKey Id() const { return id; }
		};

		class IRegistry {
		public:
			virtual ~IRegistry() { }
			virtual void Register(Subject::Ref sub) = 0;
		};

		class IConfiguringHierarchy : public virtual IHierarchy {
		public:
			virtual void AddDelegate(IConfigurationDelegate&) = 0;
			virtual void RemoveDelegate(IConfigurationDelegate&) = 0;
		};

		std::unique_ptr<IConfiguringHierarchy> CreateCompositeIO();

		using RealtimeClock = std::chrono::microseconds(*)();

		void OverrideClock(RealtimeClock, int priority);
		TimePointTy Now();
		TimePointTy Now(RealtimeClock, TimePointTy stamp);
		TimePointTy& GetCurrentActivationTime();
		double& GetCurrentActivationRate();
	}
}
