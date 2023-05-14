#include "tests.h"
#include <iostream>
#include <thread>

void spin(const std::string& label, std::atomic<size_t>& progress, size_t total) {
	const char *spinner[7] = {
		".oOo. ",
		" .oOo.",
		". .oOo",
		"o. .oO",
		"Oo. .o",
		"oOo. .",
		"......"
	};

	int spin_i = 0;

	for(;;) {
		auto percent = (progress.load(std::memory_order_relaxed) * 100) / total;
		if(percent == 100) spin_i = 6;
		std::cout << "\r[" << label << "] " << spinner[spin_i] << spinner[spin_i] << spinner[spin_i] << spinner[spin_i] << " " << percent << "%   ";
		if(percent == 100) break;
		spin_i = (spin_i + 1) % 6;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}

	std::cout << "\n";
}