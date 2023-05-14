#pragma once
#include <functional>
#include <cassert>
#include <vector>


namespace Qxx
{
	enum FromTypes
	{
		DETECT_UNKNOWN = 1,
		DETECT_CONTAINER, 
		DETECT_ENUMERABLE, 
		DETECT_ENUMERATOR,
		DETECT_CARRAY
	};

	// SFINAE for type classification
	template <typename T>
	struct detect_type{
		typedef char detect_unknown[DETECT_UNKNOWN];
		typedef char detect_container[DETECT_CONTAINER];
		typedef char detect_enumerable[DETECT_ENUMERABLE];
		typedef char detect_enumerator[DETECT_ENUMERATOR];
		typedef char detect_carray[DETECT_CARRAY];
		template <typename C>
		static detect_enumerable& test(typename C::enumerator_t* = (typename C::enumerator_t*)0);
		template <typename C>
		static detect_container& test(typename C::iterator* = (typename C::iterator*)0);
		template <typename C>
		static detect_enumerator& test(typename C::__is_enumerator = 0);
		template <typename C>
		static detect_unknown& test(...);
		static const int value = sizeof(test<T>(0));
	};

	template <int TYPE, typename T> struct _From 
	{
		void Convert(const T&);
	};

	/*
		This class holds the current value for a lazy enumerator

		If the value type of the enumerator is a reference, hold
		a pointer to an externally held value

		Otherwise, make an internally held copy.
	*/
	template <typename T, bool REFERENCE> class LazyValue;
	template <typename T> class LazyValue<T,true>
	{
		typename std::remove_reference<T>::type *__shared;
	public:
		operator const T&() const {return *__shared;}
		LazyValue<T,true>& operator=(const T& v) {__shared=&v;return *this;}
	};

	template <typename T> class LazyValue<T,false>
	{
		typename std::remove_const<T>::type __held;
	public:
		operator const T&() const {return __held;}
		LazyValue<T,false>& operator=(const T& v) {__held=v;return *this;}
	};

	/* base class for lazy enumerators built with macros below */
	template <typename T> class LazyEnumerator 
	{
		LazyValue<T,std::is_reference<T>::value> __val;
	protected:
		int __state;
		void __yield(const T& v) {__val = v;}
	public:
		typedef typename std::remove_reference<T>::type value_type;
		typedef int __is_enumerator;
		LazyEnumerator():__state(0) {}
		const T& Current() const {assert(__state>0 && "MoveNext() before calling Current()!");return __val;}
//		void Reset() {__state=0;}
		bool Empty() {return __state<0;}
	};

	/* macros to assist the creation of lazy enumerators */
#define LAZY_ENUMERATOR(TYPE,FUNCTION_NAME) struct FUNCTION_NAME: public Qxx::LazyEnumerator<TYPE>
#define DECLARE_LAZY_ENUMERATOR(TYPE,FUNCTION_NAME) LAZY_ENUMERATOR(TYPE,FUNCTION_NAME) {bool MoveNext();}
#define LAZY_BEGIN bool MoveNext() {static const int __BASE_ID(__COUNTER__); switch(this->__state) {case 0:;
#define _LAZY_YIELD(VALUE,ID) {this->__yield(VALUE); this->__state=(ID-__BASE_ID);return true; case (ID-__BASE_ID):;}
#define LAZY_YIELD(VALUE) _LAZY_YIELD(VALUE,__COUNTER__)
#define LAZY_END case -1:this->__state=-1;}return false;} typedef int __is_enumerator;
 
	/* an enumerator that lazily filters a collection using a predicate */
	template<typename SOURCE> struct WhereEnumerator: public SOURCE
	{
		typedef std::function<bool(typename SOURCE::value_type)> predicate_t;
		predicate_t pred;
		WhereEnumerator(){}
		WhereEnumerator(const SOURCE& src, const predicate_t& pred) :SOURCE(src), pred(pred) { }
		bool MoveNext()
		{
			do
			{
				if (SOURCE::MoveNext() == false) return false;
			} 
			while(!pred(this->Current()));
			return true;
		}
	};

	/* an enumerator that lazily projects a collection using a transform function */
	template<typename SOURCE,typename RESULT> struct SelectEnumerator: public SOURCE
	{
		typedef std::function<RESULT(typename SOURCE::value_type)> selector_t;
		typedef RESULT value_type;
		selector_t sel;
		SelectEnumerator(){}
		SelectEnumerator(const SOURCE& src, const selector_t& s):SOURCE(src), sel(s) { }
		RESULT Current() const 
		{
			return sel(SOURCE::Current());
		}
	};

	/* a zipping enumerator */
	template <typename LHS, typename RHS, typename RESULT> struct ZipEnumerator
	{
		typedef std::function<RESULT(typename LHS::value_type, typename RHS::value_type)> zipper_t;
		typedef RESULT value_type;
		zipper_t zip;
		LHS left;
		RHS right;
		ZipEnumerator(){}
		ZipEnumerator(const LHS& left, const RHS& right, const zipper_t& z):zip(z),
			left(left),right(right) {}

		RESULT Current() const
		{
			return zip(left.Current(),right.Current());
		}

		bool MoveNext()
		{
			return left.MoveNext() && right.MoveNext();
		}
	};

	/* an enumerator that skips N elements of a collection */
	template<typename SOURCE> struct SkipEnumerator: public SOURCE
	{
		typedef typename SOURCE::value_type value_type;
		int _num;
		SkipEnumerator(){}
		SkipEnumerator(const SOURCE& src, int num):_num(num),SOURCE(src) {}
		bool MoveNext()
		{
			do
			{
				if (SOURCE::MoveNext() == false) return false;
			} while(_num-->0);
			return true;
		}
	};

	template <typename SRC1, typename SRC2> struct UnionEnumerator
	{
		typedef typename SRC1::value_type value_type;
		bool inFirst;
		SRC1 e1;
		SRC2 e2;
		UnionEnumerator(){}
		UnionEnumerator(const SRC1& a, const SRC2& b):e1(a),e2(b),inFirst(true) {}
		bool MoveNext()
		{
			return inFirst?e1.MoveNext():e2.MoveNext();
		}

		auto Current() const -> decltype(SRC1().Current())
		{
			return inFirst?e1.Current():e2.Current();
		}
	};

	/* an enumerator that retains only N elements of a collection */
	template<typename SOURCE> struct TakeEnumerator: public SOURCE
	{
		typedef typename SOURCE::value_type value_type;
		int _num;
		TakeEnumerator(){}
		TakeEnumerator(const SOURCE& src, int num):_num(num),SOURCE(src) {}
		bool MoveNext()
		{
			if(_num>0)
			{
				_num--;
				return SOURCE::MoveNext();
			}
			else return false;
		}
	};

	template <typename SOURCE> struct SelectManyEnumerator;


	/* provides a STL iterator from an enumerator */
	template <typename ENUM_T> class EnumeratorIterator
	{
		ENUM_T en;
		bool at_end;
	public:
		EnumeratorIterator():at_end(true) {}
		EnumeratorIterator(const ENUM_T e):en(e)
		{
			at_end = !en.MoveNext();
		}
		bool operator!=(const EnumeratorIterator<ENUM_T>& rhs) const 
		{
			return at_end!=rhs.at_end;
		}
		EnumeratorIterator<ENUM_T>& operator++() {at_end=!en.MoveNext();return *this;}
		EnumeratorIterator<ENUM_T>& operator++(int) {auto tmp(*this);operator++();return tmp;}
		auto operator*() const -> decltype(ENUM_T().Current()) { return en.Current(); }
		
		typedef typename ENUM_T::value_type value_type;
	};

	/* provides an enumerator from STL iterator */
	template <typename ITERATOR, typename VALUE_TYPE> 
	LAZY_ENUMERATOR(const VALUE_TYPE&,IteratorEnumerator)
	{
	private:
		ITERATOR begin,cur,end;
		bool empty;
	public:
		IteratorEnumerator():empty(true){}
		IteratorEnumerator(const ITERATOR& b, const ITERATOR& e):begin(b),end(e),empty(false){}
		LAZY_BEGIN
			for(cur=begin;!empty && cur!=end;++cur) LAZY_YIELD(*cur);
		LAZY_END
	};

	/* flattens STL ranges */
	template <typename TOP, typename SUB> 
	LAZY_ENUMERATOR(const typename SUB::value_type&,FlattenRanges)
	{
		TOP e;
		SUB sub_b,sub_e;
		LAZY_BEGIN
			while(e.MoveNext())
			{
				sub_b = e.Current().begin();
				sub_e = e.Current().end();
				for(;sub_b!=sub_e;++sub_b) LAZY_YIELD(*sub_b);
			}
		LAZY_END
	};

	/* flattens C arrays */
	template <typename TOP, typename SUB_ELEM> 
	LAZY_ENUMERATOR(const SUB_ELEM&,FlattenArrays)
	{
		TOP e;
		const SUB_ELEM *sub;
		size_t idx,len;
		LAZY_BEGIN
			while(e.MoveNext()) 
			{
				sub = (const SUB_ELEM*)e.Current();
				for(idx=0;idx<len;++idx) LAZY_YIELD(sub[idx]);
			}
		LAZY_END
	};

	/* flattens query enumerables */
	template <typename TOP, typename SUB>
	LAZY_ENUMERATOR(const typename SUB::value_type&,FlattenEnumerables)
	{
		TOP e;
		SUB sub;
		LAZY_BEGIN
			while(e.MoveNext())
			{
				sub = e.Current().GetEnumerator();
				while(sub.MoveNext()) LAZY_YIELD(sub.Current());
			}
		LAZY_END
	};

	/* chooses an approprieate flattening method for a container */
	template <typename ENUMERABLE,int MODE> struct _SelectManyHelper
	{
		static const ENUMERABLE& Flatten(const ENUMERABLE& c) {return c;}
	};

	template <typename ENUMERABLE> struct _SelectManyHelper<ENUMERABLE,DETECT_CONTAINER>
	{
		static FlattenRanges<typename ENUMERABLE::enumerator_t,
							 typename ENUMERABLE::value_type::const_iterator> 
				Flatten(const ENUMERABLE& e)
		{
			FlattenRanges<typename ENUMERABLE::enumerator_t,
            typename ENUMERABLE::enumerator_t::value_type::const_iterator> tmp;
			tmp.e = e.GetEnumerator();
			return tmp;
		}
	};

	/* unknown types; attempt flattening as C array */
	template <typename ENUMERABLE> struct _SelectManyHelper<ENUMERABLE,DETECT_UNKNOWN>
	{
		static FlattenArrays<typename ENUMERABLE::enumerator_t,
						  typename std::remove_extent<typename ENUMERABLE::value_type>::type>			
			Flatten(const ENUMERABLE& e)
		{
			FlattenArrays<typename ENUMERABLE::enumerator_t,
						  typename std::remove_extent<typename ENUMERABLE::value_type>::type> tmp;
			tmp.e = e.GetEnumerator();
			tmp.len = std::extent<typename ENUMERABLE::value_type>::value;
			return tmp;
		}
	};

	template <typename ENUMERABLE> struct _SelectManyHelper<ENUMERABLE,DETECT_ENUMERABLE>
	{
		static FlattenEnumerables<typename ENUMERABLE::enumerator_t,
								  typename ENUMERABLE::value_type::enumerator_t> 
				Flatten(const ENUMERABLE& e)
		{
			FlattenEnumerables<typename ENUMERABLE::enumerator_t,
						 typename ENUMERABLE::value_type::enumerator_t> tmp;
			tmp.e = e.GetEnumerator();
			return tmp;
		}
	};

	template <typename T>
	T MakeMutable(const T&);

	/* a class that represents an enumerable container that can be queried */
	template <typename ENUMERATOR> class Enumerable
	{
	public:
		typedef ENUMERATOR enumerator_t;
		typedef typename ENUMERATOR::value_type value_type;
	private:
		typedef Enumerable<ENUMERATOR> _Myt;
	protected:
		enumerator_t start;
	public:

		template <typename INNER_ENUMERATOR>
		static Enumerable<INNER_ENUMERATOR> FromEnumerator(const INNER_ENUMERATOR& e)
		{
			return Enumerable<INNER_ENUMERATOR>(e);
		}

		Enumerable<enumerator_t>(){}
		Enumerable<enumerator_t>(const enumerator_t &start):start(start) {}
		enumerator_t GetEnumerator() const {return start;}

		Enumerable<WhereEnumerator<enumerator_t>> Where(const typename WhereEnumerator<enumerator_t>::predicate_t& pred) const
		{
			return Enumerable<WhereEnumerator<enumerator_t>>(
				WhereEnumerator<enumerator_t>(GetEnumerator(),pred));
		}
		
		template <class SELECTOR> auto Select(const SELECTOR& sel) const 
			-> Enumerable<SelectEnumerator<enumerator_t,decltype(sel(std::declval<value_type>()))>>
		{
			return Enumerable<SelectEnumerator<enumerator_t,decltype(sel(std::declval<value_type>()))>>(
				SelectEnumerator<enumerator_t,decltype(sel(std::declval<value_type>()))>(GetEnumerator(),sel));
		}

		template <class ZIPPER, class RHS> auto Zip(const ZIPPER& zip, const RHS& with) const
			-> Enumerable<ZipEnumerator<enumerator_t,typename RHS::enumerator_t,
										decltype(zip(std::declval<value_type>(),std::declval<typename RHS::value_type>()))>>
		{
			typedef decltype(zip(std::declval<value_type>(),std::declval<typename RHS::value_type>())) result_t;
			typedef ZipEnumerator<enumerator_t,typename RHS::enumerator_t,result_t> new_enum_t;
			return Enumerable<new_enum_t>(new_enum_t(GetEnumerator(),with.GetEnumerator(),zip));
		}

		Enumerable<TakeEnumerator<enumerator_t>> Take(int number) const
		{
			return Enumerable<TakeEnumerator<enumerator_t>>(
				TakeEnumerator<enumerator_t>(GetEnumerator(),number));
		}

		Enumerable<SkipEnumerator<enumerator_t>> Skip(int number) const
		{
			return Enumerable<SkipEnumerator<enumerator_t>>(
				SkipEnumerator<enumerator_t>(GetEnumerator(),number));
		}

		//auto SelectMany( ) const -> Enumerable<decltype(_SelectManyHelper<_Myt, detect_type<value_type>::value>::Flatten(*this))>
		//{
		//	auto tmp(_SelectManyHelper<_Myt,detect_type<value_type>::value>::Flatten(*this));
		//	return Enumerable<decltype(tmp)>(tmp);
		//}


		//template <typename SELECTOR>
		//auto SelectMany(const SELECTOR& sel)
		//	-> Enumerable<decltype(SelectMany().Select(sel))>
		//{
		//	return SelectMany().Select(sel);
		//}

		template <typename WITH> 
		Enumerable<UnionEnumerator<enumerator_t,decltype(From(WITH()).GetEnumerator())>> Union(const WITH& c)
		{
			auto tmp(From(c));
			return UnionEnumerator<enumerator_t,decltype(tmp.GetEnumerator())>(GetEnumerator(),tmp.GetEnumerator());
		}

		template <typename AGGREGATOR, typename ACCUM_T> 
		static ACCUM_T Aggregate(enumerator_t e, ACCUM_T seed, const AGGREGATOR& a)
		{
			while(e.MoveNext()) seed = a(seed,e.Current());
			return seed;
		}

		template <typename AGGREGATOR> auto Aggregate(const AGGREGATOR& a) const
			-> typename std::remove_reference<decltype(a(std::declval<value_type>(),std::declval<value_type>()))>::type
		{
			auto e(GetEnumerator());
			e.MoveNext();
			return Aggregate(e,e.Current(),a);
		}

		bool Contains(const value_type& v) {
			return Where([&v](const value_type& x) { return x == v; }).Any();
		}

	private:
		template <typename T> 
		static T* __Cast(value_type c) {return c->template Cast<T>();}
		static bool __NotNull(void *ptr) {return ptr!=0;}
	public:

		template <typename T>
		auto OfType() -> decltype(std::declval<_Myt>().Select(__Cast<T>).Where(__NotNull))
		{
			return Select(__Cast<T>).Where(__NotNull);
		}

		template <typename AGGREGATOR, typename ACCUM_T>
		ACCUM_T Aggregate(ACCUM_T seed, const AGGREGATOR& a) const {
			return Aggregate(GetEnumerator(),seed,a);
		}

		value_type FirstOrDefault(const value_type& default_value = value_type()) const {
			auto e(GetEnumerator());
			return e.MoveNext()?e.Current():default_value;
		}

		const value_type& First() {
            auto e(GetEnumerator());
			e.MoveNext();return e.Current();
		}

		bool AtLeast(int atLeast) const
		{
			if (atLeast < 1) return true;
			auto tmp(GetEnumerator());
			while(tmp.MoveNext())
			{
				if (--atLeast == 0) return true;
			}
			return false;
		}

		bool Any() const {return AtLeast(1);}

		template <typename CONTAINER>
		CONTAINER ToContainer() const
		{
			CONTAINER tmp;
			ToContainer(tmp);
			return tmp;
		}

		template <typename CONTAINER>
		void ToContainer(CONTAINER& destination) const
		{
			destination.clear();
			auto e(GetEnumerator());
			while(e.MoveNext()) destination.insert(destination.end(),e.Current());
		}

		std::vector<typename std::remove_const<value_type>::type> ToVector() const
		{
			std::vector<typename std::remove_const<value_type>::type> tmp;
			ToContainer(tmp);
			return tmp;
		}

		template <typename T> void ToVector(std::vector<T>& destination) const
		{
			ToContainer(destination);
		}

		void Now()
		{
			auto e(GetEnumerator()); while(e.MoveNext()) e.Current();
		}


		typedef EnumeratorIterator<enumerator_t> const_iterator;

		const_iterator begin() const
		{
			return const_iterator(GetEnumerator());
		}

		const_iterator end() const 
		{
			return const_iterator();
		}
	};

	/* an enumerable that wraps a STL range */
	template <typename ITERATOR, typename VALUE_TYPE>
	class IteratorEnumerable : public Enumerable<IteratorEnumerator<ITERATOR,VALUE_TYPE>>
	{
	public:
		typedef IteratorEnumerator<ITERATOR,VALUE_TYPE> enumerator_t;
		typedef VALUE_TYPE value_type;
	private:
		ITERATOR __beg,__end;
		static IteratorEnumerator<ITERATOR,value_type> 
			ConstructEnumerator(const ITERATOR& b, const ITERATOR& e)
		{
			IteratorEnumerator<ITERATOR,value_type> tmp(b,e);
			return tmp;
		}
	public:

		IteratorEnumerable(const ITERATOR& start, const ITERATOR& end)
			:Enumerable<enumerator_t>(ConstructEnumerator(start, end)), __beg(start), __end(end)
		{

		}

		ITERATOR begin() const {return __beg;}
		ITERATOR end() const {return __end;}
	};
	
	/* provide enumerables from various sources */
	template <typename T> struct _From<DETECT_CONTAINER,T>
	{
		template <typename SOURCE> static auto Convert(const SOURCE& s)
			-> IteratorEnumerable<decltype(s.begin()),typename SOURCE::value_type>
		{
			return IteratorEnumerable<decltype(s.begin()),typename SOURCE::value_type>(s.begin(),s.end());
		}
	};

	template <typename T> struct _From<DETECT_ENUMERABLE,T>
	{
		static const T& Convert(const T& s) {return s;}
	};

	template <typename T> struct _From<DETECT_ENUMERATOR,T>
	{
		static Enumerable<T> Convert(const T& s) {return Enumerable<T>(s);}
	};

	/* enumerable from STL container, enumerator or enumerable */
	template <typename SOURCE> auto From(const SOURCE& src)
		-> decltype(_From<detect_type<SOURCE>::value,SOURCE>::Convert(src))
	{
		return _From<detect_type<SOURCE>::value,SOURCE>::Convert(src);		
	}

	/* enumerable from C array */
	template <typename ELEMENT, size_t LEN>
	IteratorEnumerable<ELEMENT*,ELEMENT> From(const ELEMENT(&table)[LEN])
	{
		return IteratorEnumerable<ELEMENT*,ELEMENT>((ELEMENT*)table,(ELEMENT*)table+LEN);
	}

	/* enumerable from pointer range */
	template <typename T>
	IteratorEnumerable<const T*,T> From(const T* beg, const T* end)
	{
		return IteratorEnumerable<const T*,T>(beg,end);
	}

	/* enumerable from STL iterator range */
	template <typename T>
	IteratorEnumerable<T,typename T::value_type> From(const T b, const T e)
	{
		return IteratorEnumerable<T,typename T::value_type>(b,e);
	}

	/* polymorphic interfaces for dynamic runtime */
	template <typename T> class IEnumeratorImplementation
	{
	public:
		virtual ~IEnumeratorImplementation() {};
		virtual IEnumeratorImplementation<T>* Copy() const = 0;
		virtual const T& Current() const = 0;
		virtual bool MoveNext() = 0;
	};

	template <typename T> class IEnumerator
	{
		IEnumeratorImplementation<T> *impl;
	public:
		typedef int __is_enumerator;
		typedef T value_type;
		IEnumerator(const IEnumerator<T>& src):impl(src.impl?src.impl->Copy():0) {}
		IEnumerator(IEnumerator<T>&& src):impl(src.impl) {src.impl = 0;}
		IEnumerator(IEnumeratorImplementation<T>* i = 0):impl(i) {}
		~IEnumerator(){delete impl;}

		IEnumerator<T>& operator=(const IEnumerator<T>& src) {impl = src.impl->Copy();}
		IEnumerator<T>& operator=(IEnumerator<T>&& src) {impl = src.impl; src.impl = 0;}

		const T& Current() const {return impl->Current();}
		bool MoveNext() {return impl?impl->MoveNext():false;}
	};

	template <class T> class IEnumerable : public Enumerable<IEnumerator<T>> 
	{
		template <typename ENUM> struct Adaptor : public IEnumeratorImplementation<T>
		{
			ENUM e;
			Adaptor(const ENUM& e):e(e){}
			const T& Current() const {return e.Current();}
			bool MoveNext() {return e.MoveNext();}
			Adaptor* Copy() const {return new Adaptor(e);}
		};
	public:
		typedef T value_type;
		typedef IEnumerator<T> enumerator_t;
		template <typename ENUM> IEnumerable(const Enumerable<ENUM>& src):Enumerable<IEnumerator<T>>(IEnumerator<T>(new Adaptor<ENUM>(src.GetEnumerator()))) {}
	};
};