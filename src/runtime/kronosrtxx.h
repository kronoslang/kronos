#pragma once

#include <memory>
#include <future>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <string>

#include "pcoll/llist.h"
#include "pcoll/hamt.h"

#include "kronosrt.h"
#include "IObject.h"
#include "inout.h"

#include "config/system.h"

#ifdef _MSC_VER
#pragma comment(linker, "/include:link_kvm")
#endif

namespace {
	size_t hash_combine() {
		return 0ull;
	}

	template <typename A1, typename... AS> size_t hash_combine(const A1& a1, const AS&... as) {
		std::hash<A1> hasher;
		return hasher(a1) ^ hash_combine(as...);
	}
}

namespace Kronos {
	namespace Runtime {
		struct Value {
			const char *descriptor;
			const void *data;
		};

		struct OwnedValue {
			std::string descriptor;
			std::vector<char> data;
			operator const Value () const {
				return Value{ descriptor.data(), data.data() };
			}
		};

		std::ostream& operator<<(std::ostream& os, const Value& v);
		const char* ToStream(std::ostream& os, const char* typeInfo, const void*& dataBlob, bool handleNanInf);

		using ChildEnumerator = std::function<bool(int64_t)>;

		class IEnvironment : public IObject {
		public:
			virtual void ToOut(const char* pipe, const char* type, const void* blob, bool newline = false) = 0;
			virtual void Run(int64_t timestamp, int64_t closureTy, const void* closureArg, int64_t closureSz) = 0;
			virtual int64_t Start(int64_t closureType, const void *closureData, size_t closureSz) = 0;
			virtual void DispatchTo(IObject* child, int symIdx, const void* blob, size_t blobSz, void* res) = 0;
			virtual bool Stop(int64_t id) = 0;
            virtual int StopAll() = 0;
			virtual int64_t Now() = 0;
			virtual float SchedulerRate() = 0;
			virtual void Pop(int64_t type, void* write) = 0;
			virtual void Push(int64_t type, const void* data) = 0;
            virtual void Render(const char* audioFile, int64_t closureTy, const void* closureArg, float audioSr, int64_t numFrames) = 0;
			virtual IObject::Ref GetChild(int64_t id) = 0;
			virtual IEnvironment** GetHost() = 0;
			virtual bool RenderEvents(IO::TimePointTy require, IO::TimePointTy speculateUpTo, bool block) = 0;
			virtual void EnumerateChildren(const ChildEnumerator&) const = 0;
		};


		struct ClassCode {
			using Data = std::unique_ptr<krt_class, void(*)(krt_class*)>;
			Data classData;
			bool hasStreamClock = false;
			krt_class* operator->() { return classData.get(); }
			ClassCode(Data&& k);
			ClassCode(const ClassCode&) = delete;
			void operator=(ClassCode) = delete;
		};

		using ClassRef = std::shared_ptr<ClassCode>;
		using BuildResultFuture = std::shared_future<Runtime::ClassRef>;

		class IBuilder {
		public:
			virtual BuildResultFuture operator()(std::function<void(ClassCode&)> finalizer, int64_t priority, int64_t closureUid, int BuildFlags) = 0;
		};

		using Blob = std::vector<char>;
		using BlobView = std::tuple<const void*, size_t>;

		struct BlobRef {
			std::shared_ptr<Blob> blob;
			struct ByIdentity {
				bool operator()(const BlobRef& lhs, const BlobRef& rhs) const;
			};
			struct ByValue {
				bool less(const void* lData, size_t lSz, const void* rData, size_t rSz) const;
				bool operator()(const BlobRef& lhs, const BlobRef& rhs) const;
				bool operator()(const BlobRef& lhs, const BlobView& rhs) const;
				bool operator()(const BlobView& lhs, const BlobRef& rhs) const;
			};
			BlobRef& operator=(const BlobView& rhs);
		};

		class Instance : public IObject {
			ClassRef myClass;
			krt_instance instance;
			void *closure;
		public:
			~Instance();
			Instance(ClassRef c, krt_instance instance, void *cls) :myClass(c), instance(instance), closure(cls) {}
			Instance(const Instance&) = delete;
			Instance& operator=(const Instance&) = delete;
			void Dispatch(int symIndex, const void*, size_t, void*) override;
			void UnsubscribeAll(ISubscriptionHost*) override;
			void Bind(int symIndex, const void* data) override;
			int GetSymbolIndex(const MethodKey&) override;
			ClassCode& Class() const { return *myClass; }
			size_t SizeOfOutput() const override { return (size_t)Class()->result_type_size; }
			
			void *Closure() {
				return closure;
			}

			void *Id() const {
				return Class()->var(instance, 0);
			}

			bool HasStreamClock() const {
				return myClass->hasStreamClock;
			}

			void EnumerateSymbols(const ObjectSymbolEnumeratorTy&) const override;
		};

		using InstanceMapTy = pcoll::hamt<void*, IObject::Ref>;
		
		struct MethodData {
			krt_process_call callback;
			void const** slot;
		};

		class Scheduler;
		class StreamSubject;

		class HierarchyBroadcaster : public IO::Broadcaster {
		protected:
			IO::IHierarchy* ioParent;
			std::unordered_set<std::string> stringStore;
		public:
			virtual IO::Subject::Ref MakeSubject(const MethodKey&) { return new IO::Subject; }
			HierarchyBroadcaster(IO::IHierarchy* ioParent) :ioParent(ioParent) { }
			void UnknownSubject(const Runtime::MethodKey&, const IO::ManagedRef&, krt_instance, krt_process_call, void const**) override;
		};
	}
}
