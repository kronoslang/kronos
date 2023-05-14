#include "inout.h"

#ifdef WIN32
#include <Windows.h>
#include <iostream>
#else
#include <thread>
#include <chrono>
#endif

namespace Kronos {
	namespace IO {
		const int Quantum = 4;
#ifdef WIN32
		class TimerImpl : public ITimer {
			MMRESULT timerId;
		public:
			ITimerCallback* cb;

			DurationTy GetPeriod() override {
				return std::chrono::milliseconds(Quantum * 2);
			}

			static void CALLBACK timerProc(UINT timerId, UINT uMsg, DWORD_PTR user, DWORD_PTR dw1, DWORD_PTR dw2) {
				((ITimerCallback*)user)->Timer();
			}

			TimerImpl(ITimerCallback* cb) :cb(cb) {
				timeBeginPeriod(Quantum);
				timerId = timeSetEvent(Quantum, Quantum, timerProc, (DWORD_PTR)cb, TIME_PERIODIC | TIME_KILL_SYNCHRONOUS);
			}

			~TimerImpl() {
				timeKillEvent(timerId);
				timeEndPeriod(Quantum);
			}

		};
#else
		class TimerImpl : public ITimer {
		public:
			std::atomic_flag active;
			std::thread timerThread;
			TimerImpl(ITimerCallback *cb) {
				active.test_and_set();
				timerThread = std::thread([cb, this]() {
					while (active.test_and_set()) {
						cb->Timer();
						std::this_thread::sleep_for(std::chrono::milliseconds(Quantum));
					}
				});
			}

			~TimerImpl() {
				active.clear();
				if (timerThread.joinable()) timerThread.join();
			}

			DurationTy GetPeriod() override {
				return std::chrono::milliseconds(Quantum);
			}
		};
#endif
		std::unique_ptr<ITimer> CreateTimer(ITimerCallback *cb) {
			return std::make_unique<TimerImpl>(cb);
		}
	}
}