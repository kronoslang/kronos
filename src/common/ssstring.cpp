/* shared structure string */

#include "ssstring.h"
#include <vector>
#include <unordered_set>
#include <iterator>
#include <climits>
#include <algorithm>
#include "Ref.h"

// remove
#include <iostream>
#include <sstream>

#define MIN(a,b) (a<b?a:b)
#define MAX(a,b) (a<b?b:a)

// comment out if you want standard malloc
//#include "TLPoolAllocator.h"

class simple_string;

using namespace std;


/* based on wikipedia boyer moore */
static void make_delta1(int *delta1, const abstract_string *pattern)
{
	auto pat_beg(pattern->begin()),pat_end(pattern->end());
	size_t patlen(pattern->asciilen());
	for(size_t i(0);i<256;i++) delta1[i]=(int)patlen;
	for(auto i(pat_beg);i!=pat_end-1;++i) delta1[*i]=pat_end-1-i;
}

std::pair<string_iterator,string_iterator> suffix(string_iterator word, string_iterator word_end, string_iterator pos)
{
	while(*pos == *word_end)
	{
		--pos;
		--word_end;
		if (pos == word) break;
	}
	return std::make_pair(pos,word_end);
}

int suffix_length(string_iterator word, int wordlen, int pos)
{
	string_iterator wp(word + pos);
	string_iterator we(word + wordlen - 1);
	int i(0);
	for(;(*wp==*we) && (i<pos);--wp,--we,++i);
	return i;
}

static void make_delta2(int *delta2, const abstract_string *pattern)
{
	auto pat_beg(pattern->begin()),pat_end(pattern->end());
	int patlen = pat_end - pat_beg;
	int last_prefix_index = patlen - 1;
	for(int p(patlen-1);p>=0;p--)
	{
		if (*pattern->take(patlen-p) == *pattern->skip(p+1)->take(patlen-p)) 
			last_prefix_index = p+1;

		delta2[p] = (int)last_prefix_index + (patlen-1-p);
	}

	for(int p(0);p<patlen-1;p++)
	{
		int slen = suffix_length(pat_beg,patlen,p);
		if (pattern->get_byte(p-slen) != pattern->get_byte(patlen-1-slen))
		{
			delta2[patlen-1-slen] = (int)patlen-1-p+slen;
		}
	}

}

static string_iterator boyer_moore(const abstract_string *text, const abstract_string *pattern)
{
	size_t patlen = pattern->asciilen();
	assert(patlen <= 0x7fffffff);
	int delta1[256];
	std::vector<int> delta2(patlen);
	make_delta1(delta1,pattern);
	make_delta2(delta2.data(),pattern);

	auto i(text->seek(patlen-1));
	auto text_end(text->end()),pat_beg(pattern->begin());
	while(i!=text_end)
	{
		auto j(pattern->seek(patlen-1));
		while(*i == *j)
		{
			if (j == pat_beg)
			{
				return i;
			}
			--i;
			--j;
		}
		int skip(std::max(delta1[*i],delta2[j-pat_beg]));
		i += skip;
	}
	return text_end;
}

static size_t linear_search(const abstract_string *text, const abstract_string *pattern)
{
	size_t pos(0);
	Ref<const abstract_string> to_search = text;
	while(to_search->utf8len() >= pattern->utf8len())
	{
		if (*to_search->take(pattern->utf8len()) == *pattern) return pos;
		else pos++;
		to_search = to_search->skip(1);
	}
	return -1;
}

size_t abstract_string::find(const abstract_string *pattern) const
{
	CRef<abstract_string> h0(this),h1(pattern);
	auto text_end(end());
	auto match(boyer_moore(this,pattern));
	if (match == text_end) {
		return -1;
	} else {
		size_t u8(0);
		auto i(begin());
		while(i!=match)
		{
			if ((*i++ & 0x80) == 0) u8++;
		}
		assert(u8 == linear_search(this,pattern) && " substring search bug!");
		return u8;
	}
}

abstract_string::operator std::string() const {
	std::string tmp;
	tmp.reserve(asciilen());
	for (auto c : *this) {
		tmp.push_back(c);
	}
	return tmp;
}



class char_buffer : public vector<char>, public RefCounting
{
};

class simple_string : public abstract_string
{
	//	TLP_ALLOCATOR(simple_string);
	friend class string_iterator;

	CRef<char_buffer> content;
	size_t content_start;
	size_t content_end;

#define NATVIS_CTOR
#ifdef _MSC_VER
#ifndef NDEBUG
#undef NATVIS_CTOR
	// support for the limited vs2012 natvis
	const char *natvisStr;
	string natvisHolder;
#define NATVIS_CTOR natvisHolder = string(content->data() + content_start, content->data() + content_end); natvisStr = natvisHolder.c_str();
#endif
#endif

	size_t u8len;
	simple_string(const char *data, size_t max_len = -1):content_start(0)
	{
		Ref<char_buffer> tmp(new char_buffer);
		u8len=0;
		while(*data && max_len--) 
		{
			if ((*data & 0xC0) != 0x80) u8len++;
			tmp->push_back(*data++);
		}
		content_end = tmp->size();
		content = std::move(tmp);

		NATVIS_CTOR
	}

	simple_string(const char_buffer *content, size_t start = 0, size_t end =-1ll):content(content),content_start(start),content_end(MIN(end,content->size()))
	{
		u8len=0;
		for(size_t i(content_start);i!=content_end;++i) if ((content->at(i)&0xc0)!=0x80) u8len++;
		assert(content_end<=content->size() && "simple_string out of bounds");

		NATVIS_CTOR
	}

public:

	size_t hash(size_t seed) const {
		size_t h = seed;
		for (size_t i(content_start); i != content_end; ++i) h = (1 + h) * 2654435761ul;
		return h;
	}

	static Ref<simple_string> cons(const char *data) {return new simple_string(data);}

	int32_t _get_byte(size_t& offset) const
	{
		if (offset>=content_end-content_start) {offset-=content_end-content_start;return -1;}
		else return content->at(content_start + offset);
	}

	char_buffer::const_iterator begin() const {return content->begin() + content_start;}
	char_buffer::const_iterator end() const {return content->begin() + content_end;}

	size_t utf8len() const
	{
		return u8len;
	}

	size_t asciilen() const
	{
		return content_end - content_start;
	}

	CRef<abstract_string> skip(size_t skip) const
	{
		if (skip == 0) return this;
		size_t i(content_start);
		while(skip>0)
		{
			static CRef<abstract_string> empty = abstract_string::cons("");
			if (i>=content_end) return empty;
			if ((content->at(i++) & 0xC0)!=0x80) skip--;		
		}
		while(i < content_end && (content->at(i) & 0xC0) == 0x80) i++;

		return new simple_string(content,i,content_end);
	}

	CRef<abstract_string> take(size_t take) const
	{	
		if (take>content_end - content_start) return this;
		vector<char> t;
		size_t i(content_start);
		while(take>0 && i < content_end)
		{
			if ((content->at(i++) & 0xC0)!=0x80) take--;
		}
		while(i < content_end && (content->at(i) & 0xC0) == 0x80) i++;

		return new simple_string(content,content_start,i);		
	}

	int do_compare(const simple_string& rhs) const
	{
		auto ai(begin());
		auto bi(rhs.begin());
		while(ai!=end())
		{
			if (bi==rhs.end()) return 1;
			if (*ai < *bi) return -1;
			else if (*ai>*bi) return 1;
			++ai,++bi;
		}
		if (bi!=rhs.end()) return -1;
		return 0;
	}

	int compare(const simple_string& rhs) const
	{
		using namespace std;
		int res(do_compare(rhs));
		return res;
	}

	int compare(const abstract_string& rhs) const
	{
		return -rhs.compare(*this);
	}

	void stream(ostream& out) const
	{
		// encoding agnostic byte per byte
//		for(auto i(begin());i!=(end());++i) out<<*i;
		out.write(content->data() + content_start, content_end - content_start);
	}

	const simple_string* segment_at(size_t &offset) const 
	{ 
		if (offset>=asciilen()) {offset-=asciilen();return 0;}
		else return this;
	}
};

class composite_string : public abstract_string
{
	//	TLP_ALLOCATOR(simple_string);
	friend class abstract_string;
	friend class string_iterator;
	CRef<abstract_string> lhs,rhs;
	composite_string(const abstract_string* lhs, const abstract_string* rhs):lhs(lhs),rhs(rhs) {
	}
protected:
	const simple_string* segment_at(size_t &offset) const 
	{
		auto seg(lhs->segment_at(offset));
		if (seg) return seg;
		else return rhs->segment_at(offset);
	}
public:
	size_t utf8len() const
	{
		return lhs->utf8len() + rhs->utf8len();
	}

	size_t asciilen() const
	{
		return lhs->asciilen() + rhs->asciilen();
	}

	CRef<abstract_string> skip(size_t num) const
	{
		if (num<1) return this;
		else if (lhs->utf8len() <= num) return rhs->skip(num - lhs->utf8len());
		else return append(lhs->skip(num),rhs);
	}

	int32_t _get_byte(size_t& at) const
	{
		auto tmp(lhs->get_byte(at));
		if (tmp>0) return tmp; else return rhs->get_byte(at);
	}

	CRef<abstract_string> take(size_t num) const
	{
		if (num > utf8len()) return this;
		else if (lhs->utf8len() < num) return append(lhs, rhs->take(num - lhs->utf8len()));
		else return lhs->take(num);
	}

	int compare(const abstract_string& cmp) const
	{
		auto cmp_seg(cmp.take(lhs->utf8len()) );
		int tmp(lhs->compare(*cmp_seg));
		if (cmp.utf8len() < lhs->utf8len()) return 1;
		if (tmp) return tmp;
		cmp_seg = cmp.skip(lhs->utf8len());
		tmp = rhs->compare(*cmp_seg);
		return tmp;
	}

	int compare(const simple_string& rhs) const
	{
		return compare((abstract_string&)rhs);		
	}

	void stream(ostream& out) const
	{
		lhs->stream(out);
		rhs->stream(out);
	}

	size_t hash(size_t seed) const {
		return rhs->hash(lhs->hash(seed));
	}
};



CRef<abstract_string> abstract_string::cons(const char *str)
{
	return simple_string::cons(str);
}

CRef<abstract_string> abstract_string::unique_instance(const abstract_string *str)
{
	CRef<abstract_string> hold(str);
	/*	struct cmp{bool operator()(const abstract_string* a, const abstract_string* b) 
	{
	return a->compare(*b)<0;
	}};*/

	struct hsh{size_t operator()(const abstract_string* a) const {return a->hash(1);}};
	struct cmp{bool operator()(const abstract_string* a, const abstract_string *b) const {return a->compare(*b)==0;}};
	static unordered_set<CRef<abstract_string>,hsh,cmp> memoized_strings;

	auto f(memoized_strings.find(str));
	if (f==memoized_strings.end())
		f = memoized_strings.insert(str).first;

	return *f;
}

CRef<abstract_string> abstract_string::append(const abstract_string* lhs, const abstract_string* rhs)
{
	if (lhs->utf8len()<1) return rhs;
	else if (rhs->utf8len()<1) return lhs;
	else return new composite_string(lhs,rhs);
}

string_iterator abstract_string::seek(size_t offset)
{
	string_iterator tmp;
	tmp.host = this;
	tmp.host_pos = offset;
	tmp.current = segment_at(offset);
	tmp.current_pos = offset;
	if (tmp.current == 0) tmp.host_pos = asciilen();
	return tmp;
}

bool operator<(const abstract_string& a, const abstract_string& b) {return a.compare(b)<0;}
bool operator>(const abstract_string& a, const abstract_string& b) {return a.compare(b)>0;}
bool operator==(const abstract_string& a, const abstract_string& b) {return a.compare(b)==0;}

void string_iterator::move_iter(int offset) const
{
	string_iterator *i(const_cast<string_iterator*>(this));
	auto new_pos(current_pos + offset);
	if (current == 0 || new_pos >= current->asciilen())
	{
		*i = host->seek(host_pos + offset);
		return;
	}
	else i->current_pos = new_pos;
	i->host_pos+=offset;
}

int32_t string_iterator::operator*() const
{
	return current->get_byte(current_pos);
}

