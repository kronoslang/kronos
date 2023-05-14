#pragma once

#include "stm.h"
#include <cstdint>
#include <cassert>
#include <cstddef>
#include <algorithm>
#include <vector>
#include <stdlib.h>

#ifdef __APPLE__
#include <alloca.h>
#endif

namespace pcoll {
	namespace detail {
		using std::pair;

		template <typename BITMAP, typename ELEMENT> 
		struct bitmap_array {
			using index_t = std::uint32_t;
			using bitmap_t = BITMAP;
			using element_t = ELEMENT;

			static constexpr size_t full_array_size = sizeof(BITMAP) * 8;
			static constexpr index_t index_mask = full_array_size - 1;
			using my_t = bitmap_array<BITMAP, ELEMENT>;

			BITMAP bitmap;
			typename std::aligned_storage<sizeof(ELEMENT), alignof(ELEMENT)>::type data[full_array_size];

			bool empty() const {
				return bitmap == 0;
			}

			ELEMENT& physical(index_t i) {
				return *((ELEMENT*)&data[i]);
			}

			const ELEMENT& physical(index_t i) const {
				assert(i < count());
				return *((const ELEMENT*)&data[i]);
			}

			ELEMENT& logical(index_t i) {
				auto offset = popcnt(((1u << i) - 1) & bitmap);
				return physical(offset);
			}

			const ELEMENT& logical(index_t i) const {
				auto offset = popcnt(((1u << i) - 1) & bitmap);
				return physical(offset);
			}
			// punned lifecycle
			void destroy() {
				for (int i = 0, e = count(); i < e; ++i) {
					physical(i).~element_t();
				}
			}

			void construct(const bitmap_array& from) {
				bitmap = from.bitmap;
				for (int i = 0, e = count(); i < e; ++i) {
					new ((void*)&physical(i)) element_t(from.physical(i));
				}
			}

			void construct(bitmap_array&& from) {
				bitmap = from.bitmap;
				for(int i = 0, e = count(); i < e; ++i) {
					new ((void*)&physical(i)) element_t(from.physical(i));
				}
			}

			static size_t minimal_size(index_t for_num_elements) {
				constexpr size_t element_size = ((sizeof(ELEMENT) + alignof(ELEMENT) - 1) / alignof(ELEMENT)) * alignof(ELEMENT);
				return offsetof(my_t, data) + element_size * for_num_elements;
			}

			size_t minimal_size() const noexcept {
				return minimal_size(count());
			}

			index_t count() const noexcept {
				return popcnt(bitmap);
			}

			bool has_element(index_t at) const {
				return (bitmap & (1u << at)) != 0;
			}

			// combined insertion/removal constructor
			// the booleans should help the compiler to generate 
			// economic versions for removal or insertion only				
			template <typename I, typename J, bool DO_INSERT = true, bool DO_REMOVE = true>
			index_t with_without(bitmap_array& dest, index_t& source_index, index_t& dest_index, I insert_begin, I insert_end, J remove_begin, J remove_end) const noexcept {

				if(DO_INSERT && insert_begin == insert_end) {
					// nothing to insert
					return with_without<I, J, false, DO_REMOVE>(dest, source_index, dest_index, insert_begin, insert_end, remove_begin, remove_end);
				}

				if(DO_REMOVE && remove_begin == remove_end) {
					// nothing to remove
					return with_without<I, J, DO_INSERT, false>(dest, source_index, dest_index, insert_begin, insert_end, remove_begin, remove_end);
				}

				auto i = insert_begin;
				auto j = remove_begin;
				while (DO_INSERT || DO_REMOVE) {
					if(DO_REMOVE) {
						if(!DO_INSERT || i == insert_end || *j <= i->first) {
							// next change is removal
							auto remove_index = *j++;
							auto count_before = popcnt(bitmap & ((1u << remove_index) - 1));
							assert(count_before <= count());
							// copy construct until removal point
							while(source_index < count_before) {
								new ((void*)&dest.physical(dest_index++)) ELEMENT(physical(source_index++));
							}
							// skip source element
							source_index++;
							dest.bitmap &= ~(1u << remove_index);

							if(j == remove_end) {
								// delegate to routine that never removes
								return with_without<I,J,DO_INSERT, false>(dest, source_index, dest_index, i, insert_end, j, remove_end);
							}
						}
					}
						
					if(DO_INSERT) {
						if(!DO_REMOVE || i->first < *j) {
							// next change is insertion
							auto& ni(*i++);
							auto insert_index = ni.first;
							auto count_before = popcnt(bitmap & ((1u << insert_index) - 1));

							assert(count_before <= count());

							// copy construct until change index
							while(source_index < count_before) {
								new ((void*)&dest.physical(dest_index++)) ELEMENT(physical(source_index++));
							}

							// insert-construct
							new ((void*)&dest.physical(dest_index++)) ELEMENT((ELEMENT)ni.second);

							// if bit was set, it is an overwrite - skip source element
							// must look a dest.bitmap because an element might have
							// been removed just before this.
							source_index += (dest.bitmap >> insert_index) & 1;

							dest.bitmap |= 1u << insert_index;


							if(i == insert_end) {
								// delegate to routine that never inserts
								return with_without<I,J,false, DO_REMOVE>(dest, source_index, dest_index, i, insert_end, j, remove_end);
							}
						}
					}
				}

				// copy construct the rest
				auto count_total = popcnt(bitmap);
				assert(count_total <= count());
				while(source_index < count_total) {
					new ((void*)&dest.data[dest_index++]) ELEMENT(physical(source_index++));
				}

				return dest_index;
			}

			template <typename I, typename J, bool DO_INSERT = true, bool DO_REMOVE = true>
			index_t with_without(bitmap_array& dest, I insert_begin, I insert_end, J remove_begin, J remove_end) const noexcept {
				index_t source_index = 0;
				index_t dest_index = 0;
				dest.bitmap = bitmap;
				return with_without(dest, source_index, dest_index, insert_begin, insert_end, remove_begin, remove_end);
			}


			// insertion constructor
			template <typename I>
			index_t with(bitmap_array& dest, I begin, I end) const noexcept {
				index_t dummy(0);
				return with_without<I,index_t*, true, false>(dest, begin, end, &dummy, &dummy);
			}

			template <typename I>
			index_t without(bitmap_array& dest, I begin, I end) const noexcept {
				pair<index_t, ELEMENT> dummy(0, ELEMENT());
				return with_without<decltype(&dummy),I, false, true>(dest, &dummy, &dummy, begin, end);
			}
		};

		template <typename KEY, typename VALUE, typename HASHFN, typename EQ_COMP = std::equal_to<KEY>, thread_policy THREAD_POLICY = multi_threaded>
		class hamt_node : public reference_counted<THREAD_POLICY> {
		public:
			using my_t = hamt_node<KEY, VALUE, HASHFN, EQ_COMP, THREAD_POLICY>;
			using key_t = KEY;
			using value_t = VALUE;
			using keyval_t = pair<key_t, value_t>;
			using hasher_t = HASHFN;
			using hash_t = decltype(hasher_t()(std::declval<key_t>()));
			using comparer_t = EQ_COMP;

			using bitmap_t = std::uint32_t;
			static constexpr int bits_per_node = 5;
			static constexpr int node_size = sizeof(bitmap_t) * 8;


			using ref_t = const ref<const hamt_node, single_threaded>;
			using leaf_t = keyval_t;
			using subtree_t = ref_t;

			void dispose() {
				// use dtor and free because this class is constructed with
				// placement new into a malloc'd buffer
				this->~hamt_node();
				free(this);
			}

			void dispose() const {
				return const_cast<hamt_node*>(this)->dispose();
			}

		private:
			using keyval_array_t = bitmap_array<bitmap_t, leaf_t>;
			using subtree_array_t = bitmap_array<bitmap_t, subtree_t>;
			typedef char header_size_t[sizeof(reference_counted<THREAD_POLICY>) + (alignof(keyval_array_t)-1) & ~(alignof(keyval_array_t)-1)];

			using index_t = typename keyval_array_t::index_t;

			static size_t align_to(size_t value, size_t alignment) {
				auto mask = alignment - 1;
				return (value + mask) & ~mask;
			}

			static size_t keyval_offset() {
				return sizeof(header_size_t);
			}

			static size_t subtree_offset(int num_kv) {
				return align_to(keyval_offset() + keyval_array_t::minimal_size(num_kv), alignof(subtree_array_t));
			}

			static size_t total_size(size_t st_offset, index_t num_sts) {
				return st_offset + subtree_array_t::minimal_size(num_sts);
			}

			keyval_array_t* get_keyvalue_array() const {
				return (keyval_array_t*)((char *)this + keyval_offset());
			}

			subtree_array_t* get_subtree_array() const {
				auto kva = get_keyvalue_array();
				return (subtree_array_t*)((char *)this + subtree_offset(kva->count()));
			}

			hamt_node() { }

			hamt_node(const hamt_node& from) {
				(get_keyvalue_array())->construct(*from->get_keyvalue_array());
				(get_subtree_array())->construct(*from->get_subtree_array());
			}

			hamt_node(hamt_node&& from) {
				(get_keyvalue_array())->construct(std::move(*from->get_keyvalue_array()));
				(get_subtree_array())->construct(std::move(*from->get_subtree_array()));
			}

			static index_t extract_hash_piece(hash_t hash, int shift) {
				auto hash_bits = sizeof(hash_t) * 8;
				return (hash >> (hash_bits - bits_per_node - shift)) & (node_size - 1);
			}

			static bool hash_significant(int shift) {
				auto hash_bits = sizeof(hash_t) * 8;
				return shift < hash_bits;
			}

		public:
			~hamt_node() {
				(get_keyvalue_array())->destroy();
				(get_subtree_array())->destroy();
			}

			size_t measure_memory_use() const {
				auto kva = get_keyvalue_array();
				auto sta = get_subtree_array();
				auto stoff = subtree_offset(kva->count());
				auto num_subtrees = sta->count();
				auto sz = total_size(stoff, num_subtrees);
				for(index_t i = 0; i < num_subtrees; ++i) {
					sz += sta->physical(i)->measure_memory_use();
				}
				return sz;
			}

			template <typename I>
			ref_t assoc(I add_begin, I add_end, int shift = 0) const {
				if(add_begin == add_end) return this;
				return assoc(get_subtree_array(), get_keyvalue_array(), add_begin, add_end, shift);
			}

			ref_t dissoc(const key_t& key, hash_t hash, int shift = 0) const {
				index_t nst(0), nkv(0);
				return dissoc(key, hash, shift, false, nst, nkv);
			}

			ref_t dissoc(const key_t& key) const {
				return dissoc(key, hasher_t()(key));
			}

			VALUE* get(const KEY& key, hash_t hash, int shift = 0) const {
				auto keyvals = get_keyvalue_array();

				// todo: predict not taken
				if(!hash_significant(shift)) {
					// hash is exhausted, so search linearly
					for(int i = 0, e = keyvals->count(); i < e; ++i) {
						if(keyvals->physical(i).first == key) {
							return &keyvals->physical(i).second;
						}
					}
					auto subtrees = get_subtree_array();
					// proceed down the first subtree (like linked list)
					if(subtrees->bitmap) {

						return subtrees->physical(0)->get(key, hash, shift);
					} else return nullptr;
				}

				index_t hash_piece = extract_hash_piece(hash, shift);

				if(keyvals->has_element(hash_piece)) {
					auto &kv(keyvals->logical(hash_piece));
					if(kv.first == key) return &kv.second;
					else return nullptr;
				}

				auto subtrees = get_subtree_array();
				if(subtrees->has_element(hash_piece)) {
					return subtrees->logical(hash_piece)->get(key, hash, shift + bits_per_node);
				}
				return nullptr;
			}

			template <typename I>
			static ref_t construct(I begin, I end, int shift = 0) {
				// must not call ctor/dtor
				bitmap_t empty = 0;
				if(begin == end) {
					return construct(0, (subtree_array_t*)&empty, 0, (keyval_array_t*)&empty);
				} else {
					return assoc((subtree_array_t*)&empty, (keyval_array_t*)&empty, begin, end, shift);
				}
			}

			struct iterator_t {
				hamt_node& in;
				hash_t hcount;
				size_t subcount;

			};

		private:
			static ref_t construct(index_t num_subtrees, subtree_array_t* consume_subtrees,
				index_t num_keyvals, keyval_array_t* consume_keyvals) {
				auto kva_offset = keyval_offset();
				auto sta_offset = subtree_offset(num_keyvals);
				auto minimal_size = total_size(sta_offset, num_subtrees);
				assert(minimal_size >= sizeof(hamt_node));
				// todo: align
				auto new_node = new(malloc(minimal_size)) hamt_node;
				// todo: handle OOM
				auto new_kva = (keyval_array_t*)((char *)new_node + kva_offset);
				auto new_sta = (subtree_array_t*)((char *)new_node + sta_offset);


				new_kva->construct(std::move(*consume_keyvals));
				new_sta->construct(std::move(*consume_subtrees));

				assert(new_kva == new_node->get_keyvalue_array());
				assert(new_sta == new_node->get_subtree_array());

				consume_keyvals->destroy();
				consume_subtrees->destroy();

#ifndef NDEBUG
				{
					auto hdr_end = (char*)new_node + sizeof(hamt_node);
					auto kva = new_node->get_keyvalue_array();
					auto kva_end = (char*)kva + kva->minimal_size();
					auto sta = new_node->get_subtree_array();
					auto sta_end = (char*)sta + sta->minimal_size();
					auto mem_end = (char*)new_node + minimal_size;
					assert(kva->count() == num_keyvals && sta->count() == num_subtrees);
					assert((char*)kva >= hdr_end);
					assert((char*)sta >= kva_end);
					assert(mem_end >= sta_end);
				}
#endif
				return new_node;
			}

			// assoc range must be sorted by hash
			template <typename I>
			static ref_t assoc(const subtree_array_t* subtrees, const keyval_array_t* keyvals, I add_begin, I add_end, int shift) {
#ifndef NDEBUG
				if (add_begin != add_end)
				{
					auto testmask = (1u << shift) - 1;
					auto i = add_begin;
					auto check = i->first & testmask;
					++i;
					for(; i != add_end; ++i) {
						assert((i->first & testmask) == check && "added values should have ended up in different subtrees");
					}
				}
#endif
				// must not call ctor/dtor
				auto new_subtrees = (subtree_array_t*)alloca(sizeof(subtree_array_t));
				auto new_keyvals = (keyval_array_t*)alloca(sizeof(keyval_array_t));

				using st_insert_t = pair<index_t, subtree_t>;
				using kv_insert_t = pair<index_t, leaf_t>;
				constrained_array<st_insert_t, node_size> subtree_inserts;
				constrained_array<kv_insert_t, node_size> keyval_inserts;
				constrained_array<index_t, node_size> keyval_removes;

				auto insert = [&](index_t hash_piece, auto begin, auto end) {
					if(begin == end) return;
					if(subtrees->has_element(hash_piece)) {
						// we have a subtree, push the element range into that node
						subtree_inserts.emplace_back(hash_piece, subtrees->logical(hash_piece)->assoc(begin, end, shift + bits_per_node));
					} else if(keyvals->has_element(hash_piece)) {
						// key-val bucket at this junction
						auto &oldkv = keyvals->logical(hash_piece);
						keyval_removes.emplace_back(hash_piece);

						EQ_COMP key_equal;
						if(std::distance(begin, end) == 1 && key_equal(oldkv.first, begin->second.first)) {
							// replace key-value
							keyval_inserts.emplace_back(hash_piece, begin->second);
						} else {
							// replace with subtree
							HASHFN rehash;
							pair<hash_t, leaf_t> down_a_level(rehash(oldkv.first), oldkv);

							typename std::remove_const<ref_t>::type new_subtree;
							if(subtrees->has_element(hash_piece)) {
								new_subtree = subtrees->logical(hash_piece)->assoc(begin, end, shift + bits_per_node);
							} else {
								new_subtree = construct(begin, end, shift + bits_per_node);
							}

							// this mallocs twice which could be coalesced
							subtree_inserts.emplace_back(hash_piece,
							    new_subtree->assoc(&down_a_level, &down_a_level + 1, shift + bits_per_node));
						}
					} else {
						// junction empty, make kvpair or subtree based on range size
						switch(std::distance(begin, end)) {
						case 1:
							keyval_inserts.emplace_back(hash_piece, begin->second);
							break;
						default:
							subtree_inserts.emplace_back(hash_piece, construct(begin, end, shift));
							break;
						}
					}
				};

				if(!hash_significant(shift)) {
					// hash is no longer useful, so just search keyvalues linearly
					// append into the first subtree if more space is needed
					EQ_COMP key_equal;
					auto num_keyvals = keyvals->count();
					for(auto j = add_begin; j != add_end; ++j) {
						bool key_found = false;
						for(index_t i = 0; i < num_keyvals; ++i) {
							auto& kv = keyvals->physical(i);
							if(key_equal(kv.first, j->second.first)) {
								// key found, overwrite value
								// using index as hash works when the vector is only appended in-order
								keyval_inserts.emplace_back(i, j->second);
								key_found = true;
								break;
							}
						}
						if(!key_found) {
							if(num_keyvals < node_size) {
								// push onto the back of keyval array
								keyval_inserts.emplace_back(num_keyvals, j->second);
							} else {
								// ad-hoc linked list using he first subtree as a 'next'
								// push the rest of the nodes onto that subtree
								if(subtrees->bitmap) {
									subtree_inserts.emplace_back(0, subtrees->physical(0)->assoc(j, add_end, shift));
								} else {
									subtree_inserts.emplace_back(0, construct(j, add_end, shift));
								}
								break;
							}
						}
					}
				} else {
					auto cur_begin = add_begin;
					index_t cur_hash_piece = extract_hash_piece(add_begin->first, shift);
					auto cur_i = cur_begin;
					++cur_i;

					while(cur_i != add_end) {
						index_t hash_piece = extract_hash_piece(cur_i->first, shift);
						if(hash_piece != cur_hash_piece) {
							// this hash subrange ends, make modifications
							insert(cur_hash_piece, cur_begin, cur_i);
							// start next subrange
							cur_begin = cur_i;
							cur_hash_piece = hash_piece;
						}
						++cur_i;
					}

					if(cur_i != cur_begin) {
						// last subrange
						insert(cur_hash_piece, cur_begin, cur_i);
					}
				}

				auto num_subtrees = subtrees->with(*new_subtrees, subtree_inserts.begin(), subtree_inserts.end());
				auto num_keyvals  =  keyvals->with_without(*new_keyvals, keyval_inserts.begin(), keyval_inserts.end(), 
																	  keyval_removes.begin(), keyval_removes.end());

				return construct(num_subtrees, new_subtrees, num_keyvals, new_keyvals);
			}

			subtree_t dissoc(const key_t& key, hash_t hash, int shift, bool allow_collapse, index_t &num_subtrees, index_t &num_keyvals) const {
				using st_insert_t = pair<index_t, subtree_t>;
				using kv_insert_t = pair<index_t, leaf_t>;

				// must not call ctor/dtor
				auto new_subtrees = (subtree_array_t*)alloca(sizeof(subtree_array_t));
				auto new_keyvals = (keyval_array_t*)alloca(sizeof(keyval_array_t));
				new_subtrees->bitmap = new_keyvals->bitmap = 0;

				if(!hash_significant(shift)) {
					// hash is exhausted, so search linearly
					auto keyvals = get_keyvalue_array();
					for(int i = 0, e = keyvals->count(); i < e; ++i) {
						if(keyvals->physical(i).first == key) {
							index_t dummy(0);
							index_t rvi(i);
							num_subtrees = get_subtree_array()->without(*new_subtrees, &dummy, &dummy);
							num_keyvals = keyvals->without(*new_keyvals, &rvi, &rvi + 1);
							goto construct_tree;
						}
					}

					auto subtrees = get_subtree_array();
					// proceed down the first subtree (like linked list)
					if(subtrees->bitmap) {
						index_t nst(0), nkv(0);
						subtree_t remain = subtrees->physical(0)->dissoc(key, hash, shift, false, nst, nkv);
						st_insert_t sti(0, remain);
						index_t rvi(0);
						if(remain.get() == this) {
							return this;
						}

						num_keyvals = keyvals->without(*new_keyvals, &rvi, &rvi);
						num_subtrees = get_subtree_array()->with_without(*new_subtrees, &sti, &sti + 1, &rvi, &rvi + 1);
						goto construct_tree;
					}
				} else {
					auto kva = get_keyvalue_array();
					auto sta = get_subtree_array();
					auto hash_piece = extract_hash_piece(hash, shift);

					if(kva->has_element(hash_piece)) {
						if(kva->logical(hash_piece).first == key) {
							// remove this
							num_subtrees = sta->without(*new_subtrees, &hash_piece, &hash_piece);
							num_keyvals = kva->without(*new_keyvals, &hash_piece, &hash_piece + 1);
						} else {
							return this;
						}
					} else if(sta->has_element(hash_piece)) {
						index_t nkv(0), nst(0);

						auto old = sta->logical(hash_piece);
						auto remain = old->dissoc(key, hash, shift + bits_per_node, true, nst, nkv);						

						if(remain == old) {
							// no change
							return this;
						}

						if(allow_collapse && nst == 0 && nkv == 1) {
							// remainder is key/value
							num_subtrees = sta->without(*new_subtrees, &hash_piece, &hash_piece + 1);
							kv_insert_t kvi(hash_piece,remain->get_keyvalue_array()->physical(0));
							num_keyvals = kva->with(*new_keyvals, &kvi, &kvi + 1);
						} else {
							// remainder is a tree
							index_t dummy(0);
							num_keyvals = kva->without(*new_keyvals, &dummy, &dummy);
							if(remain.get()) {
								// replace
								st_insert_t sti(hash_piece, remain);
								num_subtrees = sta->with_without(
									*new_subtrees, &sti, &sti + 1,
									&hash_piece, &hash_piece + 1);
							} else {
								// remove
								num_subtrees = sta->without(*new_subtrees, &hash_piece, &hash_piece + 1);
							}
						}
					}
					else {
						return this;
					}
				}
			construct_tree:
				if(allow_collapse && num_subtrees == 0 && num_keyvals == 0) {
					return ref_t();
				}
				return construct(num_subtrees, new_subtrees, num_keyvals, new_keyvals);
			}
		public:
			template <typename FN>
			void for_each(FN&& iterator) const {
				auto kva = get_keyvalue_array();
				for(index_t i = 0, e = kva->count(); i < e; ++i) {
					auto& kv(kva->physical(i));
					iterator(kv.first, kv.second);
				}
				auto sta = get_subtree_array();
				for(index_t i = 0, e = sta->count(); i < e; ++i) {
					sta->physical(i).get()->for_each(iterator);
				}
			}

			bool keys_intersect(const hamt_node& other) const {
				auto kva = get_keyvalue_array(), kvb = other.get_keyvalue_array();
				auto ibmp = (kva->bitmap & kvb->bitmap);
				for(;;) {
					auto next = ctz(ibmp);
					if(next >= node_size) break;
					if(kva->logical(next).first == kvb->logical(next).first) return true;
					ibmp &= ~((1u << (next + 1)) - 1);
				}

				auto sta = get_subtree_array(), stb = other.get_subtree_array();
				ibmp = (sta->bitmap & stb->bitmap);

				for(;;) {
					auto next = ctz(ibmp);
					if(next >= node_size) return false;
					if(sta->logical(next)->keys_intersect(*stb->logical(next).get())) return true;
					ibmp &= ~((1u << (next + 1)) - 1);
				}
			}

			bool empty() {
				return get_keyvalue_array()->empty() && get_subtree_array()->empty();
			}
		};
	}

	template <typename KEY, typename VALUE, typename HASHFN = std::hash<KEY>, typename COMPEQ = std::equal_to<KEY>, detail::thread_policy THREAD_POLICY = detail::multi_threaded>
	class hamt {
	public:
		using hash_t = decltype(HASHFN()(std::declval<KEY>()));
		using key_t = KEY;
		using value_t = VALUE;
		using keyvalue_t = detail::pair<key_t, value_t>;
	private:
		using node = detail::hamt_node<KEY, VALUE, HASHFN, COMPEQ, THREAD_POLICY>;
		using node_ref = cref<node, detail::lockfree>;
		node_ref root;
		hamt(node_ref r) :root(r) { }
		
		static node_ref assoc(const node_ref& root, hash_t hash, const key_t& key, const value_t& value)  {
			auto insert = std::make_pair(hash, std::make_pair(key, value));
			return root->assoc(&insert, &insert + 1, 0);
		}

		static node_ref assoc(const node_ref& root, hash_t hash, const key_t& key, const optional<value_t>& value)  {
			if(!value.has_value) return root;
			auto insert = std::make_pair(hash, std::make_pair(key, *value));
			return root->assoc(&insert, &insert + 1, 0);
		}
	public:
		hamt(const std::initializer_list<std::pair<KEY, VALUE>>& init) {
			using namespace detail;
			using insert_t = pair<hash_t, typename node::leaf_t>;
			std::vector<insert_t> insertions(init.size());
			auto j = init.begin();
			for(int i = 0; i < init.size(); ++i) {
				auto& in(*j++);
				insertions[i] =
					std::make_pair(HASHFN()(in.first), std::make_pair(in.first, in.second));
			}
			root = node::construct(insertions.data(), insertions.data() + init.size());
		}

		hamt() {
			using namespace detail;
			using insert_t = pair<hash_t, typename node::leaf_t>;
			const insert_t* dummy = nullptr;
			root = node::construct(dummy, dummy);
		}

		size_t measure_memory_use() const {
			auto tmp = root.borrow();
			return tmp->measure_memory_use();
		}

		value_t get(const key_t& key, value_t fallback = value_t()) const {
			HASHFN hasher;
			auto tmp = root.borrow();
			auto find = tmp->get(key, hasher(key));
			return find ? *find : fallback;
		}

		optional<value_t> operator[](const key_t &key) const {
			HASHFN hasher;
			auto tmp = root.borrow();
			auto find = tmp->get(key, hasher(key));
			return find ? *find : optional<value_t>();
		}

		hamt assoc(const key_t& key, const value_t& value) const {
			if (empty()) return hamt({ {key, value} });
			auto tmp = root.borrow();
			return assoc(tmp, HASHFN()(key), key, value);
		}

		hamt dissoc(const key_t& key) const {
			if (empty()) return *this;
			auto tmp = root.borrow();
			return node_ref(tmp->dissoc(key, HASHFN()(key)).get());
		}

		template <typename UPDATE_FN>
		hamt update_in(const key_t& key, const UPDATE_FN& upd) {
			HASHFN hasher;
			auto hash = hasher(key);
			node_ref updated;
			root.swap([this, &updated, &key, &hash, &upd](const node* r) -> node_ref {
				optional<value_t> prev = r->get(key, hash);
				auto next = upd(prev);
				if (has_value(next)) {
					updated = this->assoc(r, hash, key, next);
				} else {
					updated = r->dissoc(key, hash);
				}
				return updated;
			});
			return updated;
		}

		template <typename ITERATOR_FN>
		void for_each(ITERATOR_FN&& fn) const {
			auto tmp = root.borrow();
			if (tmp.get()) tmp->for_each(fn);
		}

		const node_ref& operator->() const {
			return root;
		}

		size_t size() const {
			size_t count = 0;
			for_each([&count](const key_t&, const value_t&) mutable {
				++count;
			});
			return count;
		}

		void swap(hamt& thread_private) {
			root.exchange(thread_private.root);
		}

		bool keys_intersect(const hamt& other) const {
			auto tmp = root;
			return tmp->keys_intersect(*other.root.get());
		}

		bool empty() const {
			auto tmp = root;
			return tmp->empty();
		}

		std::vector<keyvalue_t> collect() const {                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             
			std::vector<keyvalue_t> kvs;
			for_each([&kvs](const auto& key, const auto& value) {
				kvs.emplace_back(key, value);
			});
			return kvs;
		}
	};
}
