#pragma once

#include "common.h"
#include <string>

namespace lithe {
	namespace grammar {
		namespace json {
			static inline rule number() {
				return common::numeric();
			}

			rule string();

			std::string decode_string(node);
			std::string encode_string(std::string);
		}
	}
}
