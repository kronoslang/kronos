#include "rx.h"
#include <stdlib.h>

int main() {
	using namespace Rx;

	struct FloatSum {
		void operator()(const void** data, int numBuffers, int numFrames, Timestamp interval, Observable& sendVia) {
			const float** fdata = (const float**)data;
			auto result = (float*)alloca(numFrames * sizeof(float));
			if (numBuffers) {
				memcpy(result, fdata[0], sizeof(float) * numFrames);
				for (int i = 1; i < numBuffers; i++) {
					for (int c = 0; c < numFrames; c++) {
						result[c] += fdata[i][c];
					}
				}
			}
			sendVia.PushVia(result, numFrames, interval);
		}
	};

	struct Ping : public Observable {
		void operator()(float val) {
			float chunk[10] = { val,val,val,val,val,val,val,val,val,val };
			PushVia(chunk, rand() % 10, 0);
		}
		size_t SizeOfFrame() const override { return sizeof(float); }
	};

	struct ColdFlood : public Observable {
		unsigned limit = std::numeric_limits<unsigned>::max();
		int counter = 0;

		void Dump() {
			auto chunk = std::min<unsigned>(limit, 1000);
			std::vector<float> tmp(chunk);
			for (auto& t : tmp) {
				t = (float)counter++;
			}
			limit -= chunk;
			PushVia(tmp.data(), tmp.size(), 1);
		}

		void Request(unsigned lim) override {
			limit = lim;
			if (limit) Dump();
		}

		size_t SizeOfFrame() const override { return sizeof(float); }
	};

	Vector<Ref<Ping>> pings(16);
	Vector<IObservable::Ref> pptr(16);

	for (int i = 0; i < pings.size(); ++i) {
		pptr[i] = pings[i] = new Ping;
	}

	pptr.push_back(new ColdFlood);

	Ref<Zip<FloatSum>> ref = new Zip<FloatSum>(pptr, {}, sizeof(float));

	for (int i = 0; i < 10000; ++i) {
		auto which = rand() % pings.size();
		(*pings[which])((float)which);
	}
}