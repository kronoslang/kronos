#pragma once

#include "kronosrtxx.h"
#include "scheduler.h"
#include <type_traits>

namespace Kronos {
	namespace Runtime {

		struct Stack {
			union {
				char bytes[32];
				void* ext;
			};
			void (*deleter)(void*) = nullptr;
		public:
			Stack() = default;

			~Stack() {
				Clear();
			}

			Stack(const Stack&) = delete;

			Stack(Stack&& from) {
				deleter = from.deleter;
				memcpy(bytes, from.bytes, 16);
				from.deleter = nullptr;
			}

			void Clear() {
				if (deleter) deleter(ext);
			}

			template <typename T>
			void Push(const T& data) {
				Clear();
				if (sizeof(T) > sizeof(bytes) || !std::is_trivially_destructible<T>::value) {
					ext = new T(data);
					deleter = [](void *ptr) { delete (T*)ptr; };
				} else {
					memcpy(bytes, &data, sizeof(T));
					deleter = nullptr;
				}
			}

			void Push(const void* data, size_t bytes) {
				Clear();
				ext = malloc(bytes);
				deleter = free;
				memcpy(ext, data, bytes);
			}

			const void* Data() const {
				return deleter ? ext : bytes;
			}
		};

		class Environment : public IEnvironment, public HierarchyBroadcaster {
			using SchedulerPtrTy = std::unique_ptr<Scheduler>;
		protected:
			SchedulerPtrTy scheduler;
			InstanceMapTy instances;
			StreamSubject* audioHost = nullptr;
			MethodKey audioSymbol;
			std::int64_t world;
			IBuilder &builder;
			virtual Scheduler& GetScheduler();
			virtual void Schedule(int64_t timestamp, int64_t closureTy,
								  const void* closureArg, size_t closureSz);

			size_t outFrameSz;
			std::vector<char> outputBus;
			Runtime::Instance::Ref BuildInstance(std::int64_t uid, const Runtime::BlobView& blob);
			static thread_local Stack pseudoStack;
			bool deterministicBuild = false;
			void Connect(const ClassCode&, krt_instance, IO::ManagedRef);
		public:
			Environment(IO::IHierarchy* ioParent, IBuilder& builder, std::int64_t outFrameUid, size_t outFrameSz);
			~Environment();
			void Run(int64_t timestamp, int64_t closureTy, const void* closureArg, int64_t closureSz) override;
            void Render(const char *audioFile, int64_t closureTy, const void* closureArg, float sampleRate, int64_t numFrames) override;
			int64_t Start(int64_t closureTy, const void* closureData, size_t closureSz) override;
			bool Stop(int64_t instanceId) override;
            int StopAll() override;
			void UnsubscribeAll(ISubscriptionHost*) override;
			void Dispatch(int symIdx, const void* arg, size_t argSz, void*) override;
			void DispatchTo(IObject* child, int symIdx, const void* arg, size_t argSz, void*) override;
			void Bind(int symIndex, const void* data) override;
			int GetSymbolIndex(const MethodKey& name) override;
			IObject::Ref GetChild(std::int64_t id) override;
			void *Id() const override;
			bool HasPendingEvents() const;
			size_t SizeOfOutput() const override { return outFrameSz; }
			void Finalize(Runtime::ClassCode&);
			void Require(const krt_sym* sym);
			int64_t Now();
			float SchedulerRate();
			void Pop(int64_t type, void* result) override;
			void Push(int64_t type, const void* data) override;
			void Shutdown();
			void SetDeterministic(bool value);
			IEnvironment** GetHost() override { return (IEnvironment**)&world; }

			bool RenderEvents(IO::TimePointTy require, IO::TimePointTy speculateUpTo, bool block) override {
				return GetScheduler().RenderEvents(require, speculateUpTo, block);
			}

			IO::Subject::Ref MakeSubject(const MethodKey&) override;

			void EnumerateSymbols(const ObjectSymbolEnumeratorTy&) const override;
			void EnumerateChildren(const ChildEnumerator&) const override;
		};
	}
}
