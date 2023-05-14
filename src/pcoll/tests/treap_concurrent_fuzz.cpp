#include "treap.h"
#include "tests.h"
#include "leaktest.h"

#include <array>
#include <thread>
#include <cstdlib>
#include <chrono>
#include <sstream>

int main() {
	using namespace pcoll;

	static constexpr int num_entries = 10000, num_threads = 8;

	treap<int> priority_queue;

	std::atomic<size_t> counter = 0;

	std::array<std::thread, num_threads> producers;


	std::stringstream log;

	for(int i=0;i<num_threads;++i) {
		producers[i] = std::thread([&priority_queue,i]() {
			static constexpr int max_step = 100;
			int current = i * max_step * num_entries;
			for (int j = 0;j < num_entries; ++j) {
				current += 1 + (rand() % max_step);
				priority_queue.insert_into(current);
				std::this_thread::yield();
			}
		});
	}

	std::thread consumer([&]() {
		while (counter < num_entries * num_threads) {
			bool did_receive = false;
			int recv = 0;
			if (priority_queue.try_pop_front(recv)) {
				log << recv << "\n";
				if (++counter >= num_entries * num_threads) break;
			} else {
				using namespace std::chrono;
				std::this_thread::sleep_for(1ms);
			}
		}
	});

	spin("Treap Concurrent Fuzz", counter, num_entries * num_threads);

	for (auto &p : producers) {
		if (p.joinable()) p.join();
	}

	if (consumer.joinable()) consumer.join();
}