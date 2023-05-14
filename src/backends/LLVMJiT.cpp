#include "LLVMModule.h"

#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/TargetRegistry.h"

#include "LLVMCmdLine.h"

#define DUMP_JIT_IR 0
//#define DUMP_JIT_GENERATED

namespace CL {
    extern CmdLine::Option<int> OptLevel;
}

llvm::TargetOptions GetTargetOptions(Kronos::BuildFlags flags);

namespace K3 {
    namespace Backends {

        static void Dump(const char* label, llvm::Module& mod) {
            std::string str;
            llvm::raw_string_ostream os(str);
            os << mod;
            std::clog << "\n -- " << label << "\n" << str;
        }
        
        void LLVMOptimize(llvm::Module& m, llvm::CodeGenOpt::Level optLevel);

        krt_class* LLVM::JIT(Kronos::BuildFlags flags) {
            using namespace llvm;
            
            if (!GetModule()) return nullptr;
            Build(flags);

#ifdef DUMP_JIT_GENERATED
			{
				std::string irString;
				raw_string_ostream os(irString);
				os << *GetModule();
				std::clog << irString;
			}
#endif
            
            InitializeNativeTarget();
            LLVMInitializeNativeAsmPrinter();
            LLVMInitializeNativeAsmParser();
            
            std::unique_ptr<llvm::Module> consumeModule;
            std::swap(consumeModule, GetModule());
            
            consumeModule->setTargetTriple(llvm::sys::getProcessTriple());
			LLVMOptimize(*consumeModule, (CodeGenOpt::Level)CL::OptLevel());

#if DUMP_JIT_IR
			Dump("jit", *consumeModule);
#endif // DUMP_JIT_IR

            EngineBuilder builder(std::move(consumeModule));
			
            std::string builderError;
			builder.setErrorStr(&builderError);                        
            builder.setEngineKind(EngineKind::JIT);
            auto RTDyldMM = new SectionMemoryManager();
            builder.setMCJITMemoryManager(std::unique_ptr<RTDyldMemoryManager>(RTDyldMM));

			TargetOptions opts = GetTargetOptions(flags);
            
			builder.setTargetOptions(opts);
            
            switch(CL::OptLevel()) {
                default: builder.setOptLevel(CodeGenOpt::None); break;
                case 1: builder.setOptLevel(CodeGenOpt::Less); break;
                case 2: builder.setOptLevel(CodeGenOpt::Default); break;
                case 3: builder.setOptLevel(CodeGenOpt::Aggressive); break;
            }
            
            auto jit = builder.create();
            if (!jit) {
                throw std::runtime_error("LLVM Execution Engine error: " + builderError);
            }
            
        #ifdef NDEBUG
            jit->setVerifyModules(false);
        #else
            jit->setVerifyModules(true);
        #endif
            jit->DisableLazyCompilation(true);
            jit->finalizeObject();
            
            auto jitClass = (krt_class*)jit->getGlobalValueAddress("Class");
            jitClass->pimpl = jit;
            jitClass->dispose_class = [](struct krt_class *c) {
                auto ee = (llvm::ExecutionEngine*)c->pimpl;
                delete ee;
            };
            
            RTDyldMM->invalidateInstructionCache();
            
            return jitClass;
        }
    }
}
