#include "hamt.h"
#include "tests.h"

#include <iostream>
#include <unordered_map>
#include <cstdlib>
#include <string>

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

	std::unordered_map<std::string, int> reference;
	hamt<std::string, int> test;

	for(int i = 0; i < num_entries; ++i) {
		auto val = rand();
		reference[verbalize(val)] = val;
		test = test.assoc(verbalize(val), val);
	}
}