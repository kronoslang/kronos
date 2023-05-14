#include "common.h"
#include <cassert>

namespace lithe {
	namespace grammar {
		namespace json {
			rule string() {
				static rule r = I("\"") << repeat(
					characters("string character", "\n\\\"", true)
					| (T("\\") << characters("control character", "\\\"/bfnrt", false, 1, 1))
					| (T("\\u") << characters("hexadecimal", isxdigit, false, 4, 4)))
					<< I("\"");
				return r;
			}

			std::string decode_string(node n) {
				std::string source = n[0].get_string();
				std::string result;
				for (size_t pos = 0;;) {
					size_t beg = pos;
					pos = source.find_first_of('\\', pos);
					if (pos == source.npos) {
						result += source.substr(beg);
						break;
					} else {
						result += source.substr(beg, pos - beg);
						switch (source[++pos]) {
							case 'n': result += "\n"; break;
							case 'r': result += "\r"; break;
							case 't': result += "\t"; break;
							case '\\': result += "\\"; break;
							case '\"': result += "\""; break;
							case 'u': assert(0 && "todo");
							default:break;
						}
						++pos;
					}
				}
				return result;
			}

			std::string encode_string(std::string source) {
				std::string result = "\"";
				for (size_t i = 0;i < source.size();++i) {
					switch (source[i]) {
					case '\n': result += "\\n"; break;
					case '\r': result += "\\r"; break;
					case '\t': result += "\\t"; break;
					case '\\': result += "\\\\"; break;
					case '\"': result += "\\\""; break;
					default:
						if (source[i] < 32) {
							static const char xdig[16] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };
							result += "\\u00";
							result.push_back(xdig[(source[i] >> 4) & 0xf]);
							result.push_back(xdig[(source[i] & 0xf)]);
						} else {
							result += source[i];
						}
						break;
					}
				}
				result.push_back('\"');
				return result;
			}

		}
	}
}