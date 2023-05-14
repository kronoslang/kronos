#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <cassert>

#include "PlatformUtils.h"

extern "C" void Evaluate(void *state, const void *input, void *output);
extern "C" std::uint64_t GetSize();
extern "C" const std::uint64_t ResultNumBytes;
extern "C" const char ResultTypeFormatString[];

template <typename T> void kout(std::ostream& o, const void*& data) {
	o << *(T*)data;
	data = (char *)data + sizeof(T);
}

void kprintf(std::ostream& o, const char *fmt, const void *data) {
	while (*fmt) {
		auto c = *fmt++;
		if (c == '%') {
			c = *fmt++;
			switch (c) {
			case 'f': kout<float>(o, data); break;
			case 'd': kout<double>(o, data); break;
			case 'i': kout<std::int32_t>(o, data); break;
			case 'q': kout<std::int64_t>(o, data); break;
			case '%': o << '%'; break;
			default:
				assert(0 && "Bad format string");
			}
		} else o << c;
	}
}

int main(int argn, const char *argv[]) {
	std::vector<char> result(ResultNumBytes);
	std::vector<char> state(GetSize());
	Evaluate(state.data(), nullptr, result.data());
	if (argn > 1) {
		std::ifstream reference(utf8filename(argv[1]));
		std::stringstream resultStream, refStream;
		kprintf(resultStream, ResultTypeFormatString, result.data());
		resultStream << std::endl;
		refStream << reference.rdbuf();
		if (refStream.str() != resultStream.str()) {
			std::cerr << "<==========================\n" << refStream.str() << "---------------------------\n" << resultStream.str() << "==========================>\n";
			return -1;
		} else {
			return 0;
		}
	} else {
		kprintf(std::cout, ResultTypeFormatString, result.data());
	}
	return 0;
}