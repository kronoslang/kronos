#pragma once

#include "NodeBases.h"
#include "RepositoryBuilder.h"
#include <functional>

namespace K3 {
	namespace Nodes{

		GENERIC_NODE(GenericSpecializationCallback, GenericBinary)
			GenericSpecializationCallback(CGRef key, CGRef val) : GenericBinary(key, val) {}
		PUBLIC
			static GenericSpecializationCallback* New(CGRef key, CGRef val) { return new GenericSpecializationCallback(key, val); }
		END
			
		template <typename REQUIRE> bool Check(const Type& t);
		namespace Invariant{
			GENERIC_NODE(Constant, DisposableGenericLeaf)
				std::vector<char> memory;
				Type VAL;
				bool isFunctionBodyRoot = false;
				Constant(double v) :VAL(v) {}
				Constant(std::int64_t v) :VAL(v) {}
				Constant(Nodes::CGRef quotedGraph, bool isBodyRoot):VAL(quotedGraph), isFunctionBodyRoot(isBodyRoot) { }
				Constant(const SString *str):VAL(str){}
				Constant(Type t, const void* data = nullptr):VAL(t.Fix(Type::GenerateRulesForAllTypes)) {
					if (data) {
						memory.resize(t.GetSize());
						memcpy(memory.data(), data, memory.size());
					} 
				}
				int LocalCompare(const ImmutableNode& rhs) const override;
				unsigned ComputeLocalHash() const override;
			public:
				void Output(std::ostream&) const  override;
				Constant(bool truth):VAL(truth){}
				static Constant* New(double v) {return new Constant(v);}
				static Constant* New(std::int64_t v) { return new Constant(v); }
				static Constant* NewBigNumber(const char* str) { return new Constant(Type::BigNumber(str)); }
				static Constant* New(int v) { return new Constant((std::int64_t)v); }
				static Constant* New(Nodes::CGRef quotedGraph, bool isAnonymousFunction) {return new Constant(quotedGraph,isAnonymousFunction);}
				static Constant* New(bool truthValue) {return new Constant(truthValue);}
				static Constant* New(const SString *str) {return new Constant(str);}
				static Constant* New(Type t, const void *data = nullptr) {return new Constant(t, data);}
				static Constant* New(const Graph<Generic>& cg) {return new Constant(Type::InvariantGraph(cg));}
				Type GetType() const {return VAL;}
				bool IsFunctionBodyRoot() const { return isFunctionBodyRoot; }
			END

			GENERIC_NODE(Arity, GenericUnary, IInversible)
				Arity(CGRef up):GenericUnary(up) {}
			public:
				static CGRef New(CGRef up);
				bool InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& forwardTransform) const override;
				CGRef IdentityTransform(GraphTransform<const Generic,CGRef>& st) const override {return New(st(GetUp(0)));}
			END

			GENERIC_NODE(GenericClassOf,GenericUnary)
				GenericClassOf(CGRef cls):GenericUnary(cls) {}
			PUBLIC
				static GenericClassOf* New(CGRef cls) { return new GenericClassOf(cls); }
			END

			GENERIC_NODE(GenericEqualType,GenericBinary)
				GenericEqualType(CGRef a, CGRef b):GenericBinary(a,b) {}
			PUBLIC
				static GenericEqualType* New(CGRef a, CGRef b) { return new GenericEqualType(a,b); }
			END

			GENERIC_NODE(GenericOrdinalCompareType, GenericBinary)
				int expectOrdinal;
				GenericOrdinalCompareType(int expect, CGRef a, CGRef b) : GenericBinary(a, b), expectOrdinal(expect) {}
			PUBLIC 
				static GenericOrdinalCompareType* New(int expectOrdinal, CGRef a, CGRef b) { return new GenericOrdinalCompareType(expectOrdinal, a, b); }
			END

			GENERIC_NODE(ReplicateFirst,DisposableGenericBinary,IInversible)
				ReplicateFirst(CGRef count, const Type& element, CGRef chain, int delta):DisposableGenericBinary(count,chain),element(element.Fix()),recurrenceDelta(delta) {}
				Type element;
				int recurrenceDelta;
			public:
				DEFAULT_LOCAL_COMPARE(DisposableGenericBinary,element,recurrenceDelta)
				const Type& GetElement() const {return element;}
				static CGRef New(CGRef count, const Type& element, CGRef chain, int delta);
				bool InverseFunction(int branch,const Type& down,Type& up, SpecializationTransform& t) const override;
				virtual CGRef IdentityTransform(GraphTransform<const Generic,CGRef>& copyTable) const override {return New(copyTable(GetUp(0)),element,copyTable(GetUp(1)),recurrenceDelta);}
				virtual void Output(std::ostream& stream) const override;
				CGRef GetFirst() const;
				int GetRecurrenceDelta() const { return recurrenceDelta; }
			END

			GENERIC_NODE(CountLeadingElements,DisposableGenericUnary)
				Type element;
				CountLeadingElements(const Type& element, CGRef data):DisposableGenericUnary(data),element(element.Fix()){}
			public:
				const Type& GetElement() const {return element;}
				static CGRef New(const Type& ofType, CGRef data);
				bool InverseFunction(int branch,const Type& down, Type& up, SpecializationTransform& t) const;
				CGRef IdentityTransform(GraphTransform<const Generic,CGRef>& copyTable) const override {return New(element,copyTable(GetUp(0)));}
				void Output(std::ostream& stream) const override;
			END

			GENERIC_NODE(GenericRequire,GenericBinary)
				GenericRequire(CGRef a, CGRef b):GenericBinary(a,b) {}
			public:
				static GenericRequire* New(CGRef what, CGRef pass) { return new GenericRequire(what,pass); }
			END

			GENERIC_NODE(GenericTrace,GenericBinary)
				GenericTrace(CGRef a, CGRef b):GenericBinary(a,b) { }
			public:
				static GenericTrace* New(CGRef a, CGRef b) { return new GenericTrace(a,b); }
			END

			GENERIC_NODE(ExplainConstraint, GenericTernary)
				ExplainConstraint(CGRef graph, CGRef e, CGRef recv) :GenericTernary(graph, e, recv) { }
			PUBLIC
				static ExplainConstraint* New(CGRef graph, const std::string& e, CGRef r) { return new ExplainConstraint(graph, Constant::New(Type(e.c_str())), r); }
				static ExplainConstraint* New(CGRef graph, CGRef explain, CGRef r) { return new ExplainConstraint(graph, explain, r); }
			END
				
			GENERIC_NODE(GenericNoFallback,GenericLeaf)
			public:
				static GenericNoFallback* New() { return new GenericNoFallback(); }
			END

			GENERIC_NODE(GenericPropagateFailure, GenericLeaf)
			public:
				static GenericPropagateFailure* New() { return new GenericPropagateFailure(); }
			END

			GENERIC_NODE(GenericSCEVAdd, GenericUnary)
				int delta;
				GenericSCEVAdd(CGRef a, int delta) :delta(delta), GenericUnary(a) {}
			public:
				DEFAULT_LOCAL_COMPARE(GenericUnary, delta);
				static CGRef New(CGRef a, int delta) { 
					GenericSCEVAdd *sa;
					if (a->Cast(sa)) {
						return new GenericSCEVAdd(sa->GetUp(0), delta + sa->delta);
					} else return new GenericSCEVAdd(a, delta); 
				}
				int GetDelta() const { return delta; }
			END

			GENERIC_NODE(GenericSCEVForceInvariant, GenericUnary)
				GenericSCEVForceInvariant(CGRef up) :GenericUnary(up) {}
			public:
				static CGRef New(CGRef up) { return new GenericSCEVForceInvariant(up); }
			END


			CGRef Add(CGRef a, CGRef b);
			CGRef Sub(CGRef a, CGRef b);
			CGRef Mul(CGRef a, CGRef b);
			CGRef Div(CGRef a, CGRef b);

			CGRef CmpEq(CGRef a, CGRef b);
			CGRef CmpGt(CGRef a, CGRef b);
			CGRef CmpLt(CGRef a, CGRef b);
			CGRef CmpGe(CGRef a, CGRef b);
			CGRef CmpLe(CGRef a, CGRef b);

			CGRef Not(CGRef a);

			using ivar = ttmath::Big<1, 2>;

			CGRef Custom(ivar(*)(ivar, ivar), CGRef a, CGRef b); 
			CGRef Custom(bool(*)(ivar, ivar), CGRef a, CGRef b); 
			CGRef Custom(ivar(*)(ivar), CGRef a);
		};

		namespace Util
		{
			CGRef Add(CGRef a, CGRef b);
			CGRef Sub(CGRef a, CGRef b);
			CGRef Mul(CGRef a, CGRef b);
			CGRef Div(CGRef a, CGRef b);
		};
	};

	void BuildInvariantPrimitiveOps(Parser::RepositoryBuilder& pack);
};