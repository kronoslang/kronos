#pragma once

#include "JsonRPCEndpoint.h"
#include <memory>

namespace Kronos {
	namespace LanguageServer {
		using picojson::object;
		using picojson::value;

		JsonRPC::IEndpoint::Ref Make(const char *repository, const char *version);		
	}
}
