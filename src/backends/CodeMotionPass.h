#pragma once

#include <unordered_map>
#include <map>

#include "NodeBases.h"
#include "TypeAlgebra.h"

template <typename T> class subvector{
	T *const b, *const e;
public:
	subvector(T* beg, T* end):b(beg),e(end){}
	T* begin() {return b;}
	T* end() {return e;}
};

namespace K3 {
	namespace Nodes{class FunctionBase;}
	using namespace Nodes;

	namespace Backends{

		class EquivalentExpression{
			Ref<RefCounted<EquivalentExpression>> pairLhs;
			Ref<RefCounted<EquivalentExpression>> pairRhs;
		public:
			static const unsigned MaximumTrackDistance = 8;
			Typed* inlinedExpr;
			int distanceFromLeaf;
			EquivalentExpression(Typed* i = 0, int d = MaximumTrackDistance):inlinedExpr(i),distanceFromLeaf(d) {}
			EquivalentExpression First() const;
			EquivalentExpression Rest() const;
			bool IsPair() const {return pairLhs.NotNull() || pairRhs.NotNull();}
			static EquivalentExpression Pair(const EquivalentExpression& lhs, const EquivalentExpression& rhs);
		};

		typedef std::vector<CTRef> UniquePathVector;
		struct ExpressionOccurrences{
			ExpressionOccurrences():count(0){}
			std::vector<UniquePathVector> Occurrences;
			unsigned count;
		};
		typedef std::unordered_map<Graph<Typed>,ExpressionOccurrences> EquivalenceClassMap;

		/* traces dataflows across function boundaries */
		class CodeMotionAnalysis : public CachedTransform<const Typed,EquivalentExpression> {
			EquivalenceClassMap& equivalenceClasses;
			EquivalentExpression argument;
			EquivalentExpression operate(CTRef source);
			EquivalentExpression _operateInsertCache(CTRef source);
			size_t codeVectorSize;
			CodeMotionAnalysis* parent;
			CTRef enclosing;
			void FillUniquePath(UniquePathVector& dest);
			void AddExpressionClass(CTRef from, const EquivalentExpression&);
		public:
			CodeMotionAnalysis(CTRef root, EquivalenceClassMap& eqClassMap, EquivalentExpression arg, CodeMotionAnalysis* parent, CTRef enclosing, size_t vectorSize = 1);
			UniquePathVector GetUniquePath(CTRef toNode);
		};

		class CodeMotionPass : public Transform::Identity<const Typed>, 
							   public std::unordered_set<Graph<Typed>> 
		{
			EquivalenceClassMap& equivalenceClasses;
			CTRef convertToDynamicVariable(CTRef source);
			std::unordered_map<CTRef,CTRef> substitutions;
			std::unordered_set<const void*> variableBoundaries;
			Typed* materializeVariables(CodeMotionPass& cmp, Typed* call);
			bool materializeAll;
		public:
			CodeMotionPass(CTRef root, EquivalenceClassMap& eqClassMap, bool ultimateBoundary = false);
			CTRef operate(CTRef src);
			operator CTRef();
		};
	};
};



