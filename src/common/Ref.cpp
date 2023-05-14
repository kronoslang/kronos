#include "Ref.h"

LifecycleObject::~LifecycleObject()
{
	for(auto i(begin());i!=end();++i)
		(*i)->ObjectExpired(this);
}

void LifecycleObject::_Track(LifecycleTracker *t) const
{
	for(auto i(begin());i!=end();++i)
	{
		if (*i == t) return;
	}
	LifecycleObject *o = const_cast<LifecycleObject*>(this);
	o->push_back(t);
}

void LifecycleObject::_Untrack(LifecycleTracker *t) const
{
	LifecycleObject *o = const_cast<LifecycleObject*>(this);
	for(auto i(o->begin());i!=o->end();)
	{
		if (t == *i) i = o->erase(i);
		else ++i;
	}
}