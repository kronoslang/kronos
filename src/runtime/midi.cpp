#include "midi.h"
#include "common/PlatformUtils.h"
#include <memory>

#ifdef _MSC_VER
#include <Windows.h>
#pragma comment(lib, "winmm.lib")

namespace Kronos {
	namespace IO {
		namespace MIDI {
			struct Stream {
				Stream(const Stream&) = delete;
				HMIDIIN handle;
				Stream(UINT id, DWORD_PTR callback, DWORD_PTR data) {
					timeBeginPeriod(1);
					midiInOpen(&handle, id, callback, data, CALLBACK_FUNCTION);
					midiInStart(handle);
				}

				~Stream() {
					midiInStop(handle);
					midiInClose(handle);
				}
			};

			class Device : public Subject {
				UINT id;
				std::unique_ptr<Stream> stream;
				std::string name;
				static void CALLBACK callbackProc(HMIDIIN in, UINT wMsg, DWORD_PTR self, DWORD_PTR dw1, DWORD_PTR dw2) {
					auto dev = (Device*)self;
					if (dw1 != 0xfe) {
						dev->callback(_byteswap_ulong((std::uint32_t)dw1) >> 8);
					}
				}

				std::uint32_t midi;
				void callback(std::uint32_t msg) {
					midi = msg;
					Fire(nullptr, 1);
				}

				void sanitize(std::string& n) {
					for (auto& c : n) {
						if (!isalnum(c)) c = '_';
					}
				}
				
			public:
				Device(UINT id, std::string n):id(id) {
					Bind(&midi);
					sanitize(n);
					name = "/midi/" + n;
				}

				Stream& GetStream() {
					if (!stream) stream = std::make_unique<Stream>(
						id, (DWORD_PTR)callbackProc, (DWORD_PTR)this);
					return *stream;
				}

				void Subscribe(const Runtime::MethodKey& mk, const ManagedRef& mr, 
							   krt_instance inst, krt_process_call proc, void const** slot) override {
					GetStream();
					Subject::Subscribe(mk, mr, inst, proc, slot);
				}

				void Unsubscribe(const Runtime::MethodKey& mk, krt_instance inst) override {
					Subject::Unsubscribe(mk, inst);
					if (subscribers.empty()) stream.reset();
				}

				Runtime::MethodKey Id() const override {
					return { name.c_str(), "%i" };
				}
			};

			void Setup(IRegistry& reg) {
				int numDevs = midiInGetNumDevs();
				auto allMidiDevices = new Aggregator({ "/midi", "%i" });
				reg.Register(allMidiDevices);
				for (int i = 0;i < numDevs;++i) {
					MIDIINCAPS caps;
					midiInGetDevCaps(i, &caps, sizeof(MIDIINCAPS));
					auto device = new Device(i, encode_utf8(caps.szPname));
					allMidiDevices->Include(device);
					reg.Register(device);
				}
			}
		}
	}
}
#else
#ifdef __APPLE__
#import <CoreMIDI/CoreMIDI.h>

namespace Kronos {
    namespace IO {
        namespace MIDI {
            
            void Callback(const MIDIPacketList *newPackets, void *refCon, void* connRefCon);

            struct Client {
                MIDIClientRef client;
                MIDIPortRef input;
                std::function<void(MIDIObjectRef)> addSourceCallback;
            
                static void CoreMIDINotification(const MIDINotification* message, void *user) {
                    auto c = (Client*)user;
                    if (message->messageID == kMIDIMsgObjectAdded) {
                        auto addRemove = (MIDIObjectAddRemoveNotification*)message;
                        if (addRemove->childType == kMIDIObjectType_Source) {
                            if (c->addSourceCallback)
                                c->addSourceCallback(addRemove->child);
                        }
                    }
                }

                Client(MIDIReadProc callback) {
                    MIDIClientCreate(CFSTR("kronosio"), CoreMIDINotification, (void*)this, &client);
                    MIDIInputPortCreate(client, CFSTR("kronosio"), callback, nullptr, &input);
                }
                
                ~Client() {
                    MIDIPortDispose(input);
                    MIDIClientDispose(client);
                }
                
                static Client& Get() {
                    static Client c{ Callback };
                    return c;
                }
            };
            
            
            class Device : public Subject {
                std::int32_t evtData;
                std::string id;
                Client& c;
                MIDIEndpointRef source;
            public:
                Device(Client& c, MIDIEndpointRef source, std::string name):source(source), c(c) {
                    MIDIPortConnectSource(c.input, source, this);
                    Bind(&evtData);
                    id = "/midi/" + name;
                }
                
                ~Device() {
                    MIDIPortDisconnectSource(c.input, source);
                }
                
                void Event(std::int32_t data) {
                    evtData = data;
                    Bind(&evtData);
                    Fire(nullptr, 1);
                }

                Runtime::MethodKey Id() const override {
                    return { id.c_str(), "%i" };
                }
            };
            
            void Callback(const MIDIPacketList *newPackets, void * __nullable refCon, void* __nullable connRefCon) {
                Device *dev = (Device*)connRefCon;
                int numPackets = newPackets->numPackets;
                const MIDIPacket* mp = &newPackets->packet[0];
                while(numPackets--) {
                    std::int32_t evt =
                        (mp->data[0] << 16) |
                        (mp->data[1] << 8) |
                        mp->data[2];
                    dev->Event(evt);
                    mp = MIDIPacketNext(mp);
                }
            }
            
            static std::string MoveToStdString(CFStringRef csr) {
                if (!csr) return "";
                const char *bytes = CFStringGetCStringPtr(csr, CFStringGetSystemEncoding());
                if (bytes) return bytes;
                std::string tmp;
                tmp.resize(CFStringGetLength(csr));
                CFStringGetCString(csr, (char*)tmp.data(), tmp.size() + 1, CFStringGetSystemEncoding());
                return tmp;
            }
            
            static std::string GetMidiEndpointName(MIDIEndpointRef ep) {
                MIDIEntityRef entity;
                MIDIDeviceRef device;
                
                CFStringRef endpointName, deviceName;
                MIDIEndpointGetEntity(ep, &entity);
                MIDIEntityGetDevice(entity, &device);
                
                MIDIObjectGetStringProperty(ep, kMIDIPropertyName, &endpointName);
                MIDIObjectGetStringProperty(device, kMIDIPropertyName, &deviceName);
                
                return MoveToStdString(deviceName) + "/" + MoveToStdString(endpointName);
            }
            
            void Setup(IRegistry& reg) {
                ItemCount numSources = MIDIGetNumberOfSources();
                auto allDevices = new Aggregator({ "/midi", "%i" });
                auto &client{Client::Get()};
                
                client.addSourceCallback = [&](MIDIObjectRef source) {
                    auto sourceName = GetMidiEndpointName(source);
                    auto device = new Device(client, source, sourceName);
                    allDevices->Include(device);
                    reg.Register(device);
                };
                
                for(ItemCount i = 0;i < numSources;++i) {
                    auto endpoint = MIDIGetSource(i);
                    auto sourceName = GetMidiEndpointName(endpoint);
                    auto device = new Device(client, endpoint, sourceName);
                    allDevices->Include(device);
                    reg.Register(device);
                }
                reg.Register(allDevices);
            }
        }
    }
}

#else
namespace Kronos {
    namespace IO {
        namespace MIDI {
            void Setup(IRegistry& reg) {
            }
            
        }
    }
}
#endif
#endif
