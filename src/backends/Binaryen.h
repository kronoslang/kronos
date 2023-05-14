#pragma once

#include "kronos_abi.h"
#include "kronosrt.h"
#include <ostream>

namespace K3 {
	namespace Backends {
		int BinaryenAoT(
			const char* prefix,
			const char* fileType,
			std::ostream& object,
			const char* engine,
			const Kronos::ITypedGraph* itg,
			const char* triple,
			const char* mcpu,
			const char* march,
			const char* targetFeatures,
			Kronos::BuildFlags flags);

		class BinaryenTransform;
	}
}