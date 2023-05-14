#pragma once

#include "inout.h"
#include "kronos.h"

namespace Kronos {
	namespace IO {
		namespace o2 {
			std::unique_ptr<IHierarchy> Setup(IRegistry&, IConfigurationDelegate*);
		}
	}
}
