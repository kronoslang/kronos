#pragma once

#include "common/Enumerable.h"

#include <unordered_map>
#include <unordered_set>
#include <cassert>

#define DEFAULT_HASHER std::hash
#define DEFAULT_EQUALITY std::equal_to
#define DEFAULT_ALLOCATOR std::allocator

#define LARGE_SET_TRESHOLD 32

#include <iostream>

namespace Sml{
	using namespace std;

	/* these collection classes employ linear search and swap to std::unordered implementations when they grow beyond a treshold */
	template <class KEY, class VALUE, class HASHER = DEFAULT_HASHER<KEY>, class EQUALITY = DEFAULT_EQUALITY<KEY>, class ALLOCATOR = DEFAULT_ALLOCATOR<pair<const KEY,VALUE>>>
	class Map {
		unsigned size;
		unordered_map<KEY,VALUE,HASHER,EQUALITY,ALLOCATOR> *map;
		pair<KEY,VALUE> owned[LARGE_SET_TRESHOLD];
		void growToLarge() {
			map = new unordered_map<KEY,VALUE,HASHER,EQUALITY,ALLOCATOR>();
			for(unsigned i=0;i<size;++i) map->insert(owned[i]);
		}
	public:
		typedef pair<KEY,VALUE> entry_t;
		Map& operator=(const Map& source) {
			if (map) delete map;
			size=source.size;
			map = source.map?new unordered_map<KEY,VALUE,HASHER,EQUALITY,ALLOCATOR>(*source.map):0;
			for(unsigned i(0);i<size;++i) owned[i]=source.owned[i];
			return *this;
		}

		Map& operator=(Map&& source) {
			if (map) delete map;
			size=source.size;
			map = source.map;
			source.map = 0;
			for(unsigned i(0);i<size;++i) owned[i]=source.owned[i];
			return *this;
		}

		Map(const Map& source) {
			size=source.size;
			map = source.map?new unordered_map<KEY,VALUE,HASHER,EQUALITY,ALLOCATOR>(*source.map):0;
			for(unsigned i(0);i<size;++i) owned[i]=source.owned[i];
		}; 

		Map():size(0),map(0) { }
		~Map() { if (map) delete map; }

		VALUE& operator[](const KEY& k) {
			if (size>=LARGE_SET_TRESHOLD) return (*map)[k];
			for(unsigned i(0);i<size;++i) if (owned[i].first == k) return owned[i].second;
			return insert(make_pair(k,VALUE())).second;
		}

		pair<KEY,VALUE>* find(const KEY& k) {
			if(size>=LARGE_SET_TRESHOLD) {
				auto f(map->find(k));
				if (f == map->end()) return 0;
				else return (pair<KEY,VALUE>*)(&(*f));
			}
			for(unsigned i(0);i<size;++i) if (owned[i].first == k) return &owned[i];
			return (pair<KEY,VALUE>*)0;
		}

		const pair<KEY,VALUE>* find(const KEY& k) const {
			if(size>=LARGE_SET_TRESHOLD) {
				auto f(map->find(k));
				if (f == map->end()) return 0;
				else return (pair<KEY,VALUE>*)(&(*f));
			}
			for(unsigned i(0);i<size;++i) if (owned[i].first == k) return &owned[i];
			return (pair<KEY,VALUE>*)0;
		}

		pair<KEY,VALUE>& insert(const pair<KEY,VALUE> &item) {
			assert(find(item.first) == nullptr && "Map already contains this key");
			if (size>=LARGE_SET_TRESHOLD) {
				return (pair<KEY,VALUE>&)(*map->insert(item).first);
			} else {
				owned[size++]=item;
				if (size>=LARGE_SET_TRESHOLD) growToLarge();
				return owned[size-1];
			}
		}

		pair<KEY,VALUE>& insert(const KEY& k, const VALUE& v) {return insert(make_pair(k,v));}

		template <typename FUNCTOR> void for_each(FUNCTOR f) const {
			if (size>=LARGE_SET_TRESHOLD) {
				for(auto x : *map) f(x.first,x.second);
			}
			else for(unsigned i(0);i<size;++i) f(owned[i].first,owned[i].second);
		}
	};


	template <class VALUE, class HASHER = DEFAULT_HASHER<VALUE>, class EQUALITY = DEFAULT_EQUALITY<VALUE>, class ALLOCATOR = DEFAULT_ALLOCATOR<VALUE>>
	class Set {
		unsigned _size;
		typedef unordered_set<VALUE,HASHER,EQUALITY,ALLOCATOR> fallback_t; 
		fallback_t *set;
		VALUE owned[LARGE_SET_TRESHOLD];
		void growToLarge() {
			set = new unordered_set<VALUE,HASHER,EQUALITY,ALLOCATOR>();
			for(unsigned i=0;i<_size;++i) set->insert(owned[i]);
		}

		bool fallback_compare(const Set& rhs) const {
			assert(_size == rhs._size && _size < LARGE_SET_TRESHOLD);
			fallback_t ls, rs;
			for (size_t i(0);i < _size;++i) {
				ls.insert(owned[i]);
				rs.insert(rhs.owned[i]);
			}
			return ls == rs;
		}

	public:
		LAZY_ENUMERATOR(const VALUE&,enumerator_t) {
			typename fallback_t::const_iterator cur_it;
			const VALUE* cur_pt;
			unsigned size;
			const Set& from;
			enumerator_t(const Set& from):from(from) { }
			LAZY_BEGIN
				if (from._size >= LARGE_SET_TRESHOLD) {
					for(cur_it = from.set->begin();cur_it!=from.set->end();++cur_it) LAZY_YIELD(*cur_it);
				} else {
					for(cur_pt = from.owned; cur_pt < from.owned + from._size; ++cur_pt) LAZY_YIELD(*cur_pt);
				}
			LAZY_END
		};

		typedef VALUE value_type;
		enumerator_t GetEnumerator() const { return enumerator_t(*this); }

		Set(const Set& source) {
			_size = source._size;
			for(unsigned int i(0);i<_size;++i) owned[i]=source.owned[i];
			if (source.set) set = new unordered_set<VALUE,HASHER,EQUALITY,ALLOCATOR>(*source.set);
			else set = 0;
		}

		Set(Set&& source):set(0) {
			set = 0;
			*this = std::move(source);
		}

		Set& operator=(Set&& source) noexcept {
			if (set) delete set;
			for(unsigned k(0);k<source._size;++k) owned[k]=source.owned[k];
			_size = source._size;
			set = source.set;
			source._size = 0;
			source.set = 0;
			return *this;
		}

		Set& operator=(const Set& source) {
			return *this = Set(source);
		}

		Set():_size(0),set(0) { }
		~Set() {
			if (set) delete set;
		}

		const VALUE* find(const VALUE& k) const {
			if(_size>=LARGE_SET_TRESHOLD) {
				auto f(set->find(k));
				if (f == set->end()) return 0;
				else return &(*f);
			}
			for(unsigned i(0);i<_size;++i) if (owned[i] == k) return &owned[i];
			return (VALUE*)0;
		}

		const VALUE& insert(const VALUE &item) {
			assert(find(item) == 0 && "Set already contains this key");
			if (_size>=LARGE_SET_TRESHOLD) return *set->insert(item).first;
			else {
				owned[_size++]=item;
				if (_size>=LARGE_SET_TRESHOLD) growToLarge();
				return owned[_size-1];
			}
		}	

		size_t size() const {if (_size>=LARGE_SET_TRESHOLD) return set->size(); else return _size;}

		template <typename FUNCTOR> void for_each(FUNCTOR f) const {
			if (_size>=LARGE_SET_TRESHOLD) {
				for(auto x : *set) f(x);
			}
			else for(unsigned i(0);i<_size;++i) f(owned[i]);
		}

		template <typename FUNCTOR> void transform(FUNCTOR f) {
			if (_size>=LARGE_SET_TRESHOLD) {
				for(auto x : *set) x = f(x);
			}
			else for(unsigned i(0);i<_size;++i) owned[i] = f(owned[i]);
		}

		bool operator==(const Set& rhs) const {
			if (_size >= LARGE_SET_TRESHOLD) {
				return *set == *rhs.set;
			} 

			if (_size != rhs._size) return false;

			if (_size < 5) {
				for (size_t i = 0;i < _size;++i) {
					for (size_t j = 0; j < _size;++j) {
						if (owned[i] == rhs.owned[j]) goto found;
					}
					return false;
				found:
					continue;
				}
				assert(fallback_compare(rhs));
				return true;
			}
			return fallback_compare(rhs);
		}

		bool operator!=(const Set& rhs) const {
			return !operator==(rhs);
		}
	};
}

