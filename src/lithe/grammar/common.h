#pragma once

#include <cctype>
#include "../lithe.h"

namespace lithe {
	namespace grammar {
		namespace common {
			rule digits(int at_least = 1);
			rule sign();
			rule unsigned_integer();
			rule signed_integer();
			rule whitespace();
			rule fraction();
			rule floating_point();
			rule exponent();
			rule scientific();
			rule numeric();
		}
	}
}