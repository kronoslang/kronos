#pragma once
#include "common/Enumerable.h"
#include "Type.h"

namespace K3 {
	LAZY_ENUMERATOR(Type,TupleTypeEnumerator)
	{
		const Type& tuple;
		Type cur;
		TupleTypeEnumerator(const Type& t):tuple(t) {}
		LAZY_BEGIN
			cur = tuple;
			while(cur.IsPair())
			{
				LAZY_YIELD(cur.First());
				cur = cur.Rest();
			}
			LAZY_YIELD(cur);
		LAZY_END
	};
};