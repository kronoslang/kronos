#pragma once

#include "pcoll/util.h"
#include "kronosrt.h"
#include <cstring>
#include <functional>

namespace Kronos {
	namespace Runtime {
		struct MethodKey {
			const char* name;
			const char* signature;
			struct Hash {
				size_t operator()(const MethodKey&) const;
			};
			bool operator==(const MethodKey&) const;
			MethodKey(const char *n = nullptr, const char *s = nullptr) :name(n), signature(s) {
				if (signature && !strcmp(signature, "%0")) signature = nullptr;
			}
			MethodKey(const krt_sym& sym) :MethodKey(sym.sym, sym.type_descriptor) {
			}
		};

		struct DisposableReferenceCounted : public pcoll::detail::reference_counted<pcoll::detail::multi_threaded>, public pcoll::detail::disposable {
		};

		using ObjectSymbolEnumeratorTy = std::function<void(int, const MethodKey&)>;

		class ISubscriptionHost {
		public:
			virtual void Unsubscribe(const char *sym, const char *sig, void* instance) = 0;
		};


		class IObject : public DisposableReferenceCounted {
		public:
			virtual void Dispatch(int symIdx, const void* arg, size_t argSz, void* result) = 0;
			virtual void Bind(int symIndex, const void* data) = 0;
			virtual int GetSymbolIndex(const MethodKey&) = 0;
			virtual bool HasStreamClock() const { return false; }
			virtual size_t SizeOfOutput() const = 0;
			virtual void *Id() const = 0;
			virtual void UnsubscribeAll(ISubscriptionHost*) = 0;
			virtual void EnumerateSymbols(const ObjectSymbolEnumeratorTy&) const = 0;
			using Ref = pcoll::detail::ref<IObject>;
		};
	}
}