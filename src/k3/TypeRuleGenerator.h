#pragma once

#include <unordered_set>
#include <limits>
#include <algorithm>

#include "common/Ref.h"
#include "Type.h"
#include "RegionNode.h"

namespace K3 {
	namespace Nodes {
		class Generic;
	};

	struct AxiomRule {
		Nodes::CGRef first;
        Type match;
		bool invert;
		bool operator==(const AxiomRule& ar) const;

		struct Hash {
			size_t operator()(const AxiomRule&) const;
		};
        
        AxiomRule(Nodes::CGRef f, Type m, bool i):first(f), match(m), invert(i) {
            assert(m.kind != Type::RuleGeneratorType);
        }
	};

	class RangeRule {
		Type minimum;
		Type maximum;
	public:
		bool closed_min, closed_max;
		const Type& GetMinimum() const { return minimum; }
		const Type& GetMaximum() const { return maximum; }
		bool Contains(Type value);

		RangeRule(Type min,
			Type max,
			bool open_min = false,
			bool open_max = false)
            :minimum(min.Fix(Type::GenerateNoRules)), maximum(max.Fix(Type::GenerateNoRules)), closed_min(!open_min), closed_max(!open_max) { }

		RangeRule& operator+=(const RangeRule& merge) {
			if (merge.minimum.IsNil( ) == false) {
				if (minimum.IsNil( ) || merge.minimum > minimum) {
					minimum = merge.minimum;
					closed_min = merge.closed_min;
				} else if (merge.minimum == minimum) closed_min |= merge.closed_min;
			}

			if (merge.maximum.IsNil( ) == false) {
				if (maximum.IsNil( ) || merge.maximum < maximum) {
					maximum = merge.maximum;
					closed_max = merge.closed_max;
				} else if (merge.maximum == maximum) closed_max |= merge.closed_max;
			}

			return *this;
		}

		static RangeRule Maximum(Type max, bool open) {
			return RangeRule(Type::Nil, max, false, open);
		}
		static RangeRule Minimum(Type min, bool open) {
			return RangeRule(min, Type::Nil, open, false);
		}
	};

	class TypeRuleSet : public RegionAllocator {
		bool allowNewRules;
	public:
		
		struct CGRefHash {
			size_t operator()(Nodes::CGRef ref) const;
		};

		struct CGRefEq {
			bool operator()(Nodes::CGRef lhs, Nodes::CGRef rhs) const;
		};

		typedef std::unordered_map<Nodes::CGRef, RangeRule, CGRefHash, CGRefEq> NumericalRuleSet;
		typedef NumericalRuleSet::value_type NumericalRule;
		typedef std::unordered_set<AxiomRule, AxiomRule::Hash> AxiomSet;
	private:
		NumericalRuleSet RangeRules;
		AxiomSet FixedRules;

		bool IsTypeFixed(Nodes::CGRef) const;
		void RemoveBroadRulesOn(Nodes::CGRef);

		static std::pair<bool, int64_t> SolveForNumericalRule(const NumericalRule &);
		static std::pair<bool, int64_t> SolveForFixedRule(const AxiomRule &);
		std::pair<bool, Nodes::CGRef> _GetArgumentBundle(const Type& outer, const Type& inner, Nodes::CGRef evolutionFor, Nodes::CGRef SCEV);
		int64_t SolveForRule(const NumericalRule&, std::int64_t limit);
		int64_t SolveForRule(const AxiomRule&, std::int64_t limit);
	public:
		~TypeRuleSet();
		TypeRuleSet() {
            AcceptMoreRules(true);
#ifndef NDEBUG
            assert((liveRuleGenerators = 0) == 0);
#endif
        }

		void PrintRules(std::ostream&);
		
		void AcceptMoreRules(bool a) { allowNewRules = a; }

		TypeRuleSet& operator+=(const NumericalRule& nRule);
		TypeRuleSet& operator+=(const AxiomRule& aRule);

		int64_t SolveRecursionDepth(Nodes::CGRef argBundle);

		Nodes::CGRef GetArgumentBundle(const Type& outer, const Type& inner, Nodes::CGRef arg);

#ifndef NDEBUG
		int liveRuleGenerators;
#endif
	};

	class TypeRuleGenerator : public RefCounting {
		/* encapsulation leaks jeopardize rule set integrity */
		friend class TypeRuleSet;
		Type templateType;
		Nodes::CGRef scev;
		TypeRuleSet& ruleSet;
		TypeRuleGenerator(Type templateType, Nodes::CGRef src, TypeRuleSet& masterRules);
	public:
		TypeRuleGenerator(Type templte, TypeRuleSet& masterRules);
		virtual ~TypeRuleGenerator();

		Ref<TypeRuleGenerator> New(Type templateType, Nodes::CGRef src) const {
			return new TypeRuleGenerator(templateType,src,ruleSet);
		}

		Type GetTemplateType(Type::TypeFixingRules generate = Type::GenerateRulesForAllTypes) const;
		Type First(bool doLift = true) const;
		Type Rest(bool doLift = true) const;
		Type PairTo(const Type& first) const;
		Type Add(const Type& rhs) const;
		Type Sub(const Type& rhs) const;	
		Type UnwrapUserType(bool generateRules = true) const;
		Type TypeOf(bool generateRules = true) const;
		int OrdinalCompare(const Type& rhs, bool generateRules) const;
		bool IsPair() const;
		bool IsInvariant() const;
		bool IsUserType() const {return templateType.IsUserType();}
		bool IsEqual(const Type&, bool generateRules) const;
		size_t GetSize() const {return templateType.GetSize();}
		size_t GetHash() const ;
		void OutputText(std::ostream& strm, const void*, bool) const;
		Nodes::CGRef GetSCEV() const { return scev; }
	};
};
