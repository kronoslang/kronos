#include "tests.h"
#include "leaktest.h"
#include "hamt.h"
#include <array>

constexpr int num_entries = 100000;

int main() {
	using namespace pcoll;

	hamt<int, detail::ref<leak_test>> dictionary;

	leaks.store(0);

	for(int i = 0; i < num_entries; ++i) {
		dictionary = dictionary.assoc(i, new leak_test);
	}


	for(int i = 0; i < num_entries; ++i) {
		if(i % 1000 == 0) {
			auto mem = dictionary.measure_memory_use();
			auto optimal = sizeof(decltype(dictionary)::keyvalue_t);
			auto num = num_entries - i;
			std::cout << "Memory use at " << num << " entries: " << mem << " (" << (mem - optimal * num) / num << " bytes overhead / entry)\n";
		}
		dictionary = dictionary.dissoc(i);
	}

	test_warn_if(leaks != 0, "memory management failure in hamt buildup and teardown");
	return (int)leaks;
}