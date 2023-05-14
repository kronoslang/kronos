#include "FlowControl.h"
#include "Errors.h"
#include "Evaluate.h"
#include "LibraryRef.h"
#include "TypeAlgebra.h"
#include "Native.h"
#include "Conversions.h"
#include "Reactive.h"
#include "Invariant.h"

namespace K3 {
	namespace Nodes{
		Specialization When::Specialize(SpecializationState &spec) const {
			auto& diags(spec.GetRep());
			auto block(diags.Block(LogTrace,"when"));

			for(unsigned int i=0;i+1<GetNumCons();i+=2) {
				SPECIALIZE(spec,truthValue,GetUp(i));
				if (truthValue.result.IsTrue()) {
					diags.Diagnostic(LogTrace,this,0,"branch %i",i+1);
					return spec(GetUp(i+1));
				} else if (truthValue.result.IsNil()) {
					continue;
				} else {
					diags.Diagnostic(LogErrors,this,Error::BadDefinition,"When requires either True or nil for the conditional branch");
					return spec.GetRep().TypeError("When", truthValue.result);
				}
			}
			diags.Diagnostic(LogEverything,this,Error::SpecializationFailed,"No valid branches");
			return SpecializationFailure();
		}

		Specialization Polymorphic::Specialize(SpecializationState& spec) const {
			auto &diags(spec.GetRep());
			auto block(diags.Block(LogTrace, "poly"));
			for (unsigned i = 0;i < GetNumCons();++i) {
				auto branch{ spec(GetUp(i)) };
				if (branch.node) {
					diags.Diagnostic(LogTrace, this, 0, "branch %i", i + 1);
					return branch;
				} else if (branch.result.IsTypeTag() &&
					branch.result.GetDescriptor() == &K3::SpecializationFailure) {
					continue;
				} else {
					return branch;
				}
			}
			diags.Diagnostic(LogErrors, this, Error::SpecializationFailed, "No valid forms");
			return diags.TypeError("Polymorphic", Type::Nil);
		}

		Specialization Raise::Specialize(SpecializationState& spec) const {
			SPECIALIZE(spec,exceptionData,GetUp(0));
			//if (exceptionData.result.GetSize()) {
			//	spec.GetRep().Diagnostic(Verbosity::LogErrors,this,Error::InvalidType,"User exception may not contain runtime data");
			//	return TypeError(nullptr,Type(&FatalFailure));
			//}
			return Specialization(nullptr,Type::User(&UserException,exceptionData.result.Fix()));
		}

		Raise* Raise::NewFatalFailure(const char* msg, CGRef args) {
			auto msgTy = Type::User(&FatalFailure, Type(msg));
			CGRef msgG = Invariant::Constant::New(msgTy);
			if (args) msgG = GenericPair::New(msgG, args);

			return Raise::New(msgG);
		}


		static Type Purge(const Type& t) {
			// replace any sized types with their tags.
			if (t.GetSize()) {
				if (t.IsPair()) {
					auto numLead = t.CountLeadingElements(t.First());
					auto tail = t.Rest(numLead);
					return Type::Chain(Purge(t.First()), numLead, Purge(tail));
				} else if (t.IsUserType()) {
					return Type::User(t.GetDescriptor(), Purge(t.UnwrapUserType()));
				}
				return t.TypeOf();
			} else return t;
		}

		Specialization Handle::Specialize(SpecializationState& spec) const {
			Specialization ptry = spec(GetUp(0));
			if (ptry.node) {
				return ptry;
			} else {
				Type arg = Purge(ptry.result.Fix(Type::GenerateNoRules));

				auto eval = TLS::ResolveSymbol(":Eval");
				auto wrapper = Evaluate::New("handle", eval,
												GenericPair::New(GetUp(1), Invariant::Constant::New(arg)));
				return spec(wrapper);
			} 
		}

		Specialization GenericGetVectorLen::Specialize(SpecializationTransform& t) const {
			SPECIALIZE_ARGS(t,0);

			size_t len = 1;
			bool hasNil = false;
			Type e = A0.result;

			if (A0.result.IsPair()) {
				len = A0.result.CountLeadingElements(A0.result.First());
				Type tail = A0.result.Rest(len);

				if (tail == A0.result.First()) {
					len++;
					hasNil = false;
				} else if (tail.IsNil()) {
					hasNil = true;
				} else {
					t.GetRep().Diagnostic(LogErrors,this,Error::InvalidType,A0.result,"Can't apply Select to a vector with varying element types");
					return TypeError(&FatalFailure);
				}
			}

			return Specialization(Typed::Nil(),Type::InvariantU64(len));
		}

		Specialization GenericSelect::Specialize(SpecializationTransform& t) const {
			SPECIALIZE_ARGS(t,0,1,2);
			assert("Select index: " && A1.result.IsInt32());
			if (A0.result.IsPair()) {
				auto af = A0.result.Fix();
				return Specialization(AtIndex::New(af,A0.node,A1.node),af.First());
			} else {
				return A0;
			}
		}

		Specialization GenericConstantSelect::Specialize(SpecializationTransform& t) const {
			SPECIALIZE_ARGS(t, 0);
			if (A0.result.IsPair()) {
				auto ti = A0.result.Rest();
				auto v = A0.result.First();
				auto vg = A0.node->GraphFirst();
				if (ti.IsInvariant()) {
					auto i = (std::int64_t)ti.GetInvariant();
					auto last = (std::int64_t)v.Arity().GetInvariant();

					if (i >= last) i = 0;
					if (i < 0) i = 0;
					
					for (int skip = 0; skip < i; ++skip) {
						v = v.Rest();
						vg = vg->GraphRest();
					}

					if (i < last - 1 || (v.IsNilTerminated() && v.IsPair())) {
						v = v.First();
						vg = vg->GraphFirst();
					}
                    
                    vg = ReactiveOperators::Impose::New(A0.node, vg);
                    
					return Specialization(vg, v);
				} else {
					t.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization, A0.result, "Select can accept a constant index");
					return SpecializationFailure();
				}
			} else {
				t.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization, A0.result, "Select accepts a homogeneous vector and an index");
				return SpecializationFailure();
			}
		}

		Specialization GenericSlice::Specialize(SpecializationTransform& t) const {
			SPECIALIZE_ARGS(t, 0, 1, 2);
			auto vty = A0.result;
			assert(A1.result.IsInt32());

			Type elTy;
			int staticLen = -1;

			if (vty.IsPair()) {
				auto head = vty.CountLeadingElements(vty.First());
				auto tail = vty.Rest(head);
				if (tail.IsNil() || tail == vty.First()) {
					elTy = vty.First();								
					staticLen = vty.Arity().GetInvariantI64();
				} else {
					t.GetRep().Diagnostic(LogErrors, this, Error::InvalidType, A0.result, "Slice requires a homogeneous tuple or list");
					return TypeError(&FatalFailure, Type("Slice requires a homogeneous tuple or list"));
				}
			} else if (vty.IsArrayView()) {
				elTy = vty.GetArrayViewElement();
			} else {
				t.GetRep().Diagnostic(LogErrors, this, Error::InvalidType, A0.result, "Slice requires a homogeneous tuple or list");
				return TypeError(&FatalFailure, Type("Slice requires a homogeneous tuple or list"));
			}

			auto len = A2.result;
			if (len.IsInvariant()) {
				if (len.GetInvariantI64() > std::numeric_limits<int>::max()) {
					t.GetRep().Diagnostic(LogErrors, this, Error::BadInput, A2.result, "Slice length is too long");
					return TypeError(&FatalFailure, Type::Pair(Type("Slice length is too long:"), len));
				}
				auto sa = SubArray::New((int)len.GetInvariantI64(), staticLen, elTy, A0.node, A1.node);
				return Specialization(sa, sa->FixedResult());
			} else if (len.IsInt32()) {
				auto sa = Slice::New(staticLen, elTy, A0.node, A1.node, A2.node);
				return Specialization(sa, sa->FixedResult());
			} else {
				SPECIALIZE(t, cvtInt, Convert::New(Convert::Int32, GetUp(2)));
				auto sa = Slice::New(staticLen, elTy, A0.node, A1.node, cvtInt.node);
				return Specialization(sa, sa->FixedResult());
			}
		}

		Specialization GenericSliceArity::Specialize(SpecializationState& t) const {
			SPECIALIZE_ARGS(t, 0);
			if (A0.result.IsArrayView()) {
				return { SliceArity::New(A0.node), Type::Int32 };
			} else {
				return SpecializationFailure();
			}
		}

		CTRef AtIndex::IdentityTransform(GraphTransform<const Typed, CTRef>& transform) const {
			auto i = New(vectorTy, transform(GetUp(0)), transform(GetUp(1)));
			if (auto ai = i->Cast<AtIndex>()) {
				ai->SetReactivity(GetReactivity());
			}
			return i;
		}

		CTRef AtIndex::New(Type vectorTy, CTRef vector, CTRef index) {
			Native::Constant *c;
            // constant propagation breaks reactive clock; disabled below
			if (false && index->Cast(c)) {
				assert(c->FixedResult().IsInt32());
				int i = *(std::int32_t*)c->GetPointer();
				auto svTy = vectorTy;
                                
				while (i--) {
					vector = vector->GraphRest();
					svTy = svTy.Rest();
				}
                                
				// handle possible nil termination
				if (svTy == vectorTy.First()) return vector;
				else {
					assert(svTy.First() == vectorTy.First());
					return vector->GraphFirst();
				}
			}
			// count elements to select from
			int count = vectorTy.CountLeadingElements(vectorTy.First());
			if (vectorTy.Rest(count) == vectorTy.First()) ++count;

			return new AtIndex(vectorTy, vectorTy.First(), count, vector, index);
//			return new AtIndex(elementType, order, vector, index);
		}

		Specialization GenericTernaryAtom::Specialize(SpecializationTransform& spec) const {
			SPECIALIZE_ARGS(spec,0,1,2);
			if (A1.result == A2.result &&
				A0.result.IsNativeType()) {
					if (A0.result.IsNativeVector() || A1.result.IsNativeVector()) {
						if (A0.result.GetVectorWidth() != A1.result.GetVectorWidth()) {
							spec.GetRep().Diagnostic(LogWarnings,this,Error::TypeMismatchInSpecialization,"Ternary selection predicate is a vector of %i elements, but %i are required",A0.result.GetVectorWidth(),A1.result.GetVectorWidth());
							return SpecializationFailure();
						}
					}
					return Specialization(Native::Select::New(A0.node,A1.node,A2.node),A1.result);

			} else {
				spec.GetRep().Diagnostic(LogTrace,this,Error::TypeMismatchInSpecialization,A2.result,A1.result,"Ternary selection operands must be of the same type");
				return SpecializationFailure();
			}
		}

		CGRef Modulo(CGRef dividend, CGRef divisor) {
			return Evaluate::CallLib(":Modulo", GenericPair::New(dividend,divisor));
		}

		Specialization GenericCond::Specialize(SpecializationTransform& t) const {
			SPECIALIZE(t, a, GetUp(0));
			auto conds = a;
			std::vector<Specialization> condition;
			std::vector<Specialization> branch;
			while (conds.result.IsPair()) {
				if (conds.result.Rest().IsPair()) {
					condition.emplace_back(conds.node->GraphFirst(), conds.result.First());
					conds = Specialization(conds.node->GraphRest(), conds.result.Rest());
					branch.emplace_back(conds.node->GraphFirst(), conds.result.First());
					conds = Specialization(conds.node->GraphRest(), conds.result.Rest());
				} else {
					if (conds.result.Rest().IsNil() == false) {
						t.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::BadDefinition, "Conditionals must be in a nil-terminated list.");
						return t.GetRep().TypeError("Cond", conds.result.Rest());
					}
					branch.emplace_back(conds.node->GraphFirst(), conds.result.First());
					break;
				}
			}

			if (branch.empty()) {
				t.GetRep().Diagnostic(Verbosity::LogWarnings, this, Error::Warning, "Conditional with a single branch");
				return conds;
			}

			assert(condition.size() <= std::numeric_limits<int>::max());

			CTRef sw = Native::Constant::New((int)condition.size());
			for (int i((int)condition.size() - 1); i >= 0; --i) {
				if (condition[i].result.IsNativeType() &&
					condition[i].result.IsNativeVector() == false) {
					sw = Native::Select::New(condition[i].node, Native::Constant::New(std::int32_t(i)), sw);
				} else {
					t.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::TypeMismatchInSpecialization, "Truth value must be a native scalar type.");
					return t.GetRep().TypeError("Cond", condition[i].result);
				}
			}

			// build call wrappers
			auto eval = TLS::ResolveSymbol(":Eval");
			auto arg = GenericArgument::New();
			auto wrapper = Evaluate::New("dispatch", eval, arg);

			for (auto &b : branch) {
				SpecializationTransform subTransform(wrapper, Type::Pair(b.result, Type::Nil), t.GetRep(), t.mode);
				auto subBody = subTransform.Go();
				if (subBody.node == nullptr) return subBody;
				b.node = FunctionCall::New("branch", subBody.node, b.result, subBody.result, b.node);
				b.result = subBody.result;
			}

			Type result;
			if (branch.size() == 1) result = branch[0].result;
			result = Type::Union(branch[0].result, branch[1].result, false);
			for (size_t i(2);i < branch.size();++i) {
				result = Type::Union(result, branch[i].result, true);
			}

			return Specialization(Switch::New(sw, branch, result), result);
		}

		unsigned Switch::ComputeLocalHash() const {
			size_t h(TypedPolyadic::ComputeLocalHash());
			HASHER(h, result.GetHash());
			HASHER(h, branchResultSubtypeIndex.size());
			for (auto bsti : branchResultSubtypeIndex) HASHER(h, bsti);
			return (unsigned)h;
		}

		int Switch::LocalCompare(const ImmutableNode& rhs) const {
			auto tmp = TypedPolyadic::LocalCompare(rhs);
			if (tmp) return tmp;
			const Switch* rs = (const Switch*)&rhs;
			tmp = ordinalCmp(branchResultSubtypeIndex.size(), rs->branchResultSubtypeIndex.size());
			if (tmp) return tmp;
			for (size_t i(0); i < branchResultSubtypeIndex.size(); ++i) {
				if (branchResultSubtypeIndex[i] > rs->branchResultSubtypeIndex[i]) return 1;
				if (branchResultSubtypeIndex[i] < rs->branchResultSubtypeIndex[i]) return -1;
			}
			return 0;
		}

		Switch* Switch::New(CTRef arg, const std::vector<Specialization>& branches, const Type& r) {
			auto sw = new Switch(arg, r);
			sw->branchResultSubtypeIndex.resize(branches.size() + 1);
			if (r.IsUserType(UnionTag)) {
				int n = r.UnwrapUserType().First().GetNumUnionTypes();
				auto u = r.UnwrapUserType().First();

				for (size_t i(0); i <branches.size(); ++i) {
					for (int j(0); j < n; ++j) {
						if (u.GetUnionType(j) == branches[i].result) {
							sw->branchResultSubtypeIndex[sw->GetNumCons()] = j;
							sw->Connect(branches[i].node);
						}
					}
				}
			} else {
				for (size_t i(0); i < branches.size(); ++i) {
					sw->branchResultSubtypeIndex[sw->GetNumCons()] = -1;
					sw->Connect(branches[i].node);
				}
			}
			return sw;
		}

		Specialization GenericUnionSubtype::Specialize(SpecializationTransform& t) const {
			SPECIALIZE(t, a, GetUp(0));
			Type af = a.result.Fix();
			if (af.IsUnion()) {
				assert(IsOfExactType<First>(a.node)); // point directly to the unsafe union
				return Specialization(a.node->GetUp(0), af.GetUnionType(sti));
			} else {
				return t.GetRep().TypeError("Union-Subtype", af);
			}
		}

		Specialization GenericDispatch::Specialize(SpecializationTransform& t) const {
			SPECIALIZE(t, a, GetUp(0));
			a.result = a.result.Fix();
			if (a.result.IsPair()) {
				Specialization fn(a.node->GraphFirst(), a.result.First());
				Specialization arg(a.node->GraphRest(), a.result.Rest());

				if (arg.result.IsUserType() && arg.result.GetDescriptor() == &UnionTag) {
					auto unionType = arg.result.UnwrapUserType().First();
					auto tag = arg.node->GraphRest();
					auto unsafeUnion = arg.node->GraphFirst();

					auto eval = TLS::ResolveSymbol(":Eval");
					std::vector<Specialization> branches;
					Type result;

					auto passArg = GenericArgument::New();

					for (int i(0);i < unionType.GetNumUnionTypes();++i) {
						auto subBody = Evaluate::New("dispatch", eval, passArg);
						auto subArgTy = Type::Pair(fn.result, unionType.GetUnionType(i));

						SpecializationTransform subTransform(subBody, subArgTy ,t.GetRep(), t.mode);
						auto branch = subTransform.Go();
						if (!branch.node) return branch;
						if (i) result = Type::Union(result, branch.result, true);
						else result = branch.result;

						branches.emplace_back(FunctionCall::New("dispatch", branch.node, subArgTy, branch.result,
																Pair::New(fn.node, UnsafePointerCast::New(unionType.GetUnionType(i), unsafeUnion))),
											  branch.result);
					}
					return Specialization(Switch::New(tag, branches, result), result);
				} 
			}
			t.GetRep().Diagnostic(LogTrace, this, Error::TypeMismatchInSpecialization, a.result, "Dispatch requires a function and a union argument");
			return SpecializationFailure();
		}
	};

	

	void BuildSelectPrimitiveOps(Parser::RepositoryBuilder pack) {
		using namespace Nodes;
		/* Select primitives */
		CGRef Arg(GenericArgument::New());
		CGRef vec(GenericFirst::New(Arg)),idx(Convert::New(Convert::Int32,GenericRest::New(Arg)));
		CGRef lenc(GenericGetVectorLen::New(vec));
		CGRef maxi(Convert::New(Convert::Int32,Invariant::Sub(lenc,Invariant::Constant::New(Type::InvariantI64(1)))));
		CGRef zero(Convert::New(Convert::Int32,Invariant::Constant::New(Type::InvariantI64(0))));

		pack.AddFunction("Ternary-Select",GenericTernaryAtom::New(GenericFirst::New(Arg),GenericFirst::New(GenericRest::New(Arg)),GenericRest::New(GenericRest::New(Arg))),"cond t f","Selects a native atom of similar type; if 'cond' is nonzero, 't' is selected, otherwise 'f' is selected.");

		pack.AddFunction("Select", 
			 GenericSelect::New(vec,
				Evaluate::CallLib(":Max",
					GenericPair::New(Evaluate::CallLib(":Min",
					GenericPair::New(maxi, idx)), zero)), lenc),
			"vector index", "Selects an element from the homogeneous vector, clamping the index to the bounds");

		pack.AddFunction("Slice", GenericSlice::New(
			GenericFirst::New(Arg),
			Convert::New(Convert::Int32, GenericFirst::New(GenericRest::New(Arg))),
			GenericRest::New(GenericRest::New(Arg))
		));

		pack.AddFunction("Slice-Length", GenericSliceArity::New(Arg));

		pack.AddFunction("Select", GenericConstantSelect::New(Arg,false), "vector index", 
						 "Selects an element from the vector, clamping index to the bounds. If 'index' is an invariant constant, the contents of 'vector' do not need to be homogeneous.");

		pack.AddFunction("Cond", GenericCond::New(Arg), "clauses",
						 "'clauses' is a list of conditionals and branches, followed by the 'else' branch. The first branch whose condition is true will be evaluated.");
		pack.AddFunction("Dispatch", GenericDispatch::New(Arg), 
						 "func arg", "evaluates 'func' with argument 'arg', which is a union type. 'func' is runtime polymorphic with regard to 'arg'.");

		pack.AddFunction("Select-Wrap",GenericSelect::New(vec,
			Modulo(idx,lenc),lenc),"vector index",
						 "Selects an element from the table according to index, using modulo addressing to wrap around the index");
	}
};
