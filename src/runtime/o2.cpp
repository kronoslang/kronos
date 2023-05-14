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
namespace Kronos {
    namespace IO {
        namespace MIDI {
            void Setup(IRegistry& reg) {
            }
            
        }
    }
}
#endif
