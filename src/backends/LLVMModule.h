#pragma once
#include "CodeGenModule.h"
#include "CodeGenCompiler.h"
#pragma warning (disable: 4146)
#include "LLVMCompiler.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/MCJIT.h"


namespace K3 {
	namespace Backends {
		class LLVM : public CodeGenModule {
			llvm::LLVMContext& Context; // must be before Module M
			
			LLVM(const LLVM&);
			LLVM& operator=(const LLVM&) = delete;
			std::unordered_map<Reactive::DriverSet, llvm::Function*, dshash> activations;

			llvm::Function* GetPartialSubActivation(const std::string& name, CTRef graph, Reactive::DriverSet& drivers, CounterIndiceSet& indiceSet, int longCounterTreshold);
			llvm::Function* GetSubActivation(const std::string& name, CTRef graph, Reactive::DriverSet& drivers, CounterIndiceSet& indiceSet, int longCounterTreshold);
			llvm::Function * CombineSubActivations(const std::string & name, const std::vector<llvm::Function*>& superClockFrames);
			llvm::Function* GetActivation(const std::string& nameTemplate, CTRef graph, const Type& signature, llvm::Function *sizeOfStateStub, llvm::Function *sizeOfStub);
			int firstCounterBitMaskIndex = 0;
		protected:
			std::unordered_map<Type, llvm::Function*> inputCall;
			std::unique_ptr<llvm::Module> M;
			void MakeIR(Kronos::BuildFlags);
			void Optimize(int level, std::string mcpu, std::string march, std::string mfeat);
		public:
			LLVM(CTRef AST, const Type& argType, const Type& resType);
			~LLVM();

			llvm::LLVMContext& GetContext();
			std::unique_ptr<llvm::Module>& GetModule() { return M; }
			void Build(Kronos::BuildFlags flags);
			krt_class* JIT(Kronos::BuildFlags flags);
			virtual void AoT(const char *prefix, const char *fileType, std::ostream& writeToStream, Kronos::BuildFlags flags, const char* triple, const char *mcpu, const char *march, const char *mfeat);
		};
	};
}
