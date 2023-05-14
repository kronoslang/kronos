#pragma once

#include <string>
#include <unordered_set>
#include <map>
#include <tuple>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <mutex>
#include <ostream>

#include "common/Ref.h"
#include "backends/SideEffectCompiler.h"
#include "Reactive.h"
#include "MemoizeTransform.h"
#include "kronosrt.h"

#undef max

namespace K3 {
	namespace Nodes{
		class SpecializationDiagnostic;
	};

	class Module : public RefCounting, 
		public Reactive::IDelegate,
		public Backends::IInstanceSymbolTable,
		public Reactive::DelegateMemoize,
		public Backends::InstanceMemoize {
		std::mutex symbolTableLock;
		unsigned freeSymbolIndex;
		int numSignalMaskBits;
		std::unordered_map<const void*, CRRef> globalReactivityTable;
	protected:
		Ref<MemoryRegion> buildMemory;
		Type outputReactivity;
		Type result;
		Type arg;
		Type GetOutputReactivity() const { return outputReactivity; }
		int GetBitmaskSize() const { return ((numSignalMaskBits + 31) / 32) * 4; }

		struct GlobalVarData {
			const void *uid;
			Type data;
			Nodes::GlobalVarType varType;
			std::pair<int, int> relativeRate;
			Type clock;
		};

		unsigned GetNumSymbols() const { return freeSymbolIndex; }
		std::unordered_map<const void*,unsigned> globalSymbolTable;
		std::unordered_map<const Type, GlobalVarData> globalKeyTable;
		std::unordered_set<Type> drivers;
		friend class Ref<Module>;
		Graph<Typed> intermediateAST;
		const Reactive::Node *initializer;
		int OrdinalCompare(const Type& lhs, const Type& rhs) const;
		void RegisterInput(const Type& inputId, const Type& dataType, size_t indexInInstance) { }
		void RegisterDriver(const Type& clockId, double clockMultiplier, double clockDivider) { drivers.insert(clockId); }
		void RegisterSignalMaskSlot(int bitPos) { numSignalMaskBits = std::max(bitPos,numSignalMaskBits); }
		const Reactive::Node* GetGlobalVariableReactivity(const void *uid);
		const Type& GetResultType() {return result;}
		const Type& GetArgumentType() {return arg;}
		std::vector<std::string> dependentSymbols;
	public:
		Module(const Type& argumentType, const Type& resultType);

		int GetArgumentIndex() { return GetIndex(this); }

		virtual void StandardBuild(CTRef AST, const Type& argument, const Type& result);

		virtual Graph<Generic> BeforeSpecialization(CGRef body) { return body; }
		virtual Graph<Typed> BeforeReactiveAnalysis(CTRef body) { return body; }

		int GetNumSignalMaskBits() const { return numSignalMaskBits; }

		virtual const Reactive::Node* GetInitializerReactivity() const {return initializer;}
		unsigned GetIndex(const void *uid);
		unsigned GetIndex();
		void RegisterExternalVariable(const Type& key, const Type& dataType, const void *uid, GlobalVarType varType, std::pair<int,int> sampleRateMultiplier, const Type& clock);
		const void* CanonicalizeMaskUID(const void *uid) {return (const void*)(intptr_t)GetIndex(uid);}
		void SetGlobalVariableReactivity(const void* uid, const Reactive::Node *r);

		virtual krt_class* JIT(Kronos::BuildFlags flags) = 0;
		virtual void AoT(const char *prefix, const char *fileType, std::ostream& writeToStream, Kronos::BuildFlags flags, const char * mtriple, const char *mcpu, const char *march, const char *mfeat) = 0;
	};
};
