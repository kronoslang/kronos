#pragma once

#include <cstdint>

#define HASH_KEY_TYPE	unsigned
//#define HASH_CLASSID(ptr) (((HASH_KEY_TYPE)ptr) * 0x9e3779b97f4a7c13)
#define HASH_CLASSID(ptr) ((static_cast<HASH_KEY_TYPE>((uintptr_t)ptr)) * 2654435761)
#include "PreprocessorMeta.h"

class Reflecting{
protected:
	static const char *ClassID() {return "Reflecting";}
	virtual const void *GetClassPtr(const char *) const {return 0;}
	virtual const char *PrettyName() const = 0;
public:
	static const HASH_KEY_TYPE __bloom() {return 0UL;}
	virtual const HASH_KEY_TYPE GetBaseClassBloomFilter() const {return __bloom();}
//	virtual ~Reflecting(){}; be careful to define this for derived classes when necessary!
// can't be here because it prevents derived classes from having trivial dtors

	template<class WHICH>
	WHICH *Cast() const
	{
		HASH_KEY_TYPE requestedType(HASH_CLASSID(WHICH::ClassID()));
		if ((requestedType & GetBaseClassBloomFilter()) == requestedType)
		{
			return (WHICH*)GetClassPtr(WHICH::ClassID());
		}
		else
		{
			return (WHICH*)0;
		}
	}

	template<class WHICH>
	WHICH *Cast(WHICH* &dst) const
	{
		dst = Cast<WHICH>();
		return dst;
	}

	virtual const char *TypeID() const =0;
};

template <class CAST_TO>
CAST_TO *Cast0Chk(Reflecting *object)
{
	if (object)
	{
		return object->Cast<CAST_TO>();
	}
	else 
	{
		return 0;
	}
}

template <class CAST_TO>
CAST_TO *Cast0Chk(Reflecting *object, CAST_TO* &dst)
{
	if (object)
	{
		return object->Cast(dst);
	}
	else 
	{
		dst = 0;
		return 0;
	}

}

template <class T>
bool IsOfExactType(const Reflecting *ref)
{
	return (T::ClassID() == ref->TypeID());
}

template <class T>
bool NotOfExactType(const Reflecting *ref)
{
	return (T::ClassID() != ref->TypeID());
}

template <class T>
bool IsOfCompatibleType(const Reflecting *ref)
{
	return (ref->Cast<T>()!=0);
}

template <class T>
T* ShallowCast(const Reflecting *ref)
{
	if (IsOfExactType<T>(ref)) return ref->Cast<T>();
	else return 0;
}

#define MAKE_IDENT(file,counter) file "(" #counter ")"

#define REFLECT_INHERIT_HEADER(CLASS) \
    public:static const char *ClassID() {static char ident[]=#CLASS "_" MAKE_IDENT(__FILE__,__COUNTER__);return ident;}

#define REFLECT_CHECK_BASE(CLASS) \
	tmp=CLASS::GetClassPtr(id);if(tmp) return tmp;

#define REFLECT_INHERIT_DISPATCH(CLASS,...) \
	const void* GetClassPtr(const char* id) const override {CLASS::__inheritance_check();const void*tmp(0);\
	if (id == CLASS::ClassID()) return this; \
	META_MAP(REFLECT_CHECK_BASE,__VA_ARGS__) \
	return (const void*)0;}

#define REFLECT_BLOOM_OR(BASE) | BASE::__bloom()

#define REFLECT_INHERIT_BLOOM_FILTER(CLASS,...) \
	public:static const HASH_KEY_TYPE __bloom() {\
	static const HASH_KEY_TYPE __BLOOM = HASH_CLASSID(CLASS::ClassID()) META_MAP(REFLECT_BLOOM_OR,__VA_ARGS__);return __BLOOM;}

#define REFLECT_INHERIT_FOOTER(CLASS) \
	public:const char *TypeID() const override {return CLASS::ClassID();} \
	const HASH_KEY_TYPE GetBaseClassBloomFilter() const override {return __bloom();}\
	private:static void __inheritance_check(){};

#define INHERIT_RENAME(CLASS,...) \
	REFLECT_INHERIT_HEADER(CLASS) \
	REFLECT_INHERIT_DISPATCH(CLASS,__VA_ARGS__) \
	REFLECT_INHERIT_BLOOM_FILTER(CLASS,__VA_ARGS__) \
	REFLECT_INHERIT_FOOTER(CLASS)

#define INHERIT(CLASS,...) \
	const char *PrettyName() const override {return #CLASS;}\
	INHERIT_RENAME(CLASS,__VA_ARGS__)


#define REFLECTING_CLASS	public virtual Reflecting
