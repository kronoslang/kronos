#include "kronos_abi.h"
#include "TestInstrumentation.h"
#include "common/bitstream.h"
#include "paf/PAF.h"

#include <iostream>
#include <cstring>
#include <cmath>
#include <vector>
#include <thread>

namespace {
	static constexpr int M1 = 12, N1 = 12;
	static constexpr int M = M1*M1, N = N1*N1;
	static constexpr int BaseResolution = 16;
	static constexpr int SignatureBits = BaseResolution + N1;
	static_assert(M1 == N1, "use square matrix");
}

KRONOS_ABI_EXPORT int64_t kvm_test_capture_init(const float* data, float* passThru, int64_t size);
KRONOS_ABI_EXPORT int64_t kvm_test_capture(const float* data, float* passThru, int64_t size);

KRONOS_ABI_EXPORT void* link_instruments() MARK_USED;

KRONOS_ABI_EXPORT void* link_instruments() {
	static void *api[] = {
		(void*)kvm_test_capture,
		(void*)kvm_test_capture_init
	};
	return api;
}


bool AudioDiff(const std::string& result, const std::string& reference) {
	if (result != reference) {
		// estimate magnitude differences
		bitstream bta, btb;
		bta.base64_decode(result);
		btb.base64_decode(reference);

		if (bta.num_bits() != btb.num_bits()) {
			std::cerr << "* Different reference sizes: " << bta.num_bits() << " vs. " << btb.num_bits() << "\n";
			return true;
		}

		size_t bits_per_frame = (16 + N1) * (N + M);

		int difs = 0;

		const int threshold = N;
		std::int64_t max_diff = 0;

		for (int f = 0; f < bta.num_bits() / bits_per_frame; ++f) {
			auto frame_base = f * bits_per_frame;
			
			if (bta.num_bits() - frame_base < bits_per_frame) break;
			
			for (int j = 0; j < N; ++j) {
				auto bit_pos = (int)(frame_base + j * SignatureBits);
				std::int64_t sum_a = bta.at_bit<SignatureBits>(bit_pos).widen();
				std::int64_t sum_b = btb.at_bit<SignatureBits>(bit_pos).widen();
				auto magndiff = std::abs(sum_a - sum_b);
				max_diff = std::max(max_diff, magndiff);
				if (magndiff > threshold) {
					if (difs < 8 ) std::cerr << "* Magnitude difference of " << sum_a - sum_b << " on column " << j << " on frame starting at " << f * M * N;
					else if (difs == 8) std::cerr << "* ... and more *\n";
					difs++;
				}
			}

			for (int i = 0; i < M; ++i) {
				auto bit_pos = (int)(frame_base + (N + i) * SignatureBits);
				std::int64_t sum_a = bta.at_bit<SignatureBits>(bit_pos).widen();
				std::int64_t sum_b = btb.at_bit<SignatureBits>(bit_pos).widen();
				auto magndiff = std::abs(sum_a - sum_b);
				max_diff = std::max(max_diff, magndiff);
				if (magndiff > threshold) {
					if (difs < 8) std::cerr << "* Magnitude difference of " << sum_a - sum_b << " on row " << i << " on frame starting at " << f * M * N;
					else if (difs == 8) std::cerr << "* ... and more *\n";
					difs++;
				}
			}
		}
		if (difs) {
			auto percent = (100 * difs) / ((N + M) * bta.num_bits() / bits_per_frame);
			std::cerr << "* " << difs << " differences detected, encompassing " << percent << "% of fingerprint *\n";
		}
		if (max_diff) std::cerr << "* maximum fingerprint deviation was " << max_diff << "\n";
		return difs != 0;
	}
	return false;
}

using namespace Kronos::IO;
using namespace Kronos::Runtime;
class InstrumentedIOImpl : public InstrumentedIO {
	std::vector<float> dump;
	int dumpChannels = 1;
	float dumpRate = 44100.f;
	int dumpFrames = 0;
	static thread_local InstrumentedIOImpl* current;
	static TimePointTy fakeTimePoint;
public:
	InstrumentedIOImpl(int frames) :dumpFrames(frames) {
		fakeTimePoint = Now();
		OverrideClock(FakeClock, 10);
	}

	static std::chrono::microseconds FakeClock() {
		return fakeTimePoint.time_since_epoch();
	}

	~InstrumentedIOImpl() {
	}

	void AddDelegate(Kronos::IO::IConfigurationDelegate&) { }
	void RemoveDelegate(Kronos::IO::IConfigurationDelegate&) { }

	std::function<void(void)> dumpAudioThread;

	void DumpProc(krt_instance i, krt_process_call proc) {
		const int blockSize = 1024;
		float scratch[blockSize * 8];
		current = this;

		auto tp = FakeClock();
		dump.reserve(dumpChannels * dumpFrames + blockSize);
		while (dump.size() < dumpChannels * dumpFrames) {
			auto sizeBefore = dump.size();
			GetCurrentActivationTime() = TimePointTy{ tp };
			GetCurrentActivationRate() = dumpRate / 1000000.;
			proc(i, scratch, blockSize);
			if (dump.size() == sizeBefore) {
				return;
			}
			tp += std::chrono::microseconds((int)(blockSize * 1000000 / dumpRate));
		}
		dump.resize(dumpChannels * dumpFrames);
	}
	
	void Subscribe(const MethodKey& mk, const ManagedRef& ref, krt_instance i, krt_process_call proc, const void** slot) override {
		if (mk == MethodKey{ "#Rate{audio}" }) {
			*slot = &dumpRate;
		} else if (mk == MethodKey{ "audio" }) {
			dumpAudioThread = [=]() { DumpProc(i, proc); };
		}
	}

	void Unsubscribe(const Kronos::Runtime::MethodKey& mk, krt_instance i) override {
		if (mk == MethodKey{ "audio" } && dumpAudioThread) {
			dumpAudioThread();
			dumpAudioThread = {};
		}
	}

	bool HasActiveSubjects() const override {
		return false;
	}

	void PrintTestSignature(std::ostream& stream) {
		if (dump.empty()) return;

		float matrix[M][N];
		bitstream encoder;

		for (int chunk = 0; chunk < dump.size(); chunk += M * N) {
			float *mptr = (float*)matrix;
			int j = 0;
			auto validFrames = std::min<size_t>(dump.size() - chunk, M * N);

			for (; j < validFrames; ++j) {
				*mptr++ = dump[chunk + j]; // (std::int16_t)(std::max(-1.f, std::min(32767.f / 32768.f, dump[chunk + j])) * 32768.f);
			}

			for (; j < M * N; ++j) {
				*mptr++ = 0;
			}

			for (int c = 0; c < N; ++c) {
				std::int32_t sum = 0;
				for (int r = 0; r < M; ++r) {
					std::int32_t tmp = std::int32_t(roundf(matrix[r][c] * float(1 << BaseResolution)));
					tmp = std::min(tmp, (1 << SignatureBits) - 1);
					tmp = std::max(tmp, -(1 << SignatureBits));
					sum += tmp;
				}
				encoder.push_back(bitchunk<SignatureBits>(sum));
			}

			for (int r = 0; r < M; ++r) {
				std::int32_t sum = 0;
				for (int c = 0; c < N; ++c) {
					std::int32_t tmp = std::int32_t(roundf(matrix[r][c] * float(1 << BaseResolution)));
					tmp = std::min(tmp, (1 << SignatureBits) - 1);
					tmp = std::max(tmp, -(1 << SignatureBits));
					sum += tmp;
				}
				encoder.push_back(bitchunk<SignatureBits>(sum));
			}
		}

		encoder.pad();
		encoder.base64_encode(stream);
		stream << std::endl;
	}

	bool DumpAudio(const char *fileName) override {		
		const float SilenceThreshold = 0.01;

		if (dump.size()) {
			auto file = PAF::AudioFileWriter(fileName);
			if (file) {
				file->TrySet(PAF::BitDepth, 16);
				file->TrySet(PAF::BitRate, dumpChannels * 96000);
				file->Set(PAF::SampleRate, dumpRate);
				file->Set(PAF::NumChannels, dumpChannels);
				auto toDo = dump.size();
				float peak = 0.f;
				for (auto& d : dump) {
					if (fabsf(d) > peak) {
						if (peak > SilenceThreshold) break;
						peak = fabsf(d);
					}
				}

				if (peak < SilenceThreshold) throw std::runtime_error("Silent audio, probable error");

				while (toDo) {
					auto didWrite = file(dump.data(), (int)toDo);
					if (didWrite == 0) throw std::runtime_error("Write failed");
					toDo -= didWrite;
				}
				dump.clear();
				return true;
			} else {
				throw std::runtime_error(std::string("Could not write '") + fileName + "'");
			}
		} 
		return false;
	}

	void Capture(const float* data, int64_t size) {
		dumpChannels = int(size / sizeof(float));
		for (int i = 0; i < dumpChannels; ++i) {
			dump.emplace_back(data[i]);
		}
	}

	static void CaptureProc(const float* data, int64_t size) {
		if (current) current->Capture(data, size);
	}
};

thread_local InstrumentedIOImpl* InstrumentedIOImpl::current;
TimePointTy InstrumentedIOImpl::fakeTimePoint;

std::unique_ptr<InstrumentedIO> InstrumentedIO::Create(int numChannels) {
	return std::make_unique<InstrumentedIOImpl>(numChannels);
}

KRONOS_ABI_EXPORT int64_t kvm_test_capture_init(const float* data, float* passThru, int64_t size) {
	memcpy(passThru, data, (size_t)size);
	return 0;
}

KRONOS_ABI_EXPORT int64_t kvm_test_capture(const float* data, float* passThru, int64_t size) {
	memcpy(passThru, data, (size_t)size);
	InstrumentedIOImpl::CaptureProc(data, size);
	return 0;
}
