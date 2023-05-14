#include "common/PlatformUtils.h"

#include <memory>
#include <thread>
#include <sstream>
#include <deque>
#include <functional>
#include <iostream>

#ifdef HAS_O2

#include "o2driver.h"
#include "o2.h"
#include "o2internal.h"

#include "driver/CmdLineOpts.h"

#define EXPAND_PARAMS \
F(o2, o2, std::string(""), "<name>", "Listen to O2 messages within O2 ensemble <name>") \
F(o2_service, o2s, std::string("kronos"), "<name>", "O2 service name, 'kronos' by default") \
F(o2_master, o2m, false, "", "Act as O2 clock master") \
F(osc_port, op, 7777, "<port>", "Listen to OSC events on UDP <port>")

namespace CL {
    using namespace CmdLine;
#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) Option<decltype(DEFAULT)> LONG(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION);
    EXPAND_PARAMS
#undef F
}

KRONOS_ABI_EXPORT int64_t kvm_o2_send_begin_init(int64_t world) { 
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


namespace Kronos {
	namespace IO {
		namespace o2 {
                        
            class O2Subject : public IHierarchy {
                

                struct Slot {
                    std::unique_ptr<char> slotMemory;
                    size_t bufferSize;
                    krt_instance instance;
                    krt_process_call callback;
                    O2Subject *subject;
                    ManagedRef keepAlive;
                    std::string o2method;
                };

                std::thread pollThread;
                std::recursive_mutex contextLock;
                std::atomic_flag stopFlag;
                std::vector<char> scratch;
                std::unordered_map<std::string, Slot> methodSlots;
                std::deque<std::function<void()>> runOnO2Thread;
                
                static void PollProc(O2Subject *self) {
                    self->Poll();
                }
                
                static void MessageHandler(const o2_msg_data_ptr msg, const char *types,
                                           O2arg_ptr *argv, int argc,
                                           const void *user_data) {
                    
                    Slot *s = (Slot *)user_data;

                    char *ptr = s->slotMemory.get();
                    
                    o2_extract_start(msg);
                    
                    while(*types) {
                        O2type ty = (O2type)*types++;
                        size_t sz = 0;
                        switch(ty) {
                            case O2_FLOAT:
                            case O2_INT32:
                                sz = 4;
                                break;
                            case O2_DOUBLE:
                            case O2_INT64:
                                sz = 8;
                                break;
                            default:
                                o2_get_next(ty);
                                continue;
                        }
                        
                        auto source = o2_get_next(ty);
                        memcpy(ptr, source, sz);
                        ptr += sz;
                    }

                    s->callback(s->instance, s->subject->scratch.data(), 1);
                }
                
                std::string appName;
                std::string serviceName;
                
                void RunOnO2Thread(std::function<void()> fn) {
                    std::lock_guard<std::recursive_mutex> lg { contextLock };
                    runOnO2Thread.emplace_back(std::move(fn));
                }
                
                
                
            public:
                O2Subject():scratch(16384) {
                    
                    appName = CL::o2();
                    
                    if (!appName.empty()) {
                        serviceName = CL::o2_service();
                        std::clog << "[O2] Listening as " << appName << "/" << serviceName << std::endl;
                        stopFlag.test_and_set();
                        pollThread = std::thread(PollProc, this);
                    }
                }
                
                ~O2Subject() {
                    stopFlag.clear();
                    if (pollThread.joinable()) {
                        pollThread.join();
                    }
                }
                
                void Poll() {
                                        
                    o2_initialize(appName.c_str());
                    
                    if (CL::o2_master()) {
                        o2_clock_set(NULL, NULL);
                    }
                    
                    o2_service_new(serviceName.c_str());
                    o2_osc_port_new(serviceName.c_str(), CL::osc_port(), 0);

                    while(stopFlag.test_and_set()) { {
                            std::lock_guard<std::recursive_mutex> lg { contextLock };
                        
                            if (runOnO2Thread.size()) {
                                auto run = runOnO2Thread.front();
                                runOnO2Thread.pop_front();
                                try {
                                    run();
                                } catch (std::exception &ex) {
                                    fprintf(stderr, "%s", ex.what());
                                }
                                continue;
                            }
                        
                            o2_poll();
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                    
                    o2_finish();
                }
                
                static int GetO2Signature(std::ostream& sig, const char * kronosTypeSig) {
                    
                    struct LoopStack {
                        const char *top;
                        size_t counter;
                    };
                    
                    std::vector<LoopStack> loops;
                    
                    int size = 0;
                    if (kronosTypeSig) {
                        while (*kronosTypeSig) {
                            if (*kronosTypeSig++ == '%') {
                                switch (*kronosTypeSig++) {
                                    default: break;
                                    case 'f': sig.put(O2_FLOAT); size += sizeof(float); break;
                                    case 'd': sig.put(O2_DOUBLE); size += sizeof(double);  break;
                                    case 'q': sig.put(O2_INT64); size += sizeof(std::int64_t); break;
                                    case 'i': sig.put(O2_INT32); size += sizeof(std::int32_t); break;
                                    case '[':{
                                            char *loop_start = nullptr;
                                            auto loop_count = strtoull(kronosTypeSig, &loop_start, 10);
                                            kronosTypeSig = ++loop_start;
                                            loops.push_back(LoopStack { loop_start, loop_count });
                                        }
                                        break;
                                    case ']':
                                        if (--loops.back().counter < 1) {
                                            loops.pop_back();
                                        } else {
                                            kronosTypeSig = loops.back().top;
                                        }
                                        break;
                                }
                            }
                        }
                    }
                    return size;
                }
                
                std::string GetO2Path(std::string methodName) {
                    return "/" + serviceName + "/" + methodName;
                }
                                
                void Subscribe(const Runtime::MethodKey& mk, const ManagedRef& mr, krt_instance inst, krt_process_call callback, void const** slot) override {
                    
                    if (mk.signature == nullptr) return;

                    std::string o2method = GetO2Path(mk.name);
                    Unsubscribe(mk, inst);

                    std::stringstream o2type;
                    int size = GetO2Signature(o2type, mk.signature);
                    
                    auto key = std::string(mk.name) + " " + mk.signature;
                                                          
                    methodSlots[key] = Slot {
                        std::make_unique<char>(size),
                        (size_t)size,
                        inst, callback,
                        this, mr, o2method
                    };
                    
                    *slot = (void*)methodSlots[key].slotMemory.get();
                    
                    if (size < 4) return;
                    
                    auto o2ty = o2type.str();
                    
                    RunOnO2Thread([=]() { o2_method_new(o2method.c_str(), o2ty.c_str(), MessageHandler, &methodSlots[key], false, false); });
                }
                
                void Unsubscribe(const Runtime::MethodKey& mk, krt_instance) override {

                    if (mk.signature == nullptr) return;

                    auto key = std::string(mk.name) + " " + mk.signature;

                    auto method = methodSlots.find(key);
                    if (method != methodSlots.end()) {
                        std::lock_guard<std::recursive_mutex> lg { contextLock };
                        RunOnO2Thread([=] () { o2_method_free(method->second.o2method.c_str()); });
                        methodSlots.erase(method);
                    }
                }
                
                bool HasActiveSubjects() const override {
                    return !methodSlots.empty();
                }

            };
            
			std::unique_ptr<IHierarchy> Setup(IRegistry& cx, IConfigurationDelegate* config) {
                                                               
				config->Set(":O2:Send-Data", "data => VM:Make-OP[\"kvm_o2_send_data!\" \"const char*\" String:Interop-Format(data) \"const void*\" data]");
				config->Set(":O2:Send-Start", "data => VM:Make-Op[\"kvm_o2_send_begin!\" \"const void*\" data]");
				config->Set(":O2:Send-Finish", "data => VM:Make-Op[\"kvm_o2_send_end!\" \"const char*\" address]");
				config->Set(":O2:Send", "{ Use Actions (address data) = arg Reactive:Resample(Do(Send-Start(data) For(data Send-Data) Send-Finish(address)) data) }");
                
                return std::make_unique<O2Subject>();
			}
		}
	}
}

#endif
