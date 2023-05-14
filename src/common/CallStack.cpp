#include "config/system.h"
#include "CallStack.h"

#ifndef KRONOS_NO_STACK_EXTENDER
#if HAVE_UCONTEXT_T
#include <ucontext.h>
#include <unistd.h>
#include <stdlib.h>

namespace K3 {
    static void FiberProc(void *raw) {
        auto s = (Stack*)raw;
        for (;;) {
            try {
                s->current();
            } catch (...) {
                s->uncaught_exception = std::current_exception();
            }
            swapcontext(s->fiber.get(), s->parent.get());
        }
    }
    
    Stack::Stack(size_t stackSize):fiber(std::make_unique<ucontext_t>()),parent(std::make_unique<ucontext_t>()),stackSize(stackSize) {
        getcontext(fiber.get());
        if (posix_memalign(&stackMemory, sysconf(_SC_PAGESIZE), stackSize)) {
            throw std::bad_alloc();
        }
        fiber->uc_stack.ss_size = stackSize;
        fiber->uc_stack.ss_sp = stackMemory;
        makecontext(fiber.get(), (void(*)())FiberProc, 1, this);
    }

    Stack::~Stack() {
        if (stackMemory) free(stackMemory);
    }
    
    void Stack::Execute(std::function<void(void)> body) {
        std::swap(current, body);
        
        getcontext(parent.get());
        swapcontext(parent.get(), fiber.get());
        
        if (uncaught_exception) {
            std::rethrow_exception(uncaught_exception);
        }
    }

	size_t Stack::StackAvail() const {
		// assume stack grows down
		char dummy = 0;
		return &dummy - (char*)stackMemory;
	}
}


#elif HAVE_CREATEFIBEREX

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>

namespace K3 {
	static void CALLBACK FiberProc(void*);
	static size_t GetPageSize();

	Stack::Stack(size_t stackSize) 
		:fiber(CreateFiberEx(GetPageSize(), stackSize, FIBER_FLAG_FLOAT_SWITCH, FiberProc, this)), 
		 parent(nullptr), stackSize(stackSize), stackMemory(nullptr) 
	{
		if (fiber == nullptr) throw std::bad_alloc();
	}

	size_t Stack::StackAvail() const {
		// stack grows down
		char dummy = 0;
		return &dummy - (char*)stackMemory;
	}

	Stack::~Stack() {
		if (fiber) DeleteFiber(fiber);
	}

	void Stack::Execute(std::function<void(void)> body) {
		std::swap(current, body);
		parent = ConvertThreadToFiber(nullptr);

		if (parent == nullptr) {
			if (GetLastError() == ERROR_ALREADY_FIBER) {
				parent = GetCurrentFiber();
			} else {
				throw std::runtime_error("Couldn't start fiber pool.");
			}
		}

		SwitchToFiber(fiber);

		if (uncaught_exception) {
			std::exception_ptr rethrow;
			std::swap(rethrow, uncaught_exception);
			std::rethrow_exception(rethrow);
		}
	}

	void CALLBACK FiberProc(void* raw) {
		auto s = (Stack*)raw;
		s->stackMemory = (void*)(256 + &s - s->stackSize);
		for (;;) {
		#if _HAS_EXCEPTIONS
			try {
		#endif
				s->current();
		#if _HAS_EXCEPTIONS
			} catch (...) {
				s->uncaught_exception = std::current_exception();
			}
		#endif
			SwitchToFiber(s->parent);
		}
	}


	size_t GetPageSize() {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		return si.dwPageSize;
	}
}

#endif

namespace K3 {
    Stack::Stack(Stack&& from) {
        std::swap(fiber, from.fiber);
        std::swap(parent, from.parent);
        std::swap(uncaught_exception, from.uncaught_exception);
        std::swap(current, from.current);
        std::swap(stackMemory, from.stackMemory);
    }
}
#endif
