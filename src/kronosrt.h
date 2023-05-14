#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

	typedef void* krt_instance;

	struct krt_sym;
	struct krt_class;

	typedef void(*krt_process_call)(krt_instance, void* output, int32_t numFrames);
	typedef int64_t(*krt_get_size_call)();
	typedef void(*krt_evaluate_call)(krt_instance, const void* input, void *output);
	typedef void(*krt_constructor_call)(krt_instance, const void* input);
	typedef void(*krt_destructor_call)(krt_instance);
	typedef void**(*krt_get_slot_call)(krt_instance, int32_t slot_index);
	typedef void(*krt_dispose_class_call)(struct krt_class*);
	typedef void(*krt_configure_call)(int32_t slot_index, const void* data);
	typedef krt_class*(krt_metadata_call)();

#define KRT_FLAG_NO_DEFAULT		1
#define KRT_FLAG_DRIVES_OUTPUT  2
#define KRT_FLAG_BLOCK_INPUT	4

#define KRT_SYM_SPEC(SEP) \
	F(const char*, sym) SEP \
	F(const char*, type_descriptor) SEP \
	F(krt_process_call, process) SEP \
	F(int64_t, size) SEP \
	F(int32_t, slot_index) SEP \
	F(int32_t, flags) 

#define KRT_CLASS_SPEC(SEP) \
	F(krt_configure_call, configure) SEP \
	F(krt_get_size_call, get_size) SEP \
	F(krt_constructor_call, construct) SEP \
	F(krt_get_slot_call, var) SEP \
	F(krt_evaluate_call, eval) SEP \
	F(krt_destructor_call, destruct) SEP \
	F(krt_dispose_class_call, dispose_class) SEP \
	F(const char*, eval_arg_type_descriptor) SEP \
	F(const char*, result_type_descriptor) SEP \
	F(void*, pimpl) SEP \
	F(int64_t, eval_arg_size) SEP \
	F(int64_t, result_type_size) SEP \
	F(int32_t, num_symbols) 

	// packed structs from specs above
#pragma pack(push)
#pragma pack(1)
#pragma warning(disable: 4200)
#define F(T, L) T L
	struct krt_sym {
		KRT_SYM_SPEC(;);
	};

	struct krt_class {
		KRT_CLASS_SPEC(;);
		krt_sym symbols[];
	};
#undef F
#pragma pack(pop)
#ifdef __cplusplus
}
#endif