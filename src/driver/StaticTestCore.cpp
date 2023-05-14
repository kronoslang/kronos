#include <cfloat>
#include <cassert>
#include <cstdio>
#ifdef WIN32
#include <io.h>
#else 
#include <unistd.h>
#endif
#include <cstring>
#include <cinttypes>

#include "picojson.h"
#include "CompareTestResultJSON.h"

#include "common/PlatformUtils.h"
#include "kronosrt.h"

#if !defined(FLT_DECIMAL_DIG) || !(defined(DBL_DECIMAL_DIG))
#undef FLT_DECIMAL_DIG
#undef DBL_DECIMAL_DIG
#include <limits>
#define FLT_DECIMAL_DIG std::numeric_limits<float>::max_digits10
#define DBL_DECIMAL_DIG std::numeric_limits<double>::max_digits10
#endif

extern "C" void Evaluate(void *state, const void *input, void *output);
extern "C" std::uint64_t GetSize();
extern "C" krt_class* GetClassData();
extern "C" const std::uint64_t ResultNumBytes;
extern "C" const char ResultTypeFormatString[];

//using bytestream_t = std::vector<char>;

struct bytestream_t {
	char *buf;
	int sz, cap;
	bytestream_t():cap(32),sz(0) { buf = new char[cap]; }
	~bytestream_t() { delete[] buf; }
	bytestream_t(const bytestream_t&) = delete;
	bytestream_t& operator=(const bytestream_t&) = delete;

	void push_back(char byte) {
		if (sz == cap) {
			cap *= 2;
			auto new_buf = new char[cap];
			memcpy(new_buf, buf, sz);
			delete[] buf;
			buf = new_buf;
		}
		buf[sz++] = byte;
	}

	char& back() { return buf[sz - 1]; }
	char *data() { return buf; }
	char& operator[](int i) { return buf[i]; }
	size_t size() { return sz; }
};

const char* kprintf_(bytestream_t& o, const char *fmt, const void *& data) {
	while (*fmt) {
		auto c = *fmt++;
		if (c == '%') {
			c = *fmt++;
			char buf[64] = { 0 };
			switch (c) {
			case 'f': sprintf(buf, "%.*g", FLT_DECIMAL_DIG - 1, *(float*)data); data = (const char*)data + sizeof(float); break;
			case 'd': sprintf(buf, "%.*g", DBL_DECIMAL_DIG - 1, *(double*)data); data = (const char*)data + sizeof(double);break;
			case 'i': sprintf(buf, "%" PRId32, *(std::int32_t*)data); data = (const char*)data + sizeof(std::int32_t);break;
			case 'q': sprintf(buf, "%" PRId64, *(std::int64_t*)data); data = (const char*)data + sizeof(std::int64_t);break;
			case '%': sprintf(buf, "%%"); break;
            case ']': return fmt;
            case '[': {
                char *loop_start = nullptr;
                auto loop_count = strtoull(fmt, &loop_start, 10);
                while(loop_count--) {
                    fmt = kprintf_(o, loop_start + 1, data);
                }
                break;
            }
			default:
				assert(0 && "Bad format string");
			}
			char *ptr = buf;
			while (*ptr) o.push_back(*ptr++);
		} else o.push_back(c);
	}
    return fmt;
}

void kprintf(bytestream_t& o, const char *fmt, const void* data) {
    kprintf_(o,fmt,data);
    o.push_back(0);
}


bool equal(bytestream_t& l, bytestream_t& r) {
/*	if (l.size() != r.size()) {
		printf("size: %i vs %i\n", (int)l.size(), (int)r.size());
		return false;
	}
	for (int i(0);i < l.size();++i) {
		if (l[i] != r[i]) {
			printf("%x vs %x at %i\n", l[i], r[i], i);
			return false;
		}
	}
	return true;*/

	picojson::value lj, rj;
	std::string err;
	picojson::parse(lj, l.data(), l.data() + l.size(), &err);
	picojson::parse(rj, r.data(), r.data() + r.size(), &err);
	return equal(lj, rj);
}

bool slurp(bytestream_t& v) {
	while (!feof(stdin)) v.push_back(fgetc(stdin));
	v.back() = 0;
	return true;
}

int run_test() {
	krt_class* k = GetClassData();
	char *result(new char[k->result_type_size]), *state(new char[GetSize()]);
	Evaluate(state, nullptr, result);
	bytestream_t resultStream, refStream;
	if (!isatty(fileno(stdin))) {
		slurp(refStream);
		kprintf(resultStream, k->result_type_descriptor, result);
		if (!equal(refStream, resultStream)) {
			printf("<==========================\n%s\n---------------------------\n%s\n==========================>\n",resultStream.data(), refStream.data());
			return -1;
		} else {
			return 0;
		}
	} else {
		kprintf(resultStream, k->result_type_descriptor, result);
		printf("%s",resultStream.data());
	}
	return 0;
}
