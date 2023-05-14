#include "UserErrors.h"
#include "LLVMModule.h"
#include "llvm/ADT/Triple.h"
#include "llvm/CodeGen/LinkAllAsmWriterComponents.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeWriter.h"

#include "llvm/Support/raw_os_ostream.h"

#include "LLVMCmdLine.h"

#include <fstream>

using namespace llvm;

// Returns the TargetMachine instance or zero if no triple is provided.
static TargetMachine* GetTargetMachine(Triple TheTriple, std::string mcpu, std::string march, std::string mfeats, llvm::TargetOptions options, llvm::CodeGenOpt::Level olvl) {
    std::string Error;
    const Target *TheTarget = TargetRegistry::lookupTarget(march, TheTriple,
                                                           Error);
    // Some modules don't specify a triple, and this is okay.
    if (!TheTarget) {
        return nullptr;
        return nullptr;
    }
    
    return TheTarget->createTargetMachine(TheTriple.getTriple( ),
                                          mcpu, mfeats,
                                          options, llvm::Reloc::PIC_, llvm::None,
                                          olvl);
}

TargetOptions GetTargetOptions(Kronos::BuildFlags flags) {
	TargetOptions Options;

	if ((flags & Kronos::StrictFloatingPoint) != 0) {
		Options.UnsafeFPMath = false;
		Options.NoInfsFPMath = false;
		Options.NoNaNsFPMath = false;
		Options.HonorSignDependentRoundingFPMathOption = true;
		Options.AllowFPOpFusion = FPOpFusion::Strict;
	} else {
		Options.UnsafeFPMath = true;
		Options.NoInfsFPMath = true;
		Options.NoNaNsFPMath = false; // comparisons produce QNaNs
		Options.HonorSignDependentRoundingFPMathOption = false;
		Options.AllowFPOpFusion = FPOpFusion::Fast;
	}

	Options.EnableIPRA = true;
	Options.DataSections = true;
	Options.FloatABIType = FloatABI::Default;

	return Options;
}

namespace K3 {
	namespace Backends {
		void LLVMOptimize(llvm::Module& m, llvm::CodeGenOpt::Level optLevel);

		void LLVM::AoT(const char *prefix, const char *fileType, std::ostream& writeToStream, Kronos::BuildFlags flags, const char* triple, const char *mcpu, const char *march, const char *mfeat) {
			llvm::SmallVector<char, 16384> sv;
			raw_svector_ostream SVOS(sv);

            if (CL::LlvmHeader().size()) {
                cppHeader.Open(CL::LlvmHeader(), prefix, GetArgumentType(), GetResultType());
            }

			std::string smcpu(mcpu), smtriple(triple), smarch(march);

			if (smtriple == "host") smtriple = llvm::sys::getDefaultTargetTriple();

			Build(flags);

			std::unique_ptr<llvm::Module> M;
			std::swap(M, GetModule());

			LLVMOptimize(*M, (llvm::CodeGenOpt::Level)CL::OptLevel());

			if (prefix) {
				for (auto &f : M->getFunctionList()) {
					if (f.getLinkage() == llvm::GlobalValue::ExternalLinkage &&
						f.isDeclaration() == false) {
						f.setName(prefix + f.getName());
					}
				}
			}
            
			string ftId(fileType);
			std::transform(ftId.begin(), ftId.end(), ftId.begin(), ::tolower);

            M->setTargetTriple(smtriple);
            
            InitializeAllTargets();
            InitializeAllAsmPrinters();
            InitializeAllTargetMCs();
            PassRegistry *Registry = PassRegistry::getPassRegistry();
            initializeCore(*Registry);
            initializeCodeGen(*Registry);
            initializeLoopStrengthReducePass(*Registry);
            initializeLowerIntrinsicsPass(*Registry);

            Triple TheTriple = Triple(M->getTargetTriple());

            SMDiagnostic Err;

            if (TheTriple.getTriple().empty())
                TheTriple.setTriple(sys::getDefaultTargetTriple());

            std::string Error;
            TargetOptions Options = GetTargetOptions(flags);

			if (smtriple.find("eabihf") != smtriple.npos) {
				// this seems to be not properly detected from triple, not sure why
				Options.FloatABIType = FloatABI::Hard;
			}

			
            std::unique_ptr<TargetMachine> target(GetTargetMachine(TheTriple, smcpu, smarch, mfeat, Options, (CodeGenOpt::Level)CL::OptLevel()));

            if (!target) {
                throw Error::RuntimeError(Error::InvalidAPIParameter, Error);
            }

            TargetMachine &Target = *target.get();

            legacy::PassManager PM;

            TargetLibraryInfoImpl TLII(TheTriple);
            PM.add(new TargetLibraryInfoWrapperPass(TLII));
			M->setDataLayout(target->createDataLayout());
			LLVMTargetMachine& LLVMTM = static_cast<LLVMTargetMachine&>(*target);
			MachineModuleInfo* MMI = new MachineModuleInfo(&LLVMTM);
            
            if (ftId == ".s" || ftId == ".asm" || ftId == ".obj" || ftId == ".o") {
                TargetMachine::CodeGenFileType CGFileType = TargetMachine::CodeGenFileType::CGFT_AssemblyFile;
                if (ftId == ".obj" || ftId == ".o") CGFileType = TargetMachine::CodeGenFileType::CGFT_ObjectFile;
                
                if (Target.addPassesToEmitFile(PM, SVOS, CGFileType, true, MMI)) {
                    throw Error::RuntimeError(Error::InvalidAPIParameter, "Target does not support generation of this file type");
                }
            }
            
            PM.run(*M);
                
            if (ftId == ".ir" || ftId == ".ll") {
                raw_os_ostream out(writeToStream);
                out << *M;
            }
            else if (ftId == ".bc") {
                raw_os_ostream out(writeToStream);
                WriteBitcodeToFile(M.get(), out);
            }

            auto str = SVOS.str();
			writeToStream.write(str.data(), str.size());
			writeToStream.flush();

            cppHeader.Close();
		}
	}
}
