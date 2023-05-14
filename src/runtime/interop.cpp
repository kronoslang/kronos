#include "kronos.h"
#include "kronosrtxx.h"
#include <thread>

KRONOS_ABI_EXPORT int64_t kvm_print_init(int64_t world, const char* descr, const void* data) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_print(int64_t world, const char *pipe, const char* descr, const void* data) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	if (env) env->ToOut(pipe, descr, data);
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_sleep_init(int64_t world, double time) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_sleep(int64_t world, double time) {
	std::this_thread::sleep_for(std::chrono::microseconds((std::int64_t)(time * 1000000)));
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_branch_init(int64_t world,
										  int64_t truthTy, const void* truthVal,
										  int64_t thenTy, const void* thenVal,
										  int64_t elseTy, const void* elseVal) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_branch(int64_t world,
									 int64_t truthSz, const void* truthVal,
									 int64_t thenSz, int64_t thenTy, const void* thenVal,
									 int64_t elseSz, int64_t elseTy, const void* elseVal) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	if (env) {
		auto truthBytes = (const char*)truthVal;
        auto timeStamp = env->Now();
		while (truthSz--) {
			if (*truthBytes++) goto isTrue;
		}
		env->Run(timeStamp, elseTy, elseVal, elseSz);
        return world;
	isTrue:
		env->Run(timeStamp, thenTy, thenVal, thenSz);
		return world;
	}
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_schedule_init(int64_t world, int64_t timestamp, int64_t sz, int64_t type, const void* data) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_schedule(int64_t world, int64_t timestamp, int64_t sz, int64_t type, const void* data) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	if (env) {
		env->Run(timestamp, type, data, sz);
	}
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_render_init(int64_t world, const char* audioFile, int64_t closureTy, const void* closureData, float sr, int64_t numFrames) {
    return world;
}

KRONOS_ABI_EXPORT int64_t kvm_render(int64_t world, const char* audioFile, int64_t closureTy, const void* closureData, float sr, int64_t numFrames) {
    auto env = (Kronos::Runtime::IEnvironment*)world;
    if (env) {
        env->Render(audioFile, closureTy, closureData, sr, numFrames);
    }
    return world;
}

KRONOS_ABI_EXPORT int64_t kvm_now(int64_t world) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	auto now = env ? env->Now() : 0;
	return now;
}

KRONOS_ABI_EXPORT float kvm_scheduler_rate(int64_t world) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	auto rate = env ? env->SchedulerRate() : 0.0f;
	return rate;
}

KRONOS_ABI_EXPORT int64_t kvm_start_init(int64_t world, int64_t sz, int64_t closureType, const void* closureData) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_start(int64_t world, int64_t sz, int64_t closureType, const void* closureData) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	if (env) {
		env->Start(closureType, closureData, (size_t)sz);
	}
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_stop_init(int64_t world, int64_t sz, int64_t closureType, const void* closureData) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_stop(int64_t world, int64_t id) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	if (env) {
		env->Stop(id);
	}
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_pop(int64_t world, int64_t ty, void* item) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	if (env) {
		env->Pop(ty, item);
	}
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_pop_init(int64_t world, int64_t ty , const void* item) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_push(int64_t world, int64_t ty, const void* item) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	if (env) {
		env->Push(ty, item);
	}
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_push_init(int64_t world, int64_t ty, void* item) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_drop_init(int64_t world) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_drop(int64_t world) {
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_dispatch(int64_t world, int64_t instance, const char* sym, size_t argSz, const void* argData, const char *argSign, void* result) {
	auto env = (Kronos::Runtime::IEnvironment*)world;
	if (env) {
		auto child = env->GetChild(instance);
		if (!child.empty()) {
			auto idx = child->GetSymbolIndex({ sym, argSign });
			if (idx >= 0)
				env->DispatchTo(child.get(), idx, argData, argSz, result == argData ? nullptr : result);
		}		
	}
	return world;
}

KRONOS_ABI_EXPORT int64_t kvm_dispatch_init(int64_t world, int64_t instance, const char* sym, size_t argSz, const void* argData, const char *argSign) {
	return world;
}

#ifndef _MSC_VER
KRONOS_ABI_EXPORT void** link_kvm(void *ptr) __attribute__((used));
#else
#define MARK_USED
#endif

KRONOS_ABI_EXPORT void** link_kvm(void *ptr) {
	// this function pulls the api in as a dependency
	static void *api[] = {
		(void*)kvm_print,
		(void*)kvm_print_init,
		(void*)kvm_branch,
		(void*)kvm_branch_init,
		(void*)kvm_schedule,
		(void*)kvm_schedule_init,
		(void*)kvm_start,
		(void*)kvm_start_init,
		(void*)kvm_stop,
		(void*)kvm_stop_init,
		(void*)kvm_pop,
		(void*)kvm_pop_init,
		(void*)kvm_push,
		(void*)kvm_push_init,
		(void*)kvm_dispatch,
		(void*)kvm_dispatch_init,
		(void*)kvm_scheduler_rate,
		(void*)kvm_now,
		(void*)kvm_sleep,
		(void*)kvm_sleep_init,
        (void*)kvm_render,
        (void*)kvm_render_init
	};
	return api;
}

