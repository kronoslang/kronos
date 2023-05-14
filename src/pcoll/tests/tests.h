#pragma once
#include <iostream>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <string>

#define test_warn_if(cond, msg) if(cond) { std::cerr << "** Warning: " #cond ", " msg; }
#define test_assert(cond, msg) if(!(cond)) { std::cerr << "** Failed: " #cond ", " msg; exit(-1); }

using progress_t = std::atomic<size_t>;
void spin(const std::string& label, progress_t& progress, size_t total);

using tick_t = decltype(std::chrono::high_resolution_clock::now());

static tick_t stopwatch() {
	return std::chrono::high_resolution_clock::now();
}

static auto dur_ms(tick_t begin, tick_t end) {
	return std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
}

