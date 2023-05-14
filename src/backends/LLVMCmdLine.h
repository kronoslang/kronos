#pragma once

#include "driver/CmdLineOpts.h"

namespace CL {
	extern CmdLine::Option<int> OptLevel;
    extern CmdLine::Option<string> LlvmHeader;
};

