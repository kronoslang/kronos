#pragma once

#include "common/ssstring.h"
#include "common/Ref.h"
#include "common/Reflect.h"
#include "common/CallStack.h"
#include "common/Err.h"

#include "NodeBases.h"
#include "Typed.h"
#include "kronos_abi.h"
#include "Parser.h"
#include <unordered_set>
#include <map>
#include <set>
#include <tuple>
#include <mutex>
#include <list>
#include <deque>
#include <vector>
#include <algorithm>

namespace K3 {
	class ManagedObject : public RefCounting, REFLECTING_CLASS {
	};

	struct SpecializationKey : public std::tuple<Type,Type> {
		SpecializationKey(Type genericGraph, Type argumentType):tuple(genericGraph,argumentType) {}
		const Type& GetGraph() const { return std::get<0>(*this); }
		const Type& GetArgument() const { return std::get<1>(*this); }
		struct Hasher{
			size_t operator()(const SpecializationKey& k) const {return k.GetGraph().GetHash() ^ k.GetArgument().GetHash(); }
		};
	};

	class SpecializationCache : public std::unordered_map<SpecializationKey,std::tuple<Graph<Nodes::Typed>,Type,bool,bool>,SpecializationKey::Hasher>, public RefCounting {
	};
    
    struct Asset {
        Asset();
        std::unique_ptr<void, void(*)(void*)> memory;
        Type type;
    };

	class TLS {
		size_t curUID;
		std::map<const std::string, TypeDescriptor> usertypes;
		std::unordered_set<Type> typeKeys;
		std::unordered_map<const void*, Type> typeAssoc;
		std::unordered_set<std::string> resolutionTrace;

		std::unordered_set<std::string> Strings;
		std::unordered_map<const char*,Ref<ManagedObject>> ManagedObjectStore;
		std::unordered_map<std::string, std::function<void(bool, const Type&, std::int64_t)>> specializationCallbacks;
		std::function<const char*(const char*, const char*, const char*)> modulePathResolver;
		std::function<void*(const char* url, Type&)> assetLoader;

		Ref<SpecializationCache> currentCache;

#ifndef KRONOS_NO_STACK_EXTENDER
		std::vector<std::unique_ptr<Stack>> virtualStack;
		size_t virtualStackFrame = 0;
#endif

		std::unordered_map<std::string, Asset> staticAssets;
		std::string compilerTraceFilter;
	protected:
		Kronos::BuildFlags flags;
		void ClearResolutionTrace() { resolutionTrace.clear(); }
		void DidResolve(const std::string& str) { resolutionTrace.emplace(str); }
		std::unordered_set<std::string> GetResolutionTrace() const { return resolutionTrace; }
		std::unordered_set<std::string> DrainRecentChanges();
		Parser::parser_state_t REPLState;
		Parser::Repository2 codebase;
		void InitializeDefaultResolver();
	public:

		void SetCompilerDebugTraceFilter(const char *flt) { compilerTraceFilter = flt; }
#ifndef NDEBUG
		bool ShouldTrace(const char *context, const char *label);
#endif

		TLS(Kronos::ModulePathResolver res, void *user);
		TLS(const TLS&) = delete;
		TLS& operator=(const TLS&) = delete;
        
		TypeDescriptor* GetTypeDescriptor(const std::string& key);

		static void SetCurrentInstance(TLS* instance);
		static TLS* GetCurrentInstance();

		const char *Memoize(const std::string& str) {
			return Strings.insert(str).first->c_str();
		}

		const char *ResolveModulePath(const char *package, const char* file, const char *version) const {
			return modulePathResolver(package, file, version);
		}

		void SetModuleResolver(Kronos::ModulePathResolver resolver, void *user) {
			modulePathResolver = [resolver, user](const char *package, const char *file, const char *ver) {
				return resolver(package, file, ver, user);
			};
		}

		const void* Memoize(const Type& t);
		Type Recall(const void* uid);

		void RegisterSpecilizationCallback(const std::string& signature, std::function<void(bool, const Type&, std::int64_t)> cb) {
			specializationCallbacks[signature] = cb;
		}

		void SetAssetLoader(std::function<void*(const char*, Type&)> al) {
			assetLoader = al;
		}

		void SpecializationCallback(bool hasDiagnostics, const std::string& signature, const Type& t, std::int64_t typeUid) {
			auto f = specializationCallbacks.find(signature);
			if (f != specializationCallbacks.end()) {
				f->second(hasDiagnostics, t, typeUid);
			}
		}

		size_t GetUID();

		Err<void> Initialize();

		Ref<ManagedObject> Get(const char *key) { return ManagedObjectStore[key]; }
		void Set(const char *key, Ref<ManagedObject> mo) { ManagedObjectStore[key] = mo; }

		TLS* SetForThisThread() { TLS* old = GetCurrentInstance(); SetCurrentInstance(this); return old; }

		Ref<SpecializationCache> GetSpecializationCache() { return currentCache; }
		void SetSpecializationCache(Ref<SpecializationCache> c) { currentCache = move(c); }

		static Nodes::CGRef ResolveSymbol(const char* qualifiedName);
		static void RebindSymbol(const char *qualifiedName, Nodes::CGRef temporaryBinding);
		static std::string GetModuleAndLineNumberText(const char *sourcePos, std::string* showLine);
		static Type GetRepositoryMetadata(const std::string& prefix);
		static Kronos::BuildFlags GetCurrentFlags();

        Asset& GetAsset(const std::string& name);

		template <typename FN> static auto WithNewStack(FN&& f) {
#ifdef KRONOS_NO_STACK_EXTENDER
			return f();
#else
			auto self = GetCurrentInstance();
			if (self->virtualStackFrame) {
				// is there enough space?
				if (self->virtualStack[self->virtualStackFrame - 1]->StackAvail() > 0x10000) {
					return f();
				}
			}

			if (self->virtualStackFrame >= self->virtualStack.size()) {
				if (self->virtualStack.size() > 10000) {
					throw std::runtime_error("Call depth of 10000 exceeded; infinite recursion?");
				}
				self->virtualStack.emplace_back(std::make_unique<Stack>(0x1000000));
			}
			decltype(f()) ret_val;
			self->virtualStack[self->virtualStackFrame]->Execute([&f, &ret_val, self]() mutable {
				self->virtualStackFrame++;
			#if _HAS_EXCEPTIONS
				try {
			#endif
					ret_val = f();
					self->virtualStackFrame--;
			#if _HAS_EXCEPTIONS
			} catch (...) {
					self->virtualStackFrame--;
					throw;
			}
			#endif
			});
			return ret_val;
#endif
		}
	};

	struct ScopedContext{
		TLS *newContext;
		TLS *oldContext;
		ScopedContext(TLS& c) :newContext(&c), oldContext(K3::TLS::GetCurrentInstance( )) { K3::TLS::SetCurrentInstance(&c); }
		~ScopedContext() { assert(K3::TLS::GetCurrentInstance() == newContext);  K3::TLS::SetCurrentInstance(oldContext); }
	};
}

/*
"C:/Users/vnorilo/Downloads/eva - tracks/Eva - Ilta - 04 - Nocturne.wav"
*/
