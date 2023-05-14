#pragma once

#include "Ref.h"
#include <ostream>
#include <cstdint>

class simple_string;
class abstract_string;
class string_iterator
{
	friend class abstract_string;
protected:
	const abstract_string *host;
	const simple_string *current;
	size_t host_pos;
	size_t current_pos;
	void move_iter(int32_t offset) const;
public:
	string_iterator operator++() const {move_iter(1);return *this;}
	string_iterator operator--() const {move_iter(-1);return *this;}
	string_iterator operator++(int) {auto tmp(*this);operator++();return tmp;}
	string_iterator operator--(int) {auto tmp(*this);operator--();return tmp;}
	string_iterator operator+(int32_t offset) const {auto tmp(*this);tmp.move_iter(offset);return tmp;}
	string_iterator operator-(int32_t offset) const {return operator+(-offset);}
	string_iterator operator+=(int32_t offset) {auto tmp(*this);move_iter(offset);return tmp;}
	string_iterator operator-=(int32_t offset) {return operator+=(-offset);}
	int32_t operator*() const;
	bool operator==(const string_iterator& rhs) const {return host == rhs.host && host_pos == rhs.host_pos;}
	bool operator!=(const string_iterator& rhs) const {return !operator==(rhs);}
	int32_t operator-(const string_iterator& rhs) const {assert(host==rhs.host);return (int32_t)host_pos - (int32_t)rhs.host_pos;}
};

class abstract_string : public RefCounting{
protected:
	virtual int32_t _get_byte(size_t& index) const = 0; 
public:
	virtual const simple_string* segment_at(size_t &offset) const = 0;
	static CRef<abstract_string> cons(const char *str);
	static CRef<abstract_string> unique_instance(const abstract_string *str);
	static CRef<abstract_string> append(const abstract_string *lhs, const abstract_string* rhs);
	virtual CRef<abstract_string> skip(size_t num_chars) const = 0;
	virtual CRef<abstract_string> take(size_t num_chars) const = 0;
	int32_t get_byte(size_t index) const {return _get_byte(index);}
	virtual size_t utf8len() const = 0;
	virtual size_t asciilen() const = 0;
	virtual size_t find(const abstract_string *pattern) const;
	virtual int compare(const abstract_string& rhs) const = 0;
	virtual int compare(const simple_string& rhs) const = 0;
	virtual void stream(std::ostream& out) const = 0;
	virtual size_t hash(size_t seed = 1) const = 0;
	string_iterator seek(size_t offset);
	string_iterator begin() {return seek(0);}
	string_iterator end() {return seek(asciilen());}
	const string_iterator seek(size_t offset) const {return ((abstract_string*)this)->seek(offset);}
	const string_iterator begin() const {return seek(0);} 
	const string_iterator end() const {return seek(asciilen());} 
	operator std::string() const;
};

typedef abstract_string SString;

static std::ostream& operator<<(std::ostream& out, const abstract_string& str) {str.stream(out);return out;}
bool operator<(const abstract_string& a, const abstract_string& b);
bool operator>(const abstract_string& a, const abstract_string& b);
bool operator==(const abstract_string& a, const abstract_string& b);
static bool operator!=(const abstract_string& a, const abstract_string& b) {return !operator==(a,b);}
static bool operator==(CRef<SString>& a, CRef<SString>& b) {return a.Reference() == b.Reference();}
static bool operator!=(CRef<SString>& a, CRef<SString>& b) {return !operator==(a,b);}

namespace std
{
	template <> struct hash<Ref<SString>> { size_t operator()(Ref<SString> s) const {return s->hash(1);} };
};
