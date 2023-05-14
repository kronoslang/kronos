#pragma once

#include "stm.h"
#include <cassert>

namespace pcoll {
	namespace detail {
		template <typename RETURN> struct void_is_true {
			template <typename FN, typename... ARGS>
			bool operator()(const FN& fn, ARGS&&... args) const {
				return fn(std::forward<ARGS>(args)...);
			}
		};

		template <> struct void_is_true<void> {
			template <typename FN, typename... ARGS>
			bool operator()(const FN& fn, ARGS&&... args) const {
				fn(std::forward<ARGS>(args)...);
				return true;
			}
		};

		template <typename T, typename P, thread_policy THREAD_POLICY = multi_threaded, typename CMP_LESS = std::less<T>> 
		struct treap_node : public reference_counted<THREAD_POLICY> {
			using node_t = treap_node<T, P, THREAD_POLICY, CMP_LESS>;
			using value_t = T;
			using hash_t = P;
			using cmplt_t = CMP_LESS;
			using ref_t = ref<const treap_node, THREAD_POLICY>;

			ref_t left;
			ref_t right;
			T value;
			hash_t priority;

			void dispose() const {
				delete this;
			}

			treap_node(ref_t l, ref_t r, T val, hash_t p)
				:left(std::move(l)),
				right(std::move(r)),
				value(std::move(val)),
				priority(p) {}

			static ref_t promote_left(const ref_t& left, const ref_t& right, const value_t& value, hash_t priority) {
				return new treap_node{
					left->left,
					new treap_node{
						left->right,
						right,
						value,
						priority
						},
					left->value,
					left->priority
				};
			}

			static ref_t promote_right(const ref_t& left, const ref_t& right, const value_t& value, hash_t priority) {
				return new treap_node{
					new treap_node{
						left,
						right->left,
						value,
						priority
					},
					right->right,
					right->value,
					right->priority
				};

			}

			template <typename U>
			static ref_t rebalance(const ref_t& left, const ref_t& right, const U& value, hash_t priority) {
				if (!left.empty() && left->priority > priority) {
					return promote_left(left, right, value, priority);
				}
				if (!right.empty() && right->priority > priority) {
					return promote_right(left, right, value, priority);
				}
				return new treap_node{ left,right,value,priority };
			}

			template <typename U>
			static ref_t insert(const ref_t& node, const U& new_item, hash_t priority) {
				if (node.empty()) {
					return new treap_node{ ref_t(), ref_t(), new_item, priority };
				}

				cmplt_t comes_before;

				if (comes_before(new_item, node->value)) {
					return rebalance(insert(node->left, new_item, priority), node->right, node->value, node->priority);
				}
				if (comes_before(node->value, new_item)) {
					return rebalance(node->left, insert(node->right, new_item, priority), node->value, node->priority);
				}
				return node;
			}

			bool is_leaf() const {
				return !left->is_empty() || !right->is_empty();
			}

			static size_t count(const ref_t& node) {
				if (node.empty()) return 0;
				auto lc = count(node->left);
				auto rc = count(node->right);
				return 1 + lc + rc;
			}
			
			template <typename U>
			static value_t* get(const ref_t& node, const U& value) {
				cmplt_t comes_before;
				if (node.empty()) return nullptr;
				if (comes_before(value, node->value)) {
					return get(node->left, value);
				} else if (comes_before(node->value, value)) {
					return get(node->right, value);
				}
				return &node->value;
			}

			template <typename U>
			static value_t *get_below(const ref_t& node, const U& value, const value_t* c) {
				cmplt_t comes_before;
				if (node.empty()) return c;
				if (comes_before(value, node->value)) {
					return get_below(node->left, value, c);
				} else if (comes_before(node->value, value)) {
					return get_below(node->right, value, &node->value);
				}
				return c;
			}

			static const value_t& front(const treap_node* node) {
				while (!node->left.empty()) node = node->left.get();
				return node->value;
			}

			static const value_t& back(const treap_node* node) {
				while (!node->right.empty()) node = node->right.get();
				return node->value;
			}

			static ref_t pop_front(const ref_t& node, value_t& front) {
				if (node->left.empty()) {
					front = node->value;
					return node->right;
				}

				auto l = pop_front(node->left, front);
				return new treap_node{ l, node->right, node->value, node->priority };
			}

			static ref_t pop_back(const ref_t& node, value_t& back) {
				if (node->right.empty()) {
					back = node->value;
					return node->right;
				}

				auto r = pop_back(node->right, back);
				return new treap_node{ node->left, r, node->value, node->priority };
			}

			template <typename FN>
			static ref_t remove_if(const ref_t& node, const FN& pred) {
				if (node.empty()) return node;
							
				auto l = remove_if(node->left, pred);
				auto r = remove_if(node->right, pred);

				if (pred(node->value)) {
					if (l.empty()) return r;
					if (r.empty()) return l;
					if (l->priority > r->priority)
						return promote_left(l, r, node->value, node->priority);
					else
						return promote_right(l, r, node->value, node->priority);
				}

				if (l.get() != node->left.get() || r.get() != node->right.get()) {
					return new treap_node{ l, r, node->value, node->priority };
				} else {
					return node;
				}
			}

			static ref_t union_with(const ref_t& node, const ref_t& with) {
				if (with.empty()) return node;
				if (node.empty()) return with;
				cmplt_t comes_before;
				if (comes_before(node->value, with->value)) {
					auto ul = union_with(node->left, with);
					if (ul != node->left) {
						return new treap_node{ ul, node->right, node->value, node->priority };
					}
					return node;
				}
				if (comes_before(with->value, node->value)) {
					auto ur = union_with(node->right, with);
					if (ur != node->right) {
						return new treap_node{ node->left, ur, node->value, node->priority };
					}
					return node;
				}
				auto ul = union_with(node->left, with->left);
				auto ur = union_with(node->right, with->right);
				if (ul != node->left ||
					ur != node->right) {
					return new treap_node{ ul, ur, node->value, node->priority };
				}
				return node;
			}

			template <typename U>
			static ref_t remove(const ref_t& node, const U& item) {
				if (node.empty()) return node;

				cmplt_t comes_before;
				auto l = node->left;
				auto r = node->right;

				if (comes_before(item, node->value)) {
					l = remove(node->left, item);
				} else if (comes_before(node->value, item)) {
					r = remove(node->right, item);
				} else {
					if (l.empty()) {
						if (r.empty()) return ref_t();
						else return r;
					} else if (r.empty()) return l;
				
					if (l->priority > r->priority)
						return remove(promote_left(l, r, node->value, node->priority), item);
					else
						return remove(promote_right(l, r, node->value, node->priority), item);
				}

				return new treap_node{ l,r,node->value,node->priority };
			}

			template <typename U>
			static ref_t upper_bound(const ref_t& node, const U& bound, bool open) {
				if (node.empty()) return node;
				cmplt_t comes_before;
				if (comes_before(node->value, bound)) {
					if (node->left.empty()) return node;
					return new treap_node{ node->left, upper_bound(node->right, bound, open), node->value, node->priority };
				} else if (comes_before(bound, node->value) || open) {
					return upper_bound(node->left, bound, open);
				} else {
					return new treap_node{ node->left, ref_t(), node->value, node->priority };
				}
			}

			template <typename U>
			static ref_t lower_bound(const ref_t& node, const U& bound, bool open) {
				if (node.empty()) return node;
				cmplt_t comes_before;
				if (comes_before(node->value, bound)) {
					return lower_bound(node->right, bound, open);
				} else if (open || comes_before(bound, node->value)) {
					if (node->left.empty()) return node;
					return new treap_node{ lower_bound(node->left, bound, open), node->right, node->value, node->priority };
				} else {
					return new treap_node{ ref_t(), node->right, node->value, node->priority };
				}
			}

			template <typename U>
			static void pop_many_front(const ref_t& node, ref_t& front, ref_t& back, const U& limit) {
				if (node.empty()) {
					front = back = node;
				} else {
					cmplt_t comes_before;
					if (comes_before(node->value, limit)) {
						if (node->right.empty()) {
							front = node;
							back = {};
						} else {
							ref_t f;
							pop_many_front(node->right, f, back, limit);
							front = new treap_node{
								node->left,
								f,
								node->value,
								node->priority
							};
						}
					} else {
						if (node->left.empty()) {
							front = {};
							back = node;
						} else {
							ref_t b;
							pop_many_front(node->left, front, b, limit);
							back = new treap_node{
								b,
								node->right,
								node->value,
								node->priority
							};
						}
					}
				}
			}

			static void check(const ref_t& node) {
				cmplt_t comes_before;
				if (node->left.empty() == false) {
					assert(node->left->priority < node->priority);
					assert(comes_before(node->left->value, node->value));
					check(node->left);

				}
				if (node->right.empty() == false) {
					assert(node->right->priority < node->priority);
					assert(comes_before(node->value, node->right->value));
					check(node->right);
				}
			}

			template <typename FN>
			static bool for_each(const ref_t& node, const FN& body) {
				void_is_true<decltype(body(node->value))> flow;
				if (node.empty()) return true;;
				return 
					for_each(node->left, body) &&
					flow(body, node->value) &&
					for_each(node->right, body);
			}

			template <typename FN, typename U>
			static bool for_each_ge(const ref_t& node, const FN& body, const U& bound) {
				void_is_true<decltype(body(node->value))> flow;
				if (node.empty()) return true;;

				cmplt_t comes_before;
				if (comes_before(node->value, bound)) {
					return for_each_ge(node->right, body, bound);
				} 

				return
					for_each_ge(node->left, body, bound) &&
					flow(body, node->value) &&
					for_each(node->right, body);
			}
		};
	}

	template <typename T, typename CMP_LT = std::less<T>, typename HASHFN = std::hash<T>, detail::thread_policy THREAD_POLICY = detail::multi_threaded, detail::concurrent_strategy CONCURRENT_STRATEGY = detail::lockfree>
	class treap {
		using hash_t = decltype(std::declval<HASHFN>()(std::declval<T>()));
		using node_t = detail::treap_node<T, hash_t, THREAD_POLICY, CMP_LT>;
		using root_t = cref<node_t, CONCURRENT_STRATEGY>;
		root_t root;
		treap(root_t root) :root(std::move(root)) { }
	public:
		treap() = default;

		const void* identity() const {
			auto tmp = root.borrow();
			return tmp.get();
		}


		template <typename U>
		PCOLL_RETURN_VALUE_CHECK treap insert(const U& value) const {
			HASHFN hasher;
			auto tmp = root.borrow();
			return node_t::insert(tmp.get(), value, hasher(value));
		}

		template <typename U>
		PCOLL_RETURN_VALUE_CHECK treap remove(const U& value) const {
			auto tmp = root.borrow();
			return node_t::remove(tmp.get(), value);
		}

		PCOLL_RETURN_VALUE_CHECK treap pop_front(T& front_val) const {
			auto tmp = root.borrow();
			return node_t::pop_front(tmp.get(), front_val);
		}

		const T& front() const {
			auto tmp = root.borrow();
			return node_t::front(tmp.get());
		}

		PCOLL_RETURN_VALUE_CHECK treap pop_back(T& back_val) const {
			auto tmp = root.borrow();
			return node_t::pop_back(tmp.get(), back_val);
		}

		template <typename U>
		PCOLL_RETURN_VALUE_CHECK treap slice_above(const U& limit, bool include_limit) const {
			auto tmp = root.borrow();
			return node_t::lower_bound(tmp.get(), limit, !include_limit);
		}

		template <typename U>
		PCOLL_RETURN_VALUE_CHECK treap slice_below(const U& limit, bool include_limit) const {
			auto tmp = root.borrow();
			return node_t::upper_bound(tmp.get(), limit, !include_limit);
		}

		template <typename FN>
		PCOLL_RETURN_VALUE_CHECK treap remove_if(const FN& fn) const {
			auto tmp = root.borrow();
			return node_t::remove_if(tmp.get(), fn);
		}

		T back() const {
			auto tmp = root.borrow();
			return node_t::back(tmp.get());
		}

		size_t count(const T& value) const {
			auto tmp = root.borrow();
			return node_t::count(tmp.get(), value);
		}

		bool empty() const {
			return root.get() == nullptr;
		}

		size_t count() const {
			auto tmp = root;
			return node_t::count(tmp.get());
		}

		template <typename FN>
		bool for_each(const FN& body) const {
			auto tmp = root.borrow();
			return node_t::for_each(tmp.get(), body);
		}

		template <typename FN, typename U>
		bool for_each_ge(const U& bound, const FN& body) const {
			auto tmp = root.borrow();
			return node_t::for_each_ge(tmp.get(), body, bound);
		}

		void check_invariants() const {
			auto tmp = root;
			if (tmp.get()) node_t::check(tmp.get());
		}

		T pop_front() {
			T val;
			root.swap([&val](const node_t* root) {
				return node_t::pop_front(root, val);
			});
			return val;
		}

		template <typename U>
		T get_below(const U& reference, T fallback = T()) {
			auto tmp = root;
			auto bl = node_t::get_below(tmp, reference, nullptr);
			return bl ? *bl : fallback;
		}

		template <typename U>
		void insert_into(const U& value) {
			auto p = HASHFN()(value);
			root.swap([&value,p](const node_t* root) {
				return node_t::insert(root, value, p);
			});
		}

		template <typename FN>
		void remove_from(const FN& pred) {
			root.swap([&pred](const node_t* root) {
				return node_t::remove_if(root, pred);
			});
		}

		template <typename FN>
		void transaction(const FN& upd) {
			treap tmp;
			root.swap([&](const node_t* root) {
				tmp.root = root;
				tmp = upd(tmp);
				return tmp.root;
			});
		}

		treap union_with(const treap& other) {
			return node_t::union_with(root, other.root);
		}

		template <typename U>
		treap pop_up_to(const U& upTo) {			
			using node_ref = typename node_t::ref_t;
			node_ref pop;
			root.swap([&](const node_ref& prev) {
				node_ref remain;
				node_t::pop_many_front(prev, pop, remain, upTo);
				return remain;
			});
			return root_t(pop.get());
		}

		bool try_pop_front(T& val) {
			using node_ref = typename node_t::ref_t;
			treap tmp;
			bool success;
			root.swap([&](const node_t* root) -> root_t {
				success = root != nullptr;
				tmp.root = root;
				if (success) {
					tmp.root = node_t::pop_front(node_ref(root), val);
				}
				return tmp.root;
			});
			return success;
		}
	};
}