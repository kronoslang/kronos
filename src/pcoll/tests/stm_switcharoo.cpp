#include "stm.h"
#include "tests.h"
#include "leaktest.h"
#include <array>
#include <unordered_map>
#include <random>

constexpr int num_entries = 4;
constexpr int num_threads = 16;
constexpr int num_rounds = 100000;


template <pcoll::detail::concurrent_strategy STRATEGY> int run_test(std::string label) {
	using namespace pcoll;

	leaks.store(0);

	cref<leak_test, detail::locking> mutable_array[num_entries];
	for(auto &mr : mutable_array) {
		mr = new leak_test;
	}

	std::array<std::thread, num_threads> workers;
	std::atomic<size_t> count;
	count.store(0);

	auto start_time = stopwatch();

	for(auto &t : workers) {
		t = std::thread([&mutable_array, &count]() {
			std::random_device rd;
			std::default_random_engine engine(rd());
			std::uniform_int_distribution<int> distribution(0, num_entries - 1);
			auto rng = std::bind(distribution, engine);
			for(int i = 0; i < num_rounds; ++i) {
				for(int j = 0; j < num_entries; ++j) {
					auto k = rng();
					auto a = mutable_array[j];
					auto b = mutable_array[k];
					if(a.get() == b.get()) {
						a = new leak_test;
					}
					mutable_array[j] = b;
					mutable_array[k] = a;
				}
				count.fetch_add(num_entries);
			}
		});
	}

	auto total = num_entries * num_rounds * num_threads;

	label = "STM Switcharoo (" + label;
	label += ")";

	spin(label, count, total);

	auto end_time = stopwatch();

	auto ms = dur_ms(start_time, end_time);
	std::cout << total << " contested transactions completed in " << ms << " milliseconds.\n";
	double secs = ms / 1000.0;
	double transact_rate = total / secs;
	std::cout << transact_rate << " transactions per second.\n";

	for(auto &t : workers) {
		t.join();
	}

	for(auto &mr : mutable_array) {
		mr.reset();
	}

	auto num_leaks = leaks.load();
	test_warn_if(num_leaks != 0, "memory management failure: " << num_leaks << " leaks");
	return (int)num_leaks;
}

int main() {
	auto failures = 
		run_test<pcoll::detail::locking>("mutex") +
		run_test<pcoll::detail::lockfree>("lockfree");

	return failures;
}