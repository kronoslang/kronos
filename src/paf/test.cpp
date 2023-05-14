#include <cassert>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <sstream>
#include "PAF.h"

int main() {
	using namespace PAF;
    
    std::cout << "PAF Can Read:\n";
    for(auto ext : PAF::GetReadableFormats()) {
        std::cout << ext << " ";
    }
    std::cout << "\n\nPAF Can Write:\n";

	std::vector<float> sine(10000);
	for (int i(0);i < 10000;++i) {
		sine[i] = sinf(float(i) / 100);
	}

	static const int bitDepths[] = { 16,24,32 };
	static const int bitRates[] = { 128000, 192000, 256000 };

    for(auto ext : PAF::GetWritableFormats()) {
		for (int q = 0;q < 3;++q) {
			std::stringstream name;
			name << "paf_q" << q + 1 << "." << ext;
			try {
				AudioFileWriter test(name.str().c_str());

				if (test->Has(PAF::BitDepth)) test->Set(PAF::BitDepth, bitDepths[q]);
				if (test->Has(PAF::BitRate)) test->Set(PAF::BitRate, bitRates[q]);
				if (test->Has(PAF::Lossless)) test->Set(PAF::Lossless, q == 2 ? 1 : 0);
				test->Set(PAF::SampleRate, 44100);
				test->Set(PAF::NumChannels, 2);
				test(sine.data(), 10000);
				std::cout << name.str() << " written!\n";
			} catch (...) {
				std::cout << name.str() << " failed!\n";
			}
		}
    }
    std::cout << "\n\n";
}
