#pragma once
#include "Transform.h"
#include "SideEffectCompiler.h"
#include "LLVMSignal.h"

#include <unordered_map>

#ifdef _MSC_VER
#pragma warning(disable:4244)
#pragma warning(disable:4267)
#pragma warning(disable:4800)
#pragma warning(disable:4146)
#endif

#include "CallGraphAnalysis.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IRBuilder.h"

#include "CodeGenModule.h"
#include "CodeGenCompiler.h"

namespace {
	using namespace K3;

	struct FunctionKey : std::tuple<llvm::FunctionType*,Graph<const Typed>>{
		FunctionKey(const Graph<const Typed>& key, llvm::FunctionType* fty):tuple(fty,key) {}
		llvm::FunctionType* GetFunctionTy() const { return std::get<0>(*this); }
		Graph<const Typed> GetGraph() const { return std::get<1>(*this); }
		size_t GetHash() const {return GetGraph()->GetHash(true) ^ (size_t)GetFunctionTy();}
		bool operator==(const FunctionKey& rhs) const { return (CTRef)GetGraph() == (CTRef)rhs.GetGraph(); }
	};

	typedef std::unordered_map<FunctionKey,llvm::Function*> FunctionCache;
}
namespace std{
	template <> struct hash<FunctionKey>{size_t operator()(const FunctionKey& k) const {return k.GetHash();}};
};

namespace K3 {
	namespace Backends{
		using namespace llvm;

		class ILLVMCompilationPass : public ICompilationPass {
		public:
			virtual llvm::LLVMContext& GetContext() = 0;
			virtual const std::unique_ptr<llvm::Module>& GetModule() = 0;
			virtual llvm::Function* GetMemoized(CTRef source, llvm::FunctionType* fty) = 0;
			virtual void Memoize(CTRef source, llvm::Function* func) = 0;
		};


		typedef std::unordered_map<int,llvm::Value*> SignalMaskMapTy;

		class LLVMTransform : public CachedTransform<Typed,LLVMValue,true>, public CodeGenTransformBase {
			friend class K3::Nodes::ReactiveOperators::ClockEdge;
			ILLVMCompilationPass& compilation;

			Function *function;
//			BasicBlock *entry;
			BasicBlock *bb;
			map<size_t,llvm::Value*> AllocatedBuffers;
			IRBuilder<> builder;
			std::vector<llvm::Value*> funParam;

			llvm::Value* GenerateNodeActiveFlag(IRBuilder<>&, ActivityMaskVector* avm);
			LLVMValue operate(CTRef);
			ActivityMaskVector *currentActivityMask;
			void ProcessPendingMergeBlock(llvm::BasicBlock *&pendingMergeBlock, llvm::BasicBlock *passiveBlock, LLVMTransform& subroutineBuilder, vector<CTRef> &skippedNodes);
			std::unordered_map<int, llvm::Value*> signalMaskBits;
		public:
			LLVMTransform(CTRef root, ILLVMCompilationPass& pass, Function*);
			LLVMTransform(ILLVMCompilationPass&);
			LLVMTransform(const LLVMTransform&);

			LLVMValue operator()(CTRef n) {return CachedTransform::operator()(n);}
			LLVMValue operator()(CTRef n, ActivityMaskVector* avm) {
				std::swap(currentActivityMask, avm);
				auto tmp = (*this)(n);
				std::swap(currentActivityMask, avm);
				return tmp;
			}

			ILLVMCompilationPass& GetCompilationPass() {return compilation;}
			IRBuilder<>& GetBuilder() {return builder;}
			LLVMContext& GetContext() {return compilation.GetContext();}
			const std::unique_ptr<llvm::Module>& GetModule() {return compilation.GetModule();}
			llvm::Type* GetType(const K3::Type&);
			llvm::Value* GetParameter(size_t ID);
			llvm::Value* GetSequenceCounter();

			llvm::Function* BuildSubroutineCore(const char *label, CTRef body, const std::vector<llvm::Type*>& paramTypes);
			llvm::Function* BuildSubroutine(const char *label, CTRef body, const std::vector<llvm::Type*>& paramTypes);
			llvm::Function* GetFunction() { return function; }

			llvm::Value* Alloca(size_t BufferGUID, llvm::Value* size, size_t alignment);
		};
	}
};