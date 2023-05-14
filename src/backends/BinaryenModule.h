#pragma once
#include "kronosrt.h"
#include "GenericModule.h"
#include "binaryen-c.h"
#include "Reactive.h"
#include "BinaryenCompiler.h"
#include <list>

namespace K3 {
	namespace Backends {
		using K3::Reactive::DriverSet;

		struct BinaryenSpec : public CodeGenModule, public BinaryenModule {
			BinaryenDataSegment dataSegment;

			CTRef AST;

			BinaryenFunction CompilePass(const std::string& name, Backends::BuilderPass passCategory, const DriverSet& drivers);
			BinaryenFunction CompilePass(const std::string& name, Backends::BuilderPass passCategory, const DriverSet& drivers, const CounterIndiceSet& indices);

#define T(SYM) TypeTy SYM ## Ty() { return BinaryenType ## SYM(); }
			T(Int32) T(Int64) T(Float32) T(Float64)
#undef T	
			BinaryenSpec(BinaryenModuleRef M, CTRef AST, const Type& arg, const Type& res) :CodeGenModule(arg, res), BinaryenModule(M), AST(AST) {
				BinaryenAddGlobal(M, STACK_PTR, BinaryenTypeInt32(), true, BinaryenConst(M, BinaryenLiteralInt32(0)));

				BinaryenType params[] = {
					BinaryenTypeInt32(),
					BinaryenTypeInt32()
				};

				BinaryenAddFunctionImport(M, "alloca", "builtin", "alloca",
					BinaryenTypeCreate(params, 2), BinaryenTypeInt32());
			}

			~BinaryenSpec() {
				BinaryenModuleDispose(M);
			}

			void AoT(const char *prefix, const char *fileType, std::ostream& writeToStream, Kronos::BuildFlags flags, const char* triple, const char *mcpu, const char *march, const char *mfeat) override;

			krt_class* JIT(Kronos::BuildFlags flags) override { KRONOS_UNREACHABLE; }

			virtual FunctionTy GetActivation(const std::string& nameTemplate, CTRef graph, const Type& signature) = 0;
		};

		using Binaryen = GenericCodeGen<BinaryenSpec>;
	}
}
