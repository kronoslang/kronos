#include "WaveCoreModule.h"
#include "Parser.h"
#include "CallGraphAnalysis.h"
#include "DriverSignature.h"

namespace K3 {
	Type GetResultReactivity(CRRef node);
	namespace Backends {
		WaveCore::WaveCore(CTRef AST, const Type& argTy, const Type& resTy) :LLVM(AST, argTy, resTy) {
		}
	}
}