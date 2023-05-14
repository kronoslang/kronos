#include "hamt.h"
#include "tests.h"
#include <iostream>
#include <algorithm>
#include <thread>
#include <array>

bool ready = false;

int main() {
	using namespace pcoll;

	static constexpr int num_threads = 2, num_entries = 1000, num_rounds = 50;

	hamt<int, int> fuzz;

	progress_t progress = 0;
	
	std::array<std::thread, num_threads> threads;
	for(auto &t : threads) {
		t = std::thread([&]() {
			for(int j = 0; j < num_rounds; ++j) {
				for(int i = 0; i < num_entries; ++i) {					
					fuzz.update_in(i, [](optional<int> value) {
						if(value.has_value) return *value + 1;
						else return 1;
					});
				}
				progress += num_entries;
			}
		});
	}

	spin("HAMT Concurrent Fuzz", progress, num_threads * num_entries * num_rounds);

	for(auto &t : threads) {
		t.join();
	}

	for(int i = 0; i < num_entries; ++i) {
		auto val = *fuzz[i];
		test_assert(val == num_threads * num_rounds, "incorrect value in hash array mapped trie");
	}
}