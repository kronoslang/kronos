#pragma once

#include <atomic>
#include <utility>
#include <type_traits>
#include <cstdint>
#include <cstddef>
#include <cassert>

#ifdef __GNUC__
#define PCOLL_RETURN_VALUE_CHECK __attribute__((warn_unused_result))
#define clz(x) (x?__builtin_clz(x):32)
#define ctz(x) (x?__builtin_ctz(x):32)
#elif defined(_MSC_VER)
#include <intrin.h>
#define PCOLL_RETURN_VALUE_CHECK _Check_return_
uint32_t __inline ctz(uint32_t value) {
	unsigned long trailing_zero = 0;

	if(_BitScanForward(&trailing_zero, value)) {
		return trailing_zero;
	} else {
		return 32;
	}
}

uint32_t __inline clz(uint32_t value) {
	unsigned long leading_zero = 0;

	if(_BitScanReverse(&leading_zero, value)) {
		return 31 - leading_zero;
	} else {
		return 32;
	}
}
#else
#define PCOLL_RETURN_VALUE_CHECK __attribute__((warn_unused_result))
static uint32_t ALWAYS_INLINE popcnt(uint32_t x) {
	x -= ((x >> 1) & 0x55555555);
	x = (((x >> 2) & 0x33333333) + (x & 0x33333333));
	x = (((x >> 4) + x) & 0x0f0f0f0f);
	x += (x >> 8);
	x += (x >> 16);
	return x & 0x0000003f;
}
static uint32_t ALWAYS_INLINE clz(uint32_t x) {
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
	return 32 - popcnt(x);
}
static uint32_t ALWAYS_INLINE ctz(uint32_t x) {
	return popcnt((x & -x) - 1);
}

#endif

namespace pcoll {
	namespace detail {
		enum concurrent_strategy {
			locking,
			lockfree
		};

		enum thread_policy {
			single_threaded,
			multi_threaded
		};
	}

	template <typename T, detail::concurrent_strategy S> class cref;
	
	namespace detail {
		template <typename BASE> class noncopying : public BASE {
		public:
			noncopying() {}

			noncopying(const noncopying& from) {}
			noncopying(noncopying&& from) {}

			noncopying& operator=(const noncopying&) { return *this; }
			noncopying& operator=(noncopying&&) { return *this; }
		};

		class plain_reference_counter {
			mutable int counter;
		public:
			static constexpr bool threadsafe = false;

			plain_reference_counter(int initial) :counter(initial) {}

			void retain() const {
				++counter;
			}

			bool release() const {
				if (--counter == 0) return true;
				return false;
			}

			void reset_reference_count(int to) const { 
				counter = to;
			}
		};

		class atomic_reference_counter {
			template <typename T, concurrent_strategy S> friend class pcoll::cref;
			mutable std::atomic<size_t> counter;
		public:

			static constexpr bool threadsafe = true;

			atomic_reference_counter(int initial = 0) { counter.store(initial, std::memory_order_relaxed); }

			void retain() const {
				counter.fetch_add(1, std::memory_order_relaxed);
			}

			bool release() const {
				if(counter.fetch_sub(1, std::memory_order_release) == 1) {
					atomic_thread_fence(std::memory_order_acquire);
					return true;
				}
				return false;
			}

			void reset_reference_count(int to) const {
				counter.store(to);
			}

			size_t get_count() const {
				return counter.load(std::memory_order_relaxed);
			}
		};

		struct disposable {
			virtual ~disposable() { }
			virtual void dispose() const {
				delete this;
			}
		};

		static inline std::uint32_t popcnt(std::uint32_t mask) {
#ifdef _MSC_VER
#ifdef __AVX__
			return _mm_popcnt_u32(mask);
#else
			std::uint32_t v = mask; // count bits set in this (32-bit value)
			std::uint32_t c; // store the total here
			static const std::uint32_t S[] = { 1, 2, 4, 8, 16 }; // Magic Binary Numbers
			static const std::uint32_t B[] = { 0x55555555, 0x33333333, 0x0F0F0F0F, 0x00FF00FF, 0x0000FFFF };

			c = v - ((v >> 1) & B[0]);
			c = ((c >> S[1]) & B[1]) + (c & B[1]);
			c = ((c >> S[2]) + c) & B[2];
			c = ((c >> S[3]) + c) & B[3];
			c = ((c >> S[4]) + c) & B[4];
			return c;
#endif
#else
			return __builtin_popcount(mask);
#endif
		}



		template <thread_policy> struct reference_counted{ };

		template <> struct reference_counted<single_threaded> : public noncopying<plain_reference_counter> { };
		template <> struct reference_counted<multi_threaded> : public noncopying<atomic_reference_counter> { };

		template <typename T, thread_policy THREAD_POLICY = multi_threaded>
		class ref { };

		template <typename T>
		class ref<T, multi_threaded> {
			template <typename U, concurrent_strategy S> friend class cref;
			std::atomic<T*> aptr;
			void set(T* nv) {
				aptr.store(nv, std::memory_order_relaxed);
			}

			static void release(T* ptr) {
				if (ptr->release()) {
					ptr->dispose();
				}
			}

		public:
			T* get() const {
				return aptr.load(std::memory_order_relaxed);
			}

			ref(T* ptr = nullptr) { if(ptr) ptr->retain(); set(ptr); }

			ref(const ref<T>& from) {
				auto ptr = from.get();
				if(ptr) ptr->retain();
				assert(from.get() == ptr);
				set(ptr);
			}

			ref(ref<T>&& from) {
				set(from.get());
				from.set(nullptr);
			}

			~ref() {
				auto ptr = get();
				if (ptr) release(ptr);
			}

			ref& operator=(ref from) {
				from.set(aptr.exchange(from.aptr));
				return *this;
			}

			template <typename U>
			operator ref<U>() const {
				return ref<U>(get());
			}

			T* operator->() const {
				return get();
			}
				
			template <typename... CONS_ARGS>
			static ref make(CONS_ARGS&&... cons_args) {
				return ref(new T(std::forward<CONS_ARGS>(cons_args)...));
			}

			bool empty() const {
				return get() == nullptr;
			}

			template <typename U, thread_policy P>
			bool operator==(const ref<U,P>& rhs) const {
				return get() == rhs.get();
			}

			void unsafe_reset() const {
				set(nullptr);
			}

		};

		template <typename T>
		class ref<T, single_threaded> {
			friend class ref<T, multi_threaded>;
			T* ptr;
			void set(T* nv) {
				ptr = nv;
			}

			static void release(T* ptr) {
				if (ptr->release()) {
					ptr->dispose();
				}
			}

		public:
			T* get() const {
				return ptr;
			}

			ref(T* ptr = nullptr) { if(ptr) ptr->retain(); set(ptr); }

			ref(const ref& from) {
				auto ptr = from.get();
				if(ptr) ptr->retain();
				set(ptr);
			}

			ref(ref&& from) {
				set(from.get());
				from.set(nullptr);
			}

			~ref() {
				auto ptr = get();
				if(ptr) release(ptr);
			}

			ref& operator=(ref from) {
				std::swap(ptr, from.ptr);
				return *this;
			}

			T* operator->() const {
				return get();
			}

			template <typename... CONS_ARGS>
			static ref make(CONS_ARGS&&... cons_args) {
				return ref(new T(std::forward<CONS_ARGS>(cons_args)...));
			}

			bool empty() const {
				return get() == nullptr;
			}

			template <typename U, thread_policy P>
			bool operator==(const ref<U, P>& rhs) const {
				return get() == rhs.get();
			}

			template <typename UPDATE_FN>
			void atomic_update(const UPDATE_FN& update) {
				set(update(get()));
			}
		};

		template <typename T, size_t MAX>
		class constrained_array {
            typename std::aligned_storage<sizeof(T), alignof(T)>::type mem[MAX];
			int capacity = 0;
		public:
			constrained_array() { }

			template <size_t FROM_MAX>
			constrained_array(const constrained_array<T, FROM_MAX>& from) {
				capacity = from.capacity;
				assert(capacity <= MAX);
				for (int i = 0; i < capacity; ++i) {
					operator[](i).T(from[i]);
				}
			}

			template <size_t FROM_MAX>
			constrained_array(constrained_array<T, FROM_MAX>&& from) {
				capacity = from.capacity;
				assert(capacity <= MAX);
				for (int i = 0; i < capacity; ++i) {
					operator[](i).T(std::move(from[i]));
				}
			}

			template <size_t FROM_MAX>
			constrained_array& operator=(const constrained_array<T, FROM_MAX>& from) {
				auto old_capacity = capacity;
				capacity = from.capacity;
				assert(capacity <= MAX);
				int i = 0;
				for(; i < capacity; ++i) {
					operator[](i) = from[i];
				}
				for(; i < old_capacity; ++i) {
					operator[](i).~T();
				}
			}

			template <size_t FROM_MAX>
			constrained_array& operator=(constrained_array<T, FROM_MAX>&& from) {
				auto old_capacity = capacity;
				capacity = from.capacity;
				int i = 0;
				for(; i < capacity; ++i) {
					operator[](i) = std::move(from[i]);
				}
				for(; i < old_capacity; ++i) {
					operator[](i).~T();
				}
			}

			~constrained_array() {
				for(int i = 0; i < capacity; ++i) {
					operator[](i).~T();
				}
			}

			T& operator[](int index) {
				return *(T*)&mem[index];
			}

			const T& operator[](int index) const {
				return *(const T*)&mem[index];
			}

			template <typename... ARGS>
			void emplace_back(ARGS&&... cargs) {
				assert(capacity < MAX);
				new (data()+capacity++) T(std::forward<ARGS>(cargs)...);
			}

			template <size_t SZ>
			void append(const constrained_array<T, SZ>& with) {
				for(auto& item : with) emplace_back(item);
			}

			template <size_t RESULT_SZ = MAX, size_t SZ = MAX>
			constrained_array<T, MAX> concat(const constrained_array<T, SZ>& with) const {
				constrained_array<T, MAX> tmp;
				tmp.append(*this); tmp.append(with);
				return tmp;
			}

			T* begin() {
				return data();
			}

			T* end() {
				return data() + capacity;
			}

			const T* begin() const {
				return data();
			}

			const T* end() const {
				return data() + capacity;
			}

			T* data() {
				return (T*)mem;
			}

			const T* data() const {
				return (const T*)mem;
			}

			size_t size() const {
				return capacity;
			}
		};

		template <typename ELEMENT, size_t MAX_SIZE, thread_policy THREAD_POLICY = multi_threaded>
		struct managed_constrained_array : public constrained_array<ELEMENT, MAX_SIZE>, reference_counted<THREAD_POLICY> { 
			using super_t = constrained_array<ELEMENT, MAX_SIZE>;
			template <size_t FROM_MAX>
			managed_constrained_array(const managed_constrained_array<ELEMENT, FROM_MAX>& from):super_t(from) { }

			template <size_t FROM_MAX>
			managed_constrained_array(managed_constrained_array<ELEMENT, FROM_MAX>&& from):super_t(std::move(from)) { }

			managed_constrained_array() { }
		};

		template <typename U> struct maybe_helper {
			template <typename FN, typename OPT>
			OPT operator()(const FN& fn, const OPT& o, const OPT& fallback) const {
				if (!o.has_value) return fallback;
				return fn(o.value);
			}
		};

		template <> struct maybe_helper<void> {
			template <typename FN, typename OPT>
			void operator()(const FN& fn, const OPT& o, const OPT& fallback) const {
				if (o.has_value) fn(o.value);
			}
		};
	}

	template <typename T>
	class optional {
	public:
		T value;
		bool has_value;
		optional():has_value(false) { }
		optional(const T& v) :value(v), has_value(true) { }
		optional(const T* ptr):has_value(ptr != nullptr) {
			if(ptr) value = *ptr;
		}

		const T& operator*() const {
			// TODO: throw if no value
			return value;
		}

		template <typename FN>
		auto maybe(FN fn, optional fallback = {}) {
			detail::maybe_helper<decltype(fn(value))> mb;
			return mb(fn, *this, fallback);
		}
	};

	struct none {
		template <typename T> operator optional<T>() const {
			return optional<T>();
		}
	};

	template <typename T> static bool has_value(const T&) {
		return true;
	}

	static bool has_value(none) {
		return false;
	}

	template <typename T> static bool has_value(const optional<T>& o) {
		return o.has_value;
	}

	template <typename T> static const T& get_value(const T& v) {
		return v;
	}

	template <typename T> static const T& get_value(const optional<T>& o) {
		return o.value;
	}
}
