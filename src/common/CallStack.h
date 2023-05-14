#pragma once

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 600
#endif

#include <stdexcept>
#include <functional>

#ifdef WIN32
using fiber_t = void*;
#else
#include "config/system.h"
#if HAVE_UCONTEXT_T
#include <ucontext.h>
#include <memory>
using fiber_t = std::unique_ptr<ucontext_t>;
#else
#define KRONOS_NO_STACK_EXTENDER
#endif
#endif

namespace K3 {
#ifndef KRONOS_NO_STACK_EXTENDER
	struct Stack {
		fiber_t fiber;
		fiber_t parent;
        void *stackMemory = nullptr;
		size_t stackSize = 0;
		std::exception_ptr uncaught_exception;
		std::function<void(void)> current;
		Stack(size_t stackSize);
		Stack(const Stack&) = delete;
		Stack(Stack&&);
		~Stack();
		void Execute(std::function<void(void)> body);
		Stack& operator=(const Stack&) = delete;
		size_t StackAvail() const;
	};
#endif
}
