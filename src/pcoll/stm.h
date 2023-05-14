#pragma once

#include <atomic>
#include <mutex>
#include <limits>
#include <algorithm>
#include <thread>
#include "util.h"

namespace {
	
#ifdef _MSC_VER
#include <intrin.h>
	// hand-roll enough of std::atomic interface to work with cref
	// MSVC stl uses mutex for anything more than 8 bytes
	template <typename T> struct atomic128 {
		__declspec(align(16)) T data;
		static_assert(sizeof(T) == 16, "this atomic wrapper is only for 16-byte data types");

		bool cmpxchg16b(T& old, const T& desired) const {
			auto ptr64 = (__int64*)&data;
			__declspec(align(16)) auto tmp_in = old;
			__declspec(align(16)) auto tmp_out = desired;
			auto tmp_out_ptr = (__int64*)&tmp_out;
			auto tmp_in_ptr = (__int64*)&tmp_in;
			auto success = _InterlockedCompareExchange128(
				ptr64,
				tmp_out_ptr[1],
				tmp_out_ptr[0],
				tmp_in_ptr
			) != 0;
			old = tmp_in;
			return success;
		}

		template <typename FN>
		T transaction(const FN& update) const noexcept {
			for(;;) {
				auto tmp_in = data;
				auto tmp_out = update(tmp_in);
				if(cmpxchg16b(tmp_in, tmp_out)) {
					return tmp_in;
				}
			}
		}

	public:
		T load(std::memory_order) const noexcept {
			return transaction([](const T& data) noexcept {
				return data;
			});
		}

		void store(const T& data, std::memory_order) noexcept {
			transaction([&data](const T& old) noexcept {
				return data;
			});
		}

		bool compare_exchange_weak(T& expected, const T& desired, std::memory_order) noexcept {
			return cmpxchg16b(expected, desired);
		}

		bool compare_exchange_strong(T& expected, const T& desired, std::memory_order) noexcept {
			return cmpxchg16b(expected, desired);
		}

		T exchange(const T& incoming, std::memory_order) noexcept {
			T outgoing;
			transaction([&outgoing, &incoming](const T& current) {
				outgoing = current;
				return incoming;
			});
			return outgoing;
		}

		bool is_lock_free() const noexcept {
			return true;
		}
	};
#else
	template <typename T> using atomic128 = std::atomic<T>;
#endif
}

namespace pcoll {
	namespace profile {
#ifdef PCOLL_PROFILE_STM 
#define PROFILE(name) static int name(int change = 0) { static std::atomic<size_t> counter {0}; return counter.fetch_add(change); }
#else
#define PROFILE(name) static void name(int) {}
#endif
		PROFILE(stm_cref_spin_cycles)
		PROFILE(stm_transaction_conflicts)
		PROFILE(stm_weight_increases)
		PROFILE(stm_transactions)
#undef PROFILE
	}

	namespace detail {
		template <typename T> class locked_struct {
			mutable std::recursive_mutex lock;
			T data;
		public:
			void store(const T& nd, std::memory_order) noexcept {
				std::lock_guard<std::recursive_mutex> lg(lock);
				data = nd;
			}

			T load(std::memory_order) const noexcept {
				std::lock_guard<std::recursive_mutex> lg(lock);
				T tmp = data;
				return tmp;
			}

			T exchange(const T& nd, std::memory_order) noexcept {
				std::lock_guard<std::recursive_mutex> lg(lock);
				T tmp = data;
				data = nd;
				return tmp;
			}

			template <typename UPD>
			void transaction(const UPD& update) noexcept {
				std::lock_guard<std::recursive_mutex> lg(lock);
				data = update(data);
			}

			bool compare_exchange_strong(T& expected, const T& desired, std::memory_order) {
				std::lock_guard<std::recursive_mutex> lg(lock);
				if (memcmp(&data, &expected, sizeof(T)) == 0) {
					data = desired;
					return true;
				} else {
					expected = data;
					return false;
				}
			}

			bool compare_exchange_weak(T& expected, const T& desired, std::memory_order mo) {
				return compare_exchange_strong(expected, desired, mo);
			}

			bool is_lock_free() const noexcept {
				// it's lock free. trust me. it's one of the 
				// lock-free-est structures ever created.
				return true;
			}
		};

		template <typename T> struct no_rollback {
			void operator()(const T&, const T&) const {}
		};

		template <typename T, typename UPD, typename RB = no_rollback<T>>
		static void transaction(locked_struct<T>& data, const UPD& pure_update, const RB& rollback = no_rollback<T>()) {
			// locked transaction never fails
			data.transaction(pure_update);
			profile::stm_transactions(1);
		}

		template <typename T, typename UPD, typename RB = no_rollback<decltype(std::declval<T>().load(std::memory_order_acquire))>>
		static void transaction(T& data, const UPD& pure_update, const RB& rollback = no_rollback<decltype(std::declval<T>().load(std::memory_order_acquire))>()) {
			auto prev = data.load(std::memory_order_acquire);
			profile::stm_transactions(1);
			for(;;) {
				auto old = prev;
				auto updated = pure_update(prev);
				if(data.compare_exchange_weak(prev, updated, std::memory_order_acq_rel)) return;
				rollback(old, updated);
				profile::stm_transaction_conflicts(1);
			}
		}
	}

	namespace detail {
		template <typename T, detail::concurrent_strategy S> struct atomic_strategy {};
		template <typename T> struct atomic_strategy<T, detail::lockfree> { using data_t = atomic128<T>; };
		template <typename T> struct atomic_strategy<T, detail::locking> { using data_t = detail::locked_struct<T>; };
	}

	template <typename T, detail::concurrent_strategy STRATEGY = detail::lockfree>
	class cref {
		static_assert(T::threadsafe, "concurrent reference requires a thread safe reference count");

		using value_t = T;

#ifdef NDEBUG
		struct alignas(16) refdata_t {
			const T* arc;
			size_t weight;
		};
#else
		struct alignas(16) refdata_t {
			const T* arc = nullptr;
			size_t weight = 0;
		};
#endif


		using atomic_refdata_t = typename detail::atomic_strategy<refdata_t, STRATEGY>::data_t;
		alignas(16) mutable atomic_refdata_t refdata;

		static size_t add_weight(const detail::atomic_reference_counter *arc) {
			size_t to_add = 0;
			detail::transaction(arc->counter, [&](size_t weight) {
				auto biggest_power_of_2 = 1ull << (std::numeric_limits<size_t>::digits - 1);
				to_add = (size_t)std::max((biggest_power_of_2 - (weight >> 1)) >> 15, 0x100ull);
				assert((weight <= std::numeric_limits<size_t>::max() - to_add) && "out of reference count headroom");
				return weight + to_add;
			});
			profile::stm_weight_increases(1);
			return to_add;
		}

		static refdata_t yield_weight(atomic_refdata_t& from, refdata_t* remain = nullptr) {
			// the first transaction must always stop 'from' possibly
			// owning all the weight, because it might be immediately
			// disposed of by a racing thread

			refdata_t new_rd, remain_rd;
			bool need_more_weight;
			bool spinlock;

			for(;;) {
				detail::transaction(from, [&spinlock, &new_rd, &need_more_weight, &remain_rd](const refdata_t& rd) {
					if (!rd.arc) {
						spinlock = need_more_weight = false;
						remain_rd = new_rd = rd;
						return rd;
					} else {
						spinlock = rd.weight < 1;
						need_more_weight = rd.weight < 2;

						if (spinlock) {
							return new_rd = remain_rd = rd;
						}

						remain_rd = { rd.arc, rd.weight / 2 };
						new_rd    = { rd.arc, rd.weight - remain_rd.weight };

						assert(new_rd.weight >= remain_rd.weight);
						assert(new_rd.weight + remain_rd.weight == rd.weight);
						return remain_rd;
					}
				});
				if (!spinlock) break;
				profile::stm_cref_spin_cycles(1);
				std::this_thread::yield();
			}

			if (need_more_weight) {
				// the new references might not be able to delete the referee
				// due to having zero weight. must add weight in that case
				size_t  new_weight = add_weight(new_rd.arc);
				size_t to_old_reference = 0;

				detail::transaction(from, [&to_old_reference, new_weight, new_rd, &remain_rd](const refdata_t& rd) {
					// add weight unless 'from' was already mutated.
					if (rd.arc != new_rd.arc) {
						to_old_reference = 0;
						remain_rd = rd;
						return remain_rd;
					}
					// if it was, the previous reference was deleted
					// with zero weight. Give all new weight to the
					// new reference.

					to_old_reference = new_weight / 2;
					remain_rd = { rd.arc, rd.weight + to_old_reference };
					return remain_rd;
				});

				// add all the weight not grabbed by 'from'
				new_rd.weight += new_weight - to_old_reference;
			}

			if (remain) *remain = remain_rd;

			return new_rd;
		}

		void unsafe_reset() {
			refdata_t z{ nullptr, 0 };
			refdata.store(z, std::memory_order_release);
		}

		static void destroy(const refdata_t& rd) {
			if(rd.arc != nullptr && rd.weight != 0) {
				auto counter_was = rd.arc->counter.fetch_sub(rd.weight, std::memory_order_release);
				assert(counter_was >= rd.weight);
				if(counter_was == rd.weight) {
					// deletion
					atomic_thread_fence(std::memory_order_acquire);
					delete rd.arc;
				}
			}
		}

	public:
		cref(const T* src = nullptr) {
			if(src) {
				refdata.store(refdata_t{ src, add_weight(src)}, std::memory_order_release);
			} else {
				refdata.store(refdata_t{ nullptr, 0 }, std::memory_order_release);
			}
		}

		cref(const cref& from)  {
			// this is private to current thread, from is shared state
			refdata.store({ yield_weight(from.refdata) }, std::memory_order_release);
		}

		cref(cref&& from) {
			// we don't really need to care about concurrency because both
			// instances are private to this thread
			refdata.store(from.refdata.load(std::memory_order_acquire), std::memory_order_release);
			from.refdata.store({ nullptr, 0 }, std::memory_order_release);
		}

		template <typename U, detail::thread_policy P>
		cref(detail::ref<U, P> transfer):cref(transfer.get()) {
		}

		~cref() {
			destroy(refdata.load(std::memory_order_acquire));
		}

		void reset() {
			*this = cref();
		}

		T* get() const {
			return (T*)refdata.load(std::memory_order_acquire).arc;
		}

		size_t get_weight() const {
			return refdata.load(std::memory_order_acquire).weight;
		}

		cref& operator=(cref from) {
			// from is private to current thread, this is shared state
			auto nrd = from.refdata.load(std::memory_order_relaxed);
			auto rd = refdata.exchange(nrd, std::memory_order_acq_rel);

			from.refdata.store(rd, std::memory_order_relaxed);
			return *this;
		}

		T* operator->() const {
			return const_cast<cref*>(this)->get();
		}

		// this can point to shared state. argument must *not* be concurrently mutated.
		void exchange(cref& thread_private) {
			auto prev = refdata.exchange(
				thread_private.refdata.load(std::memory_order_acquire), 
				std::memory_order_acq_rel);
			thread_private.refdata.store(prev, std::memory_order_release);
		}

		template <typename UPD>
		void swap(const UPD& updater) {
			refdata_t hold;
			try {
				for (bool retry = true;;) {
					refdata_t expected, overwritten;

					hold = yield_weight(refdata, &expected);
					cref next = updater((T*)hold.arc);

					destroy(hold);
					hold.arc = nullptr;

					auto next_data = next.refdata.load(std::memory_order_relaxed);

					detail::transaction(refdata, [&](const refdata_t& rd) {
						overwritten = rd;
						retry = rd.arc != expected.arc;
						if (retry) {
							return rd;
						} else {
							overwritten = rd;
							return next_data;
						}
					});

					if (!retry) {
						next.refdata.store(overwritten, std::memory_order_relaxed);
						break;
					}
				}
			} catch (...) {
				destroy(hold);
				throw;
			}
		}

		class borrow_t {
			cref borrowed;
			const cref& from;
		public:
			borrow_t(const cref& from):from(from), borrowed(from) {
			}

			~borrow_t() {
				bool did_return;
				detail::transaction(from.refdata, [&](refdata_t rd) {
					did_return = rd.arc == borrowed.get();
					if (did_return) {
						rd.weight += borrowed.get_weight();
					}
					return rd;
				});

				if (did_return) {
					borrowed.refdata.store({ nullptr, 0 }, std::memory_order_relaxed);
				}
			}

			operator const cref&() const {
				return borrowed;
			}

			T* operator->() const {
				return borrowed.get();
			}

			T* get() const {
				return borrowed.get();
			}
		};

		borrow_t borrow() const {
			return borrow_t(*this);
		}
	};
}
