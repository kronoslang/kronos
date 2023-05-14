#pragma once

#include <assert.h>
#include <vector>
#include <atomic>

class NonAssignableCounter {
public:
	mutable int count;
	NonAssignableCounter():count(0){};
	NonAssignableCounter(const NonAssignableCounter& src):count(0) {}
	NonAssignableCounter(NonAssignableCounter&& src) noexcept :count(0) { }
	void operator=(const NonAssignableCounter& src) {}
	void operator=(NonAssignableCounter&& src) noexcept {}
};

class Counter {
public:
	int GetNumRefs() const {return _count.count;}
	void Attach() const { assert(_count.count < 0xfeeefeee); ++_count.count; }
	bool Detach() const { return --_count.count == 0; }
	int& _GetCount() const { return _count.count; }
	NonAssignableCounter _count;
};

class AtomicCounter {
	mutable std::atomic<int> count;
public:
	AtomicCounter() { count.store(0, std::memory_order_relaxed); }
	AtomicCounter(const AtomicCounter& src) { 
		count.store(0, std::memory_order_relaxed); 
	}
	AtomicCounter(AtomicCounter&& src) noexcept { 
		count.store(0, std::memory_order_relaxed); 
	}
	AtomicCounter& operator=(const AtomicCounter&) { return *this; }
	AtomicCounter& operator=(AtomicCounter&&) noexcept { return *this; }
	int GetNumRefs() const { return count.load(std::memory_order_acquire); }
	void Attach() const { count.fetch_add(1, std::memory_order_acquire); }
	bool Detach() const { return count.fetch_sub(1, std::memory_order_release) == 1; }
	int _GetCount() const { return count.load(std::memory_order_acquire); }
};

class RefCounting : public Counter {
public:
	virtual ~RefCounting() 
	{
		assert(_GetCount() < 1 && "Illegal deletion of referenced object");
#ifndef NDEBUG
		_GetCount() = -1;
#endif
	};
	
	bool Detach() const 
	{
		assert(_GetCount() > 0 && "Double delete detected");
		if (Counter::Detach()) {delete this;return true;}
		else return false;
	}
};

class AtomicRefCounting : public AtomicCounter {
public:
	virtual ~AtomicRefCounting() {
		assert(_GetCount() < 1 && "Illegal deletion of referenced object");
	};

	bool Detach() const {
		assert(_GetCount() > 0 && "Double delete detected");
		if (AtomicCounter::Detach()) { 
			delete this; 
			return true; 
		} else {
			return false;
		}
	}
};

template <class T> class Ref{
public:
	Ref()
	{
		ptr = 0;
	}

	template <typename... CTOR_ARGS> static Ref<T> Cons(CTOR_ARGS&&... arg) {return Ref(new T(std::forward<CTOR_ARGS>(arg)...));}

	Ref(T* ptr):ptr(ptr)
	{
		if (ptr) ptr->Attach();
	}

	Ref(const Ref& source)
	{
		ptr = source.ptr;
		if (ptr) ptr->Attach();
	}

	Ref(Ref&& source) noexcept
	{
		ptr = source.ptr;
		source.ptr = 0;
	}

	template<class SOURCE> Ref(const Ref<SOURCE> &from){
		ptr = from.Pointer();
		if (ptr) ptr->Attach();
	}

	~Ref()
	{
		if (ptr) ptr->Detach();
	}

	T* Pointer() const {return ptr;}
	T& Reference() const {return *ptr;}

	operator T*() const {return Pointer();}
	operator T&() const {return Reference();}

	T* operator->() const
	{
		return ptr;
	}

	T& operator*() const
	{
		return *ptr;
	}

	Ref<T>& operator=(const Ref<T>& source)
	{
		if (ptr) ptr->Detach();
		ptr = source.ptr;
		if (ptr) ptr->Attach();
		return *this;
	}

	Ref<T>& operator=(Ref<T>&& source)
	{
		std::swap(ptr, source.ptr);
		return *this;
	}

	void SetNull() {if (ptr) ptr->Detach(); ptr=0;}
	bool NotNull() const {return ptr!=0;}

	bool Unique() const {return (ptr->GetNumRefs() == 1);}

private:
	T* ptr;
};

template <class T> class CRef : public Ref<const T> 
{ public: template <typename ARG> CRef(ARG ptr):Ref<const T>(ptr) {} CRef(){} };

template <class T> Ref<T> MakeRef(T* ptr) {return Ref<T>(ptr);}

class LifecycleObject;
class LifecycleTracker {
public:
	virtual void ObjectExpired(LifecycleObject *which) = 0;
};

class LifecycleObject : private std::vector<LifecycleTracker*>{
public:
	LifecycleObject() {}
	LifecycleObject(const LifecycleObject& obj) {} // empty copy ctor
	~LifecycleObject();
	void _Track(LifecycleTracker *src) const;
	void _Untrack(LifecycleTracker *src) const;
};

template <class T>
class WeakRef : public LifecycleTracker{
	T* ptr;
public:
	WeakRef(const WeakRef& src) {ptr=src.ptr;if (ptr) ptr->_Track(this);}
	WeakRef& operator=(const WeakRef& src) {
		if (ptr) ptr->_Untrack(this); 
		ptr=src.ptr; 
		if (ptr) ptr->_Track(this);
		return*this;
	}
	WeakRef(T* src):ptr(src) {if (ptr) ptr->_Track(this);}
	~WeakRef() {if (ptr) ptr->_Untrack(this);}
	void ObjectExpired(LifecycleObject *obj) {assert(obj==ptr);ptr=0;}
	operator T*() {return ptr;}
	operator const T*() const {return ptr;}
	T* operator->() {return ptr;}
	const T* operator->() const {return ptr;}
};

template <class T>
class RefCounted : public RefCounting, public T{
	using Counter::_count;
	using Counter::_GetCount;
public:
	RefCounted(const T& src):T(src) {}
	~RefCounted() 
	{
		assert(_GetCount() < 1 && "Illegal deletion of referenced object");
		_GetCount() = -1;
	};
	bool Detach() const 
	{
		assert(_GetCount() > 0 && "Double delete detected");
		if (--_GetCount() < 1) {delete this;return true;}
		else return false;
	}
};
