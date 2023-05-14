#include "lithe/lithe.h"
#include "lithe/grammar/kronos.h"
#include <cctype>
#include <string>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <cassert>
#include <algorithm>
#include <stack>
#include <sstream>
#include <cstdlib>

#if (defined(__EXCEPTIONS) && __EXCEPTIONS) || (defined(__cpp_exceptions) && __cpp_exceptions == 199711)
#define HAVE_EXCEPTIONS 1
#else 
#define HAVE_EXCEPTIONS 0
#endif 

lithe::rule grammar() {
	static lithe::rule g = lithe::grammar::kronos::parser();
	return g;
}

lithe::node kronos_parse(const std::string& src) {
	static std::string text;

	text = src;
	const char* B = text.data();
	const char *E = B + text.size();
	auto parseTree = (*grammar())(B, E);
	return parseTree;
}

#include <emscripten/bind.h>

#define GUARD_SIZE 256

extern "C" {
	void* vnr_malloc(size_t bytes) {
#ifdef NDEBUG
		return malloc(bytes);
#else
		void* ptr = malloc(bytes + GUARD_SIZE * 2);
		memset(ptr, 0xcdcdcdcd, bytes + GUARD_SIZE * 2);
		*(size_t*)ptr = bytes;
		return (char*)ptr + GUARD_SIZE;
#endif
	}

	void vnr_free(void* ptr) {
		unsigned char* bytes = (unsigned char*)ptr;
#ifndef NDEBUG
		bytes -= GUARD_SIZE;
		size_t allocSz = *(size_t*)bytes;
		bool valid = true;
		for (int i = 0; i < GUARD_SIZE; ++i) {
			if ((i > sizeof(size_t) && bytes[i] != 0xcd) ||
				(bytes[i + GUARD_SIZE + allocSz] != 0xcd)) {
				std::clog << "! Malloc guard thrashed at " << (i - GUARD_SIZE) << " !\n";
				valid = false;
			}
		}
		if (!valid) abort();
#endif
		free(bytes);
	}

	void vnr_memset(void* dst, int src, size_t bytes) {
		memset(dst, src, bytes);
	}
}

using namespace emscripten;
EMSCRIPTEN_BINDINGS(kronos_parser) {
	class_<lithe::node>("AST")
		.function("get_string", &lithe::node::get_string)
		.function("is_error", &lithe::node::is_error)
		.function("size", &lithe::node::size)
		.function("get_child", &lithe::node::get_child)
		.function("get_header", &lithe::node::get_header);
	function("parse", &kronos_parse);
}