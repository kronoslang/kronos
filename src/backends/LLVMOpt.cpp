#pragma warning(disable: 4146 4267 4244)
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/LinkAllPasses.h"
#include <memory>
#include <iostream>
#include <chrono>

namespace K3 {
	namespace Backends {
		using namespace llvm;
		static void Dump(const char* label, Module& mod) {
			std::string str;
			llvm::raw_string_ostream os(str);
			os << mod;
			std::clog << "\n -- " << label << "\n" << str;
		}

		void LLVMOptimize(Module& mod, llvm::CodeGenOpt::Level lvl) {
			legacy::PassManager mpm;
			auto start = std::chrono::high_resolution_clock::now();

			if (lvl) {
				legacy::FunctionPassManager fpm(&mod);
				for (auto pass : {
					createCFGSimplificationPass()
					,createSROAPass()
					,createEarlyCSEPass()
					,createConstantPropagationPass()
					,createDeadCodeEliminationPass()
				}) fpm.add(pass);

				for (auto &fn : mod) fpm.run(fn);

				//Dump("per-function", mod);

				// alias analysis and target library info
				mpm.add(createMergeFunctionsPass());
				switch (lvl) {
				case CodeGenOpt::Level::None: break;
				case CodeGenOpt::Level::Less:
					mpm.add(createFunctionInliningPass(10));
					break;
				case CodeGenOpt::Level::Default:
					mpm.add(createFunctionInliningPass(40));
					break;
				case CodeGenOpt::Level::Aggressive:
					mpm.add(createFunctionInliningPass(100));
					break;
				}
				mpm.add(createScopedNoAliasAAWrapperPass());

				mpm.add(createIPSCCPPass());
				mpm.add(createGlobalOptimizerPass());
				mpm.add(createPromoteMemoryToRegisterPass());
				mpm.add(createDeadArgEliminationPass());
				
				mpm.add(createInstructionCombiningPass(lvl > 2));

				//Dump("early passes", mod);

				mpm.add(createCFGSimplificationPass());
//				mpm.add(createFunctionInliningPass(lvl, 1, false));

				//Dump("inlining", mod);
				mpm.add(createSROAPass());
				mpm.add(createEarlyCSEPass());
				
				mpm.add(createInstructionCombiningPass(lvl > 2));
				mpm.add(createLibCallsShrinkWrapPass());

				mpm.add(createTailCallEliminationPass());
				mpm.add(createCFGSimplificationPass());
				mpm.add(createReassociatePass());
				mpm.add(createLoopRotatePass(-1));
				mpm.add(createLICMPass());

				mpm.add(createCFGSimplificationPass());
				mpm.add(createInstructionCombiningPass(lvl > 2));
				mpm.add(createIndVarSimplifyPass());
				mpm.add(createLoopInterchangePass());

				if (lvl > 2) {
					mpm.add(createLoopRotatePass(-1));
					mpm.add(createLoopVectorizePass());
					mpm.add(createLoopLoadEliminationPass());
				}

				//Dump("loop vectorize", mod);

				if (lvl > 1) {
					mpm.add(createEarlyCSEPass());
					mpm.add(createCorrelatedValuePropagationPass());
					mpm.add(createCFGSimplificationPass(1, true, true, false, true));
					mpm.add(createLoopUnrollPass(lvl));

					mpm.add(createInstructionCombiningPass(lvl > 2));
				}

				mpm.add(createGlobalDCEPass());
				mpm.add(createConstantMergePass());

				//Dump("late passes", mod);

				mpm.add(createMergeFunctionsPass());
				mpm.add(createLoopSinkPass());
				mpm.add(createInstructionSimplifierPass());
				mpm.add(createDivRemPairsPass());
				mpm.add(createCFGSimplificationPass());

			} else {
				mpm.add(createMergeFunctionsPass());
			}
			auto mpmstart = std::chrono::high_resolution_clock::now();
			mpm.run(mod);
			auto end = std::chrono::high_resolution_clock::now();
            
            (void)mpmstart; (void)end; (void)start;

/*			auto fpmtime = std::chrono::duration_cast<std::chrono::milliseconds>(mpmstart - start);
			auto mpmtime = std::chrono::duration_cast<std::chrono::milliseconds>(end - mpmstart);
			auto totaltime = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

			std::clog << "Opt: " << totaltime.count() << "\nFPM: " << fpmtime.count() << "\nMPM: " << mpmtime.count() << "\n";*/
			
		}
	}
}
