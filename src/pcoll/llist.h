#pragma once
#include "stm.h"
#include <xmmintrin.h>

#define ALIGNED_NEW_DELETE(T) \
void* operator new(size_t i) { \
	return _mm_malloc(i, alignof(T)); \
} \
void operator delete(void *p) { \
	_mm_free(p); \
}


namespace pcoll {
	namespace detail {
		template <typename T>
		struct node : public detail::atomic_reference_counter {
			using ref_t = cref<node>;
			T value;
			ref_t next;
			node(T v, ref_t n) :value(std::move(v)), next(std::move(n)) {}
			ALIGNED_NEW_DELETE(node<T>)
		};
	}

	template <typename T>
	class llist {
		using node_t = detail::node<T>;
		using node_ref = typename node_t::ref_t;
		node_ref head;
		llist(node_ref h) :head(std::move(h)) { }
	public:

		llist() { }
		llist(T value, node_ref next = node_ref()) :head(new node_t{ std::move(value), next }) { }

		llist begin() const {
			return *this;
		}

		llist end() const {
			return {};
		}

		bool operator==(const llist& rhs) const {
			return head.get() == rhs.head.get();
		}

		bool operator!=(const llist& rhs) const {
			return head.get() != rhs.head.get();
		}

		const T& operator*() const {
			return head->value;
		}

		T& operator*() {
			return head->value;
		}

		llist operator++() {
			head = head->next;
			return *this;
		}
		
		llist operator++(int) {
			llist tmp = *this;
			head = head->next;
			return tmp;
		}

		llist push_front(T value) const {
			return { std::move(value), head };
		}
	};
}