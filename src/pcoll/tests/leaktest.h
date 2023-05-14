#pragma once
#include "util.h"
#include <memory>

std::atomic<size_t> leaks;

struct leak_test : pcoll::detail::reference_counted<pcoll::detail::multi_threaded> {
	std::unique_ptr<char[]> stuff;
	leak_test() {
		leaks.fetch_add(1);
		stuff = std::make_unique<char[]>(0x1000);
		memset(stuff.get(), 1, 0x1000);
	}

	~leak_test() {
		memset(stuff.get(), 0, 0x1000);
		leaks.fetch_sub(1);
	}
};
