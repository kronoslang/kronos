#pragma once
#include "inout.h"
#include "pad/pad.h"

#include <ostream>

namespace Kronos {
	namespace IO {

        void AddAudioCmdLineOptions(CmdLine::IRegistry&);
        
		struct AudioSessionState {
			PAD::Session Session;
			PAD::AudioDeviceIterator Device;
			RealtimeClock Clock;
			AudioSessionState();

			using Ref = std::shared_ptr<AudioSessionState>;
		};

		class AudioSubject : public Subject, PAD::EventSubscriber {
			std::unique_ptr<AudioSessionState> state;
			std::mutex initLock;
			std::once_flag init;
			Subject *preHook, *postHook;
			int32_t frameCount;
			IConfigurationDelegate* config;
		public:
			AudioSubject(IConfigurationDelegate* config, Subject* preHook = nullptr, Subject* postHook = nullptr);
			~AudioSubject();
			Runtime::MethodKey Id() const override;
			AudioSessionState& State();
			void Init();
			void Subscribe(const Runtime::MethodKey& subject, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) override;
		};

		class AudioRateSubject : public Subject, public PAD::EventSubscriber {
			AudioSubject& audio;
			float rate = 44100.f;
			std::once_flag init;

			void Subscribe();
			void Init();
			PAD::EventSubscriber sampleRateChange;
		public:
			AudioRateSubject(AudioSubject& audio);
			Runtime::MethodKey Id() const override;
			void Subscribe(const Runtime::MethodKey& subject, const ManagedRef& handle, krt_instance instance, krt_process_call callback, void const** slot) override;
		};

		namespace Audio {
			void Setup(IRegistry& reg, IConfigurationDelegate*);
		}
	}
}
