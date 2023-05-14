#pragma once

#include "json.h"
#include <string>
#include <unordered_map>

namespace lithe {
	namespace grammar {
		namespace kronos {
			struct op_t {
				int precedence;
				bool right_associative;
			};

			const std::unordered_map<std::string, op_t>& infix_mappings();

			lithe::rule parser(bool keep_comments = false);
			lithe::rule identifier();
			lithe::rule package_version();
			int istokenchar(int c);
#define TAG(s, name) extern const char *s; 
#include "kronos_tags.inc"
#undef TAG
		}
	}
}
