#include <iostream>
#include <sstream>
#include <unordered_set>
#include <cmath>
#include <algorithm>

#include "common/PlatformUtils.h"

#include "TypeRuleGenerator.h"
#include "TypeAlgebra.h"
#include "Invariant.h"
#include "TypeAlgebra.h"
#include "Evaluate.h"

#include "Type.h"

#include "EnumerableGraph.h"

using namespace Qxx;
using namespace std;

namespace std {
	ostream& operator<<(ostream& strm, const K3::RangeRule& rr) {
		bool open_low(rr.GetMinimum().IsNil());
		bool open_hig(rr.GetMaximum().IsNil());
		if (open_low && open_hig) strm << " e {R}";
		else if (open_low) strm << (rr.closed_max?" <= ":" < ") << rr.GetMaximum();
		else if (open_hig) strm << (rr.closed_min?" >= ":" < ") << rr.GetMinimum();
		else if (rr.GetMinimum() == rr.GetMaximum()) strm << " = " << rr.GetMinimum();
		else strm << " e ["<<rr.GetMinimum()<<","<<rr.GetMaximum()<<"]";
		return strm;
	}

	template<> struct hash<K3::AxiomRule> {
		size_t operator()(const K3::AxiomRule& ar) const {
			return ar.first->GetHash() ^ ar.match.GetHash();
		}
	};
}

namespace K3
{
	 Nodes::CGRef SimplifyArityRequirement(Nodes::CGRef accessor, long double& arity);

	using namespace Nodes;
	using namespace Invariant;

	bool AxiomRule::operator==(const AxiomRule& ar) const {
		return *first == *ar.first && match == ar.match && invert == ar.invert;
	}

	size_t AxiomRule::Hash::operator()(const AxiomRule& ar) const {
		return ar.first->GetHash() ^ ar.match.GetHash();
	}

	TypeRuleGenerator::TypeRuleGenerator(Type tmpl, CGRef source,TypeRuleSet& ruleSet)
		:templateType(tmpl.Fix()),scev(source),ruleSet(ruleSet)
	{
#ifndef NDEBUG
		ruleSet.liveRuleGenerators++;
#endif
	}

	TypeRuleGenerator::TypeRuleGenerator(Type tmpl, TypeRuleSet& ruleSet):templateType(tmpl.Fix()),ruleSet(ruleSet)
	{
		RegionAllocator allocateFrom(ruleSet.GetRegion());
		scev = GenericArgument::New();
#ifndef NDEBUG
		ruleSet.liveRuleGenerators++;
#endif
	}

	TypeRuleGenerator::~TypeRuleGenerator() {
#ifndef NDEBUG
		ruleSet.liveRuleGenerators--;
#endif
	}

	size_t TypeRuleSet::CGRefHash::operator()(Nodes::CGRef ref) const {
		return ref->GetHash( );
	}

	bool TypeRuleSet::CGRefEq::operator()(Nodes::CGRef lhs, Nodes::CGRef rhs) const {
		return *lhs == *rhs;
	}

	Type TypeRuleGenerator::GetTemplateType(Type::TypeFixingRules fixRules) const
	{
		Type tmp(templateType);

		if (fixRules == Type::GenerateNoRules) return tmp;
		
		if (fixRules == Type::GenerateRulesForSizedTypes) {
			if (tmp.GetSize() < 1) return tmp;
			if (tmp.IsPair()) {
				Type fst = tmp.First();
				auto count = tmp.CountLeadingElements(fst);
				Type tail = tmp.Rest(count);

				if (fst.GetSize()) First(true);

				if (tail.GetSize() == 0) return tmp;
				else {
					RegionAllocator allocateFrom(ruleSet.GetRegion());
					auto accessor = scev;
					for(decltype(count) i=0;i<count;++i) accessor = GenericRest::New(accessor);
					return Type::Chain(fst,count,New(tail,accessor)->GetTemplateType(fixRules));
				}
			} else if (tmp.IsUserType()) {
				return Type::User(tmp.GetDescriptor(),UnwrapUserType(false).Fix(Type::GenerateRulesForSizedTypes));
			}
		}
		 
		RegionAllocator allocateFrom(ruleSet.GetRegion());
		// distill into final rule and discard rule generator
		ruleSet += AxiomRule{scev, tmp, false };
		return tmp;
	}

	/* type algebra lifting */
	Type TypeRuleGenerator::First(bool liftRules) const {
		RegionAllocator allocateFrom(ruleSet.GetRegion());
		// split rule generator

		if (liftRules && templateType.kind == Type::TupleType) {
			ruleSet += TypeRuleSet::NumericalRule(Arity::New(scev),
				RangeRule::Minimum(Type::InvariantLD(templateType.IsNilTerminated()?2.0L:2.0L),false));
		}
		
		if (templateType.First().IsNativeType()) {

		}
		auto sr = TypeRuleGenerator::New(templateType.First(), GenericFirst::New(scev));
		if (sr->templateType.IsNativeType()) {
			// no use to generate further rules
			return sr->GetTemplateType(liftRules ? Type::GenerateRulesForAllTypes : Type::GenerateNoRules);
		}
		return Type(sr.Pointer());
	}

	Type TypeRuleGenerator::Rest(bool liftRules) const {
		RegionAllocator allocateFrom(ruleSet.GetRegion());
		// distill arity rule

		size_t requiredArity = templateType.IsNilTerminated() ? 2 : 2;
		
		if (liftRules && templateType.kind == Type::TupleType) {
			ruleSet += TypeRuleSet::NumericalRule(Arity::New(scev), 
				RangeRule::Minimum(Type::InvariantU64(requiredArity), false));
		} 		
		return Type(TypeRuleGenerator::New(templateType.Rest(), GenericRest::New(scev)).Pointer());
	}

	Type TypeRuleGenerator::PairTo(const Type& first) const {
		if (first.IsRuleGenerator()) {
			// must preserve complete SCEV information
			RegionAllocator allocateFrom(ruleSet.GetRegion());
			return Type(
				TypeRuleGenerator::New(Type::Pair(first.GetRuleGenerator()->templateType, templateType),
					GenericPair::New(first.GetRuleGenerator()->GetSCEV(), scev)).Pointer());
		}

		if (templateType.kind == Type::TupleType && templateType.data.Tuple.Data->fst.OrdinalCompare(first,false) == 0) {
			RegionAllocator allocateFrom(ruleSet.GetRegion());
			return Type(TypeRuleGenerator::New(Type::Pair(first, templateType),
				GenericPair::New(Constant::New(templateType.First()),scev)).Pointer());
		} else {
			return Type(first, Type(this), 1);
		}
	}

	bool TypeRuleGenerator::IsInvariant() const {
		RegionAllocator allocateFrom(ruleSet.GetRegion());
		if (templateType.IsInvariant()) {
			return true; // do I need to add some kind of rule maybe?
		} else {
			/* trigger type fixing */
			return GetTemplateType().IsInvariant();
		}
	}

	bool TypeRuleGenerator::IsPair() const {		
		RegionAllocator allocateFrom(ruleSet.GetRegion());

		long double requiredArity = templateType.IsNilTerminated() ? 2 : 2;
		if (templateType.IsPair()) {
			ruleSet += TypeRuleSet::NumericalRule(Arity::New(scev),RangeRule::Minimum(Type::InvariantLD(requiredArity),false));
			return true;
		} else {
			ruleSet += TypeRuleSet::NumericalRule(Arity::New(scev),RangeRule::Maximum(Type::InvariantLD(requiredArity-1ul),false));
			return false;
		}
	}

	/* invariant arithmetic lifting */

	Type TypeRuleGenerator::Add(const Type& rhs) const {
		if (rhs.IsInvariant()) {
			RegionAllocator allocateFrom(ruleSet.GetRegion());
			return Type(TypeRuleGenerator::New(templateType + rhs, Invariant::GenericSCEVAdd::New(scev, (int)rhs.GetInvariantI64())).Pointer());
		} else {
			return GetTemplateType() + rhs;
		}
	}

	Type TypeRuleGenerator::Sub(const Type& rhs) const {
		if (rhs.IsInvariant()) {
			RegionAllocator allocateFrom(ruleSet.GetRegion());
			return Type(TypeRuleGenerator::New(templateType - rhs, Invariant::GenericSCEVAdd::New(scev, -(int)rhs.GetInvariantI64())).Pointer());
		}
		else {
			return GetTemplateType() - rhs;
		}
	}

	int TypeRuleGenerator::OrdinalCompare(const Type& rhs, bool generateRules) const {
		RegionAllocator allocateFrom(ruleSet.GetRegion());
		if (rhs.IsRuleGenerator()) {
			if (&rhs.data.RGen->ruleSet != &ruleSet) {
				return OrdinalCompare(rhs.Fix(Type::GenerateNoRules), generateRules);
			}
			// if two rule generators are identical, no additional rules are ever needed
			if (*scev== *rhs.data.RGen->scev &&
				templateType.OrdinalCompare(rhs.data.RGen->templateType,false) == 0) return 0;

			// find template result
			if (generateRules) {
				return OrdinalCompare(rhs.data.RGen->GetTemplateType(generateRules?Type::GenerateRulesForAllTypes:Type::GenerateNoRules), true);
			} else {
				return OrdinalCompare(rhs.data.RGen->templateType, false);
			}
		} else {			
			if (templateType.IsInvariant() && rhs.IsInvariant() && generateRules) {
				int result = templateType.OrdinalCompare(rhs,generateRules);
				switch(result) {
				case 0:
                    ruleSet += AxiomRule{scev, rhs.Fix(Type::GenerateNoRules), false };
					break;
				case 1:
					ruleSet += TypeRuleSet::NumericalRule(scev,RangeRule::Minimum(rhs,true));
					break;
				case -1:
					ruleSet += TypeRuleSet::NumericalRule(scev,RangeRule::Maximum(rhs,true));
					break;
				default:
					assert(0&&"Bad ordinality in comparison");break;
				}

				return result;
			} else {
				if (generateRules)
					return GetTemplateType().OrdinalCompare(rhs,true);
				else
					return templateType.OrdinalCompare(rhs,false);
			}
		}
	}

	bool TypeRuleGenerator::IsEqual(const Type& rhs, bool generateRules) const {
        if (rhs.kind == Type::RuleGeneratorType) {
            if (rhs.data.RGen == this) {
                if (*scev == *rhs.data.RGen->scev) return true;
                if (generateRules) {                    
                    ruleSet += AxiomRule{rhs.data.RGen->scev, rhs.data.RGen->templateType, false};
                }
            }
            return IsEqual(rhs.data.RGen->templateType, generateRules);
        }
        
		if (!generateRules) return templateType == rhs;
		auto matching = templateType.OrdinalCompare(rhs, false);
		ruleSet += AxiomRule{scev, rhs, matching != 0 };
		return matching == 0;
	}

	size_t TypeRuleGenerator::GetHash() const {
		size_t h(scev->GetHash());
		HASHER(h,templateType.GetHash());
		return h;
	}

	Type TypeRuleGenerator::UnwrapUserType(bool generateRules) const {
		RegionAllocator allocateFrom(ruleSet.GetRegion());
		return Type(TypeRuleGenerator::New(templateType.UnwrapUserType(),scev));
	}

	Type TypeRuleGenerator::TypeOf(bool generateRules) const {
		// this should generate some kind of rules, but for now we rely on argument evolution
		// to rule out typeclass mutation
		return templateType.TypeOf();
	}

	void TypeRuleGenerator::OutputText(std::ostream& stream, const void *instance, bool nested) const {
		stream<<"<<";
		templateType.OutputText(stream,instance,nested);
		stream<<">>";
	}

	TypeRuleSet::~TypeRuleSet() {
#ifndef NDEBUG
		if (liveRuleGenerators > 0) {
//			cerr << "RuleSet deleted with orphan rule generators\n";
		}
#endif
	}

	Nodes::CGRef SimplifyArityRequirement(Nodes::CGRef accessor, long double& arity) {
		/*
			Arity(pair(a,b)) = Arity(b)+1
			Arity(pair(a,b)) <> x  <==> Arity(b) <> x-1
		*/
		const GenericPair *p;
		while((p = ShallowCast<const GenericPair>(accessor))) {
			accessor = p->GetUp(1);
			arity-=1.0;
		}

		/* 
			Arity(rest(a)) = Arity(a)-1
			Arity(rest(a)) <> x  <==> Arity(a) <> x+1
		*/
		const GenericRest *r;
		while((r = ShallowCast<const GenericRest>(accessor)))  {
			accessor = r->GetUp(0);
			arity+=1.0;
		}

		return accessor;
	}

	void TypeRuleSet::RemoveBroadRulesOn(Nodes::CGRef key)
	{
		RangeRules.erase(key);
	}

	TypeRuleSet& TypeRuleSet::operator+=(const TypeRuleSet::NumericalRule& n)
	{
		if (allowNewRules) {
			auto rr(RangeRules.find(n.first));
			if (rr!=RangeRules.end()) {
				rr->second.operator+=(n.second);
			}
			else RangeRules.insert(n);
			n.first->HasInvisibleConnections( );
		}
		return *this;
	}

	TypeRuleSet& TypeRuleSet::operator+=(const AxiomRule& a)
	{
		//std::cout << *a.first << " |- " << a.second << "\n";
		if (allowNewRules) {
			if (FixedRules.insert(a).second == false) {
//				std::cout << *a.first << " " << a.match << " rejected\n";
			}
		}
		return *this;
	}

	void TypeRuleSet::PrintRules(std::ostream& o)
	{
		using namespace std;
		o << "[RULES]\n";
		for(auto ar : RangeRules)
		{
			o << *ar.first << ar.second << endl;
		}

		for(auto tcr : FixedRules)
		{
			o << *tcr.first << (tcr.invert ? " != " : " == ") << tcr.match << endl;
		}
	}

	CGRef Finalize(CGRef n, const Type& t)
	{
		return n?n:Invariant::Constant::New(t);
	}

	static CGRef GetTotalSCEV(const Type& inner) {
		if (inner.IsFixed()) {
			return Constant::New(inner);
		}
		if (inner.IsRuleGenerator()) {
			return inner.GetRuleGenerator()->GetSCEV();
		}
		if (inner.IsPair()) {
			return GenericPair::New(
				GetTotalSCEV(inner.First()),
				GetTotalSCEV(inner.Rest()));
		}
		if (inner.IsUserType()) {
			return GenericMake::New(Constant::New(Type(inner.GetDescriptor())), 
									GetTotalSCEV(inner.UnwrapUserType()));
		}
		assert(0 && "unreachable");
		abort();
	}

	CGRef TypeRuleSet::GetArgumentBundle(const Type& outer, const Type& inner, CGRef evolutionRoot)
	{
//		std::cout << outer << " -> " << inner << "\n";
		auto scev = GetTotalSCEV(inner);
//		std::cout << *scev << "\n";
		auto tmp(_GetArgumentBundle(outer,inner.Fix(Type::GenerateNoRules),evolutionRoot, scev));
		return tmp.first?Finalize(tmp.second,outer):0;
	}

	static bool InvariantSCEV(const Type& outer, const Type& inner, CGRef scev, CGRef match, int& delta) {
		if (outer == inner && *scev == *match) return true;
		Invariant::GenericSCEVAdd* add;
		if (scev->Cast(add)) {
			delta += add->GetDelta();
			return InvariantSCEV(outer + Type(add->GetDelta()),
				inner, add->GetUp(0), match, delta);
		}
		return false;
	}

	static bool TupleSCEV(const Type& outer, const Type& inner, CGRef scev, CGRef match, int& delta) {
		if (outer == inner && *scev == *match) return true;
		if (inner.IsPair() && outer.IsPair() && inner.First() != outer.First()) return false;

		GenericPair* p; 
		if (scev->Cast(p) && inner.IsPair()) {
			delta += 1;
			return TupleSCEV(outer, inner.Rest(), p->GetUp(1), match, delta);
		}

		GenericRest* r;
		if (scev->Cast(r) && outer.IsPair()) {
			delta -= 1;
			return TupleSCEV(outer.Rest(), inner, r->GetUp(0), match, delta);
		}
		return false;
	}

	pair<bool, CGRef> TypeRuleSet::_GetArgumentBundle(const Type& outer, const Type& inner, CGRef src, CGRef scev) {
		if (outer.IsRuleGenerator()) {
			assert(&outer.data.RGen->ruleSet != this && "Illegal type rule set recursion");
			// break ruleset encapsulation because outer type will be fixed on a successful sequence specialization
			return _GetArgumentBundle(outer.data.RGen->templateType, inner, src, scev);
		}

		if (inner.IsInvariant() && outer.IsInvariant()) {
			int delta(0);
			if (InvariantSCEV(outer, inner, scev, src, delta)) {
				if (delta == 0) return make_pair(true, nullptr);
				CGRef N(GenericArgument::New());
				if (delta != 1)
					N = Util::Mul(N, Invariant::Constant::New(delta));
				return make_pair(true, GenericSCEVForceInvariant::New(Util::Add(Invariant::Constant::New(outer), N)));
			}
		} 

		GenericRest *gr(nullptr); GenericPair *gp(nullptr);		
		if (scev->Cast(gr) || scev->Cast(gp)) {
			int delta(0);
			if (TupleSCEV(outer, inner, scev, src, delta) && delta != 0) {
				CGRef N(GenericArgument::New());
				if (delta != 1)
					N = Util::Mul(N, Invariant::Constant::New(delta));

				auto e = inner.First();
				auto maxCount = outer.CountLeadingElements(e);
				return make_pair(true,
					ReplicateFirst::New(
						Util::Add(Invariant::Constant::New(Type::InvariantU64(maxCount)), N),
						e, Invariant::Constant::New(outer.Rest(maxCount)), delta));
			}
			if (gp) {
				auto fst(_GetArgumentBundle(outer.First(), inner.First(), GenericFirst::New(src), GenericFirst::New(scev)));
				auto rst(_GetArgumentBundle(outer.Rest(), inner.Rest(), GenericRest::New(src), GenericRest::New(scev)));

				if (fst.first && rst.first) {
					if (fst.second || rst.second) return make_pair(true,
						GenericPair::New(
							Finalize(fst.second, outer.First()),
							Finalize(rst.second, outer.Rest())));
					else return make_pair(true, (Generic*)0);
				}
				else return make_pair(false, (Generic*)0);
			}
		}

		GenericMake *mk;
		if (scev->Cast(mk)) {
			if (inner.IsUserType() && outer.IsUserType() &&
				inner.GetDescriptor() == outer.GetDescriptor()) {
				auto content = _GetArgumentBundle(outer.UnwrapUserType(), inner.UnwrapUserType(), src, mk->GetUp(1));
				if (content.second) {
					return std::make_pair(true, GenericMake::New(scev->GetUp(0), (CGRef)content.second));
				} else return content;
			}
		}

		return make_pair(inner.OrdinalCompare(outer, false) == 0, nullptr);
	}


	class RecursionReasoning
	{
	public:
		typedef RecursionReasoning _Myt;
		long double unknown;
		bool solved;
		SpecializationDiagnostic d;
		SpecializationTransform spec;
		RecursionReasoning(CGRef root,SpecializationTransform::Mode m):unknown(0),solved(false),spec(root,Type(false),d,m),d(0) {}

		void VisitArgument(const GenericArgument *arg, const Type& down)
		{
			unknown = down.GetInvariant();
			solved = true;
		}

		void Visit(CGRef node, const Type& down)
		{
			const GenericArgument *arg;
			if (node->Cast(arg)) { 
				VisitArgument(arg, down);
				return;
			}
			IInversible *inv;
			if (node->Cast(inv))
			{
				for(unsigned i(0);i<node->GetNumCons();++i)
				{
					Type up;
					if (inv->InverseFunction(i,down,up,spec)) Visit(node->GetUp(i),up);
				}
			}
		}
	};

	template <typename RULE, typename CHECK> static std::int64_t Solve(const RULE& r, const CHECK& didItPass, std::int64_t high_bound) {
		std::int64_t low_bound = 0;

		bool didItPassZero = didItPass(r, SpecializationTransform::Infer(r.first, Type::InvariantI64(0)));

		if (high_bound < 0) {
			high_bound = 65536;
			while (high_bound <= std::numeric_limits<int>::max() / 2) {
				auto result = SpecializationTransform::Infer(r.first, Type::InvariantI64(high_bound));
				if (didItPass(r, result) != didItPassZero) {
					high_bound++;
					break;
				}
				low_bound = high_bound;
				high_bound *= 2;
			}
		}

		while (high_bound > low_bound + 1) {
			std::int64_t try_count = (high_bound + low_bound + 1) / 2;
			auto result = SpecializationTransform::Infer(r.first, Type::InvariantI64(try_count));
			if (didItPass(r, result) == didItPassZero) {
				low_bound = try_count;
			} else {
				high_bound = try_count;
			}
		}
		return low_bound;
	}

	std::int64_t TypeRuleSet::SolveForRule(const NumericalRule& nr, std::int64_t maximum) {
		//std::cout << "[Solve] " << *nr.first << nr.second << "\n";
		return Solve(nr, [](const NumericalRule& r, const Type& result) {
			bool pass_minimum = true, pass_maximum = true;
			if (r.second.GetMinimum( ).IsNil( ) == false) {
				if (r.second.closed_min)
					pass_minimum = result >= r.second.GetMinimum( );
				else
					pass_minimum = result > r.second.GetMinimum( );
			}

			if (r.second.GetMaximum( ).IsNil( ) == false) {
				if (r.second.closed_max)
					pass_maximum = result <= r.second.GetMinimum( );
				else
					pass_maximum = result < r.second.GetMinimum( );
			}

			return pass_maximum && pass_minimum;
		}, maximum);
	}

	std::int64_t TypeRuleSet::SolveForRule(const AxiomRule& ar, std::int64_t maximum) {
		//std::cout << "[Solve] " << *ar.first << " |- " << (ar.invert ? "NOT " : "") << ar.match << "\n";
		return Solve(ar, [](const AxiomRule& ar, const Type& result) {
			return ar.invert != ( ar.match == result );
		}, maximum);
	}

	int64_t TypeRuleSet::SolveRecursionDepth(CGRef argBundle) {
		RegionAllocator RecursionSolver;

		struct UserTypeRemover : public CachedTransform<const Generic,CGRef> {
			UserTypeRemover(CGRef root):CachedTransform(root) { }
			CGRef operate(CGRef n) {
				if (IsOfExactType<GenericMake>(n)) return operate(n->GetUp(1));
				else return n->IdentityTransform(*this);
			}
		} RemoveMakes(argBundle);

		argBundle = RemoveMakes.Go();

		struct RuleFormingCopy : public CachedTransform<const Generic,CGRef> {
			CGRef argBundle;
			RuleFormingCopy(CGRef root, CGRef a):argBundle(a),CachedTransform(root) { }

			CGRef operate(CGRef n) {
				if (IsOfExactType<GenericArgument>(n)) return argBundle;
				else return n->IdentityTransform(*this);
			}
		} RuleProcessor(argBundle,argBundle);

		auto variableRangeRules = From(RangeRules)
			.Select([&](const NumericalRule& rule) {
				auto out = NumericalRule(RuleProcessor(rule.first),rule.second);
				return out;
			})
			.Where([](const NumericalRule& rule) {
				return FromGraph(rule.first).OfType<GenericArgument>().Any();
			});

		auto variableFixedRules = From(FixedRules)
			.Select([&](const AxiomRule& rule) {
			return AxiomRule{ RuleProcessor(rule.first), rule.match, rule.invert };
			})
			.Where([](const AxiomRule& rule) {
				return FromGraph(rule.first).OfType<GenericArgument>().Any();
			});

		std::int64_t upper_bound(-1);
		for(auto vr : From(variableRangeRules)) {
			upper_bound = SolveForRule(vr, upper_bound);
			//std::cout << "[GOOD FOR " << upper_bound << "]\n";
			if (upper_bound == 0) return upper_bound;
		}

		for(auto fr : From(variableFixedRules)) {
			upper_bound = SolveForRule(fr, upper_bound);
			//std::cout << "[GOOD FOR " << upper_bound << "]\n";
			if (upper_bound == 0) return upper_bound;
		}
		if (upper_bound == -1) return std::numeric_limits<std::int64_t>::max();
		else return upper_bound;
	}
};
