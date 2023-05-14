#include "common.h"
namespace lithe {
	namespace grammar {
		namespace common {
			rule digits(int at_least) {
				return characters("decimal", isdigit, false, at_least);
			}

			rule sign() {
				static rule r = characters("sign", "+-");
				return r;
			}

			rule unsigned_integer() {
				static rule r = (characters("non-zero decimal", "123456789", false, 1)
								 << digits(0))
					| T("0");
				return r;
			}

			rule signed_integer() {
				static rule r = O(sign()) << unsigned_integer();
				return r;
			}

			rule whitespace() {
				static rule r = characters("whitespace", isspace);
				return r;
			}

			rule fraction() {
				static rule r = T(".") << digits();
				return r;
			}

			rule floating_point() {
				static rule r = O(signed_integer()) << fraction();
				return r;
			}

			rule exponent() {
				static rule r = characters("exponent", "eE") << signed_integer();
				return r;
			}

			rule scientific() {
				static rule r = signed_integer() << O(fraction()) << exponent();
				return r;
			}

			rule numeric() {
				static rule r =
					((signed_integer() << O(fraction())) | fraction())
					<< O(exponent());
				return r;
			}
		}
	}
}