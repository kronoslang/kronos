#include "audio.h"
#include "driver/CmdLineOpts.h"
#include "common/bitstream.h"

#include <iostream>
#include <future>

#define EXPAND_PARAMS \
	F(dump_audio, DA, 0, "<frames>", "Prints the audio signature for <frames> to stdout") \
	F(audio_driver, ad, std::string(".*"), "<regex>", "Select first audio device that matches regex")

namespace CL {
	using namespace CmdLine;
#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) Option<decltype(DEFAULT)> LONG(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION);
	EXPAND_PARAMS
#undef F
}


namespace Kronos {
	namespace IO {

        void AddAudioCmdLineOptions(CmdLine::IRegistry& reg) {
            CmdLine::Registry().AddParsersTo(reg);
        }

        class AudioHookSubject : public Subject {
			const char *driver;
		public:
			AudioHookSubject(const char *driver) :driver(driver) {}
			Runtime::MethodKey Id() const override {
				return { driver, nullptr };
			}
		};

		static std::string ZeroTuple(int numElements) {
			std::string t = "(";
			for (int i = 0; i < numElements; ++i) {
				if (i) t += " 0";
				else t += "0";
			}
			return t + ")";
		}

		AudioSubject::AudioSubject(IConfigurationDelegate* config, Subject* pre, Subject* post):preHook(pre), postHook(post), config(config) {
		}

		Runtime::MethodKey AudioSubject::Id() const {
			return { "audio" , nullptr };
		}
		
		AudioSessionState::AudioSessionState() : Device(Session.FindDevice(CL::audio_driver().c_str())) {
			if (Device == Session.end()) {
				throw std::runtime_error("Could not find any audio devices that match '" + CL::audio_driver() + "'");
			}
			Clock = Device->GetDeviceTimeCallback();
			OverrideClock(Clock, 10);
		}

		AudioSessionState& AudioSubject::State() {
			std::lock_guard<std::mutex> lg(initLock);
			try {
				if (!state) {
					state = std::make_unique<AudioSessionState>();
					config->Set(":Audio:Device-Inputs", ZeroTuple(state->Device->GetNumInputs()));
					config->Set(":Audio:Device-Outputs", ZeroTuple(state->Device->GetNumOutputs()));
				}
			} catch (std::exception& e) {
				std::clog << "* " << e.what() << "\n";
			}
			return *state;
		}

		AudioSubject::~AudioSubject() {
			if (state) {
				auto dev = State().Device;
				dev->Close();
			}
		}		

		void AudioSubject::Init() {
			auto dev = State().Device;
			When(dev->BufferSwitch, [&](PAD::IO io) {
				frameCount = io.numFrames;
				if (frameCount) {

					if (preHook) {
						preHook->Bind(&frameCount);
						preHook->Fire(nullptr, 1);
					}

/*					firingTimeStamp = TimePointTy(std::chrono::duration_cast<MicroSecTy>(io.outputBufferTime + this->clockOffset));
					firingTickRate = io.config.GetSampleRate();*/

					GetCurrentActivationTime() = Now(State().Clock, TimePointTy{ io.outputBufferTime });
					GetCurrentActivationRate() = io.config.GetSampleRate();

					memset(io.output, 0, sizeof(float) * io.numFrames * io.config.GetNumStreamOutputs());
					*Slot() = io.input;
					this->Fire(io.output, io.numFrames);

					if (postHook) {
						postHook->Bind(&frameCount);
						postHook->Fire(nullptr, 1);
					}
				}
			});

			auto cfg = dev->DefaultAllChannels();
			dev->Open(cfg);
		}

		void AudioSubject::Subscribe(const Runtime::MethodKey& subject, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot)  {
			Subject::Subscribe(subject, handle, instance, callback, slot);
			std::call_once(init, [&]() {Init();});
		}

		void AudioRateSubject::Subscribe() {
			PAD::EventSubscriber::Reset();
			auto& dev = *audio.State().Device;

			auto updateRate = [this](auto cfg) {
				if (rate != cfg.GetSampleRate()) {
					rate = (float)cfg.GetSampleRate();
					this->Fire(nullptr, 1);
				}
			};

			When(dev.AboutToBeginStream, updateRate);
		}

		void AudioRateSubject::Init() {
			rate = (float)audio.State().Device->DefaultAllChannels().GetSampleRate();
			Subscribe();
	}

		AudioRateSubject::AudioRateSubject(AudioSubject& audio) :audio(audio) {
			*Slot() = &rate;
		}

		Runtime::MethodKey AudioRateSubject::Id() const {
			return { "#Rate{audio}", "%f" };
		}

		void AudioRateSubject::Subscribe(const Runtime::MethodKey& subject, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) {
			std::call_once(init, [&]() { Init(); });
			Subject::Subscribe(subject, handle, instance, callback, slot);
		}
        
        void ListAudioDevices(std::ostream& os) {
            PAD::Session ses;
            for (auto& d : ses) {
                os << " - [" << d.GetHostAPI() << "] " << d.GetName() << " " << d.GetNumOutputs() << "/" << d.GetNumInputs() << "\n";
            }
        }


		namespace Audio {
			void Setup(IRegistry& reg, IConfigurationDelegate* config) {

				auto pre = new AudioHookSubject("(audio Pre)");
				auto post = new AudioHookSubject("(audio Post)");
				auto as = new AudioSubject{ config, pre, post };
				reg.Register(as);
				reg.Register(pre);
				reg.Register(post);
				reg.Register(new AudioRateSubject(*as));
			}
		}
	}
}
