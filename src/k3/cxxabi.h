#pragma once

#include <string>
#include <vector>
#include <cassert>

namespace abi {

	class raw_buffer {
	public:
		virtual ~raw_buffer() { }
		virtual const void* data() const = 0;
		virtual size_t element_size() const = 0;
		virtual size_t element_count() const = 0;
	};

	template <typename T> class vector : public raw_buffer {
		T* _vec;
		size_t sz;
	public:
		vector(const T* begin, const T* end):sz(end - begin) { _vec = new T[end - begin]; for (int i(0); begin + i < end; ++i) _vec[i] = begin[i]; }
		~vector() { delete[] _vec; }

		const void *data() const { return _vec; }
		size_t element_size() const { return sizeof(T); }
		size_t element_count() const { return sz; }
	};

	template <typename VAL> class pimpl {
		VAL* v;
	public:
		pimpl(VAL* v):v(v) { }
		~pimpl() { delete v; }
		VAL& operator*() { return *v; }
		VAL& operator->() { return *v; }
		VAL* release() { VAL* r(v); v = nullptr; return r; }

		template <typename T> operator std::vector<T>() const {
			assert(sizeof(T) == v->element_size());
			return std::vector<T>((const T*)v->data(), (const T*)v->data() + v->element_count());
		}

		operator std::string() const {
			assert(sizeof(std::string::value_type) == v->element_size());
			return std::string((char*)v->data(), (char*)v->data() + v->element_count());
		}
	};

	typedef pimpl<raw_buffer> praw;
	typedef raw_buffer* pptr;

	static praw str(const std::string& from) {
		return new vector<char>(from.data(), from.data() + from.size());
	}

	template <typename T> static praw vec(const std::vector<T>& v) {
		return new vector<char>(v.data(), v.data() + v.size());
	}
}