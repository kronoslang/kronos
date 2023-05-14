#include "common/PlatformUtils.h"
#include <memory>

#ifdef HAS_O2

#include "o2driver.h"
#include "o2.h"

static void init_o2() {
    static bool initialized = false;
    if (initialized) return;

	const char *appName = getenv("KO2_APPLICATION");
	if (!appName) appName = "ko2";
    
    o2_initialize(appName);
    initialized = true;
}

KRONOS_ABI_EXPORT int64_t kvm_o2_send_begin_init(int64_t world) { 
	init_o2();
	return world;
}
KRONOS_ABI_EXPORT int64_t kvm_o2_send_data_init(int64_t world, const char *type, const void* data) { return world; }
KRONOS_ABI_EXPORT int64_t kvm_o2_send_end_init(int64_t world) { return world; }

static std::mutex& msg_lock() {
    static std::mutex l;
    return l;
}

KRONOS_ABI_EXPORT int64_t kvm_o2_send_begin(int64_t world, const void*) {
	msg_lock().lock();
	o2_send_start();
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_o2_send_data(int64_t world, const char *typeString, const char* data) {
	std::string str;
	while (*typeString) {
		const char t = *typeString++;
		if (t == '%') {
			if (str.size()) {
				o2_add_string(str.c_str());
				str.clear();
			}
			switch (*typeString++) {
			case 'f': o2_add_float(*(float*)data); data += 4; break;
			case 'd': o2_add_double(*(double*)data); data += 8; break;
			case 'i': o2_add_int32(*(int32_t*)data); data += 4; break;
			case 'q': o2_add_int64(*(int64_t*)data); data += 8; break;
			default: break;
			}
		} else {
			str.push_back(t);
		}
	}
	if (str.size()) o2_add_string(str.c_str());
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_o2_send_end(int64_t world, const char *address) {
	o2_send_finish(0, address, 0);
	msg_lock().unlock();
	return world;
}
#else 
#include "inout.h"
#endif


namespace Kronos {
	namespace IO {
		namespace o2 {
			void Setup(IRegistry& cx, IConfigurationDelegate* config) {
				config->Set(":O2:Send-Data", "data => VM:Make-OP[\"kvm_o2_send_data!\" \"const char*\" String:Interop-Format(data) \"const void*\" data]");
				config->Set(":O2:Send-Start", "data => VM:Make-Op[\"kvm_o2_send_begin!\" \"const void*\" data]");
				config->Set(":O2:Send-Finish", "data => VM:Make-Op[\"kvm_o2_send_end!\" \"const char*\" address]");
				config->Set(":O2:Send", "{ Use Actions (address data) = arg Reactive:Resample(Do(Send-Start(data) For(data Send-Data) Send-Finish(address)) data) }");
			}
		}
	}
}
