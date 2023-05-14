#include "treap.h"
#include "tests.h"

#include <set>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>

std::string verbalize(int number) {
	const char *small_number[21] = {
		"zero",
		"one",
		"two",
		"three",
		"four",
		"five",
		"six",
		"seven",
		"eight",
		"nine",
		"ten",
		"eleven",
		"twelve",
		"thirteen",
		"fourteen",
		"fifteen",
		"sixteen",
		"seventeen",
		"eighteen",
		"nineteen",
		"twenty"
	};

	const char *tens[] = {
		"",
		"",
		"",
		"thirty",
		"fourty",
		"fifty",
		"sixty",
		"seventy",
		"eighty",
		"ninety"
	};

	if(number <= 20) {
		return small_number[number];
	}

	if(number < 30) {
		return "twenty" + verbalize(number - 20);
	}

	if(number < 100) {
		return tens[number / 10] + verbalize(number % 10);
	}

	if(number < 1000) {
		return verbalize(number / 100) + "hundred " + verbalize(number % 100);
	}

	if(number < 1000000) {
		return verbalize(number / 1000) + "thousand " + verbalize(number % 1000);
	}

	if(number < 1000000000) {
		return verbalize(number / 1000000) + "million " + verbalize(number % 1000000);
	}

	return verbalize(number / 1000000000) + "billion " + verbalize(number % 1000000000);
}

int main() {
	using namespace pcoll;

	static constexpr int num_entries = 100;

	std::set<std::string> reference;
	treap<std::string> test;

	for(int i = 0; i < num_entries; ++i) {
		auto val = verbalize(rand());
		reference.emplace(val);
		test = test.insert(val);
		test.check_invariants();
	}

	decltype(reference) check;
	std::vector<std::string> all;
	test.for_each([&](std::string value) {
		check.emplace(value);
		all.emplace_back(value);
	});

	test_assert(check == reference, "treap item insertion did not work correctly");

	auto copy = test;
	for (int i = 0;i < num_entries;++i) {
		std::string val;
		copy = copy.pop_front(val);
		copy.check_invariants();
		std::cout << val << std::endl;
	}

	while (all.size()) {
		auto val = all[rand() % all.size()];
		all.erase(std::remove(all.begin(), all.end(), val), all.end());
		test = test.remove(val);
		reference.erase(val);
		test.check_invariants();
		test_assert(reference.size() == test.count(), "item not properly removed from treap");
	}
	
	check.clear();
	test.for_each([&check](std::string value) {
		check.emplace(value);
	});

	test_assert(check == reference, "treap item removal did not work correctly");
}