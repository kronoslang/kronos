#include "TypeAlgebra.h"
#include "TLS.h"
#include "Errors.h"
#include "Invariant.h"
#include "Evaluate.h"
#include "LibraryRef.h"
#include "Native.h"

#include <sstream>

namespace K3 {
	namespace Nodes{

		/* typetag */
		GenericTypeTag::GenericTypeTag(const std::string& fullPath):typeDesc(TLS::GetCurrentInstance()->GetTypeDescriptor(fullPath)) { }
		GenericTypeTag::GenericTypeTag(TypeDescriptor* desc):typeDesc(desc) { }

		Specialization GenericTypeTag::Specialize(SpecializationState &t) const {
			return Specialization(Typed::Nil(),Type(typeDesc));
		}

		/* pair */
		GenericPair::GenericPair(CGRef lhs, CGRef rhs):GenericBinary(lhs,rhs) {}
		GenericPair* GenericPair::New(CGRef lhs, CGRef rhs) {
			return new GenericPair(lhs,rhs);
		}

		bool GenericPair::InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& forwardTransform) const {
			switch(forBranch) {
				case 0:	out = down.First();break;
				case 1: out = down.Rest();break;
				default:assert(0 && " Bad inverse function");
			}
			return true;
		}

		void GenericPair::Output(std::ostream& stream) const {
			stream << "Pair";
			return;
		}


		Specialization GenericPair::Specialize(SpecializationState &t) const {
			SPECIALIZE(t,a,GetUp(0));
			SPECIALIZE(t,b,GetUp(1));

			if (a.node == b.node && Typed::IsNil(a.node)) {
				return Specialization(Typed::Nil(), Type::Pair(a.result, b.result));
			} else {
				return Specialization(Pair::New(a.node, b.node), Type::Pair(a.result, b.result));
			}
		}

		/* First and Rest ctors reduce immediately connected pair algebra nodes */
		GenericFirst::GenericFirst(CGRef up):GenericUnary(up) { }
		CGRef GenericFirst::New(CGRef up) {
			GenericPair *p;
			Invariant::ReplicateFirst *rf;
			if (up->Cast(p)) return p->GetUp(0);
			else if (up->Cast(rf)) return rf->GetFirst();
			else return new GenericFirst(up);
		}

		void GenericFirst::Output(std::ostream& stream) const {
			int skip(1);
			CGRef up(GetUp(0));
			while (IsOfExactType<GenericRest>(up)) {
				skip++;
				up = up->GetUp(0);
			}
			switch (skip % 100) {
			case 11:
			case 12:
			case 13: stream << skip << "th"; break;
			default:
				switch (skip % 10) {
					case 1: stream << skip << "st"; break;
					case 2: stream << skip << "nd"; break;
					case 3: stream << skip << "rd"; break;
					default: stream << skip << "th"; break;
				}
			}
			return;
		}

		bool GenericFirst::InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& t) const {
			auto fwd(t(GetUp(0)));
			if (fwd.node==0) return false;
			out = Type::Pair(down,fwd.result.Rest());
			return true;
		}

		Specialization GenericFirst::Specialize(SpecializationState &t) const {
			SPECIALIZE(t,a,GetUp(0));
			if (a.result.IsPair()) {
				if (Typed::IsNil(a.node)) return Specialization(Typed::Nil(),Type::First(a.result));
				else return Specialization(First::New(a.node),Type::First(a.result));
			} else if (a.result.IsUserType()) {
				auto first = TLS::ResolveSymbol(":Fallback:First");
				if (first) {
					/* offer polymorphic First/Rest for user types */
					Evaluate *ext = Evaluate::New("Fallback:First",first,GetUp(0));
					Specialization fbFirst = ext->Specialize(t);
					return fbFirst;
				}
			} 
			t.GetRep().Diagnostic(LogTrace,this,Error::TypeMismatchInSpecialization,a.result,"Not a sequence");
			return SpecializationFailure();
		}

		GenericRest::GenericRest(CGRef up):GenericUnary(up) { }

		CGRef GenericRest::New(CGRef up) {
			GenericPair *p;
			Invariant::ReplicateFirst *rf;
			if (up->Cast(p)) return p->GetUp(1);
			else if ((rf = ShallowCast<Invariant::ReplicateFirst>(up))) {
				return Invariant::ReplicateFirst::New(
					Invariant::Add(rf->GetUp(0),Invariant::Constant::New(Type::InvariantI64(-1))),
					rf->GetElement(),
					rf->GetUp(1),
					rf->GetRecurrenceDelta());
			} else return new GenericRest(up);
		}

		void GenericRest::Output(std::ostream& stream) const {
			stream << "Rest";
		}

		Specialization GenericRest::Specialize(SpecializationState &t) const {
			SPECIALIZE(t,a,GetUp(0));
			if (a.result.IsPair()) {
				if (Typed::IsNil(a.node)) return Specialization(Typed::Nil(),Type::Rest(a.result));
				else return Specialization(Rest::New(a.node),Type::Rest(a.result));
			} else if (a.result.IsUserType()) {
				auto rest = TLS::ResolveSymbol(":Fallback:Rest");
				if (rest) {
					/* offer polymorphic First/Rest for user types */
					Evaluate *ext = Evaluate::New("Fallback:Rest",rest,GetUp(0));
					Specialization fbRest = ext->Specialize(t);
					return fbRest;
				}
			} 
			t.GetRep().Diagnostic(LogTrace,this,Error::TypeMismatchInSpecialization,a.result,"Not a sequence");
			return SpecializationFailure();
		}

		bool GenericRest::InverseFunction(int forBranch, const Type& down, Type& out, SpecializationTransform& t) const {
			auto fwd(t(GetUp(0)));
			if (fwd.node==0) return false;
			out = Type::Pair(fwd.result.First(),down);
			return true;
		}

		First::First(CTRef up):TypedUnary(up) { }

		CTRef First::New(CTRef up) {
			return up->GraphFirst();
		}

		CTRef First::PairWithRest(CTRef up) const {
			Rest *r;
			if (up->Cast(r) && GetUp(0) == r->GetUp(0)) {
				return GetUp(0);
			}
			return MakeFullPair(this, up);
		}

		CTRef Typed::GraphFirst() const {
			return new First(this);
		}

		Type First::Result(ResultTypeTransform& withArgument) const {
			return Type::First(GetUp(0)->Result(withArgument));
		}

		Type Rest::Result(ResultTypeTransform& withArgument) const {
			return Type::Rest(GetUp(0)->Result(withArgument));
		}

		Rest::Rest(CTRef up):TypedUnary(up) {}

		CTRef Rest::PairWithFirst(CTRef fst) const {
			First *f;
			if (fst->Cast(f)) {
				if (f->GetUp(0) == GetUp(0)) return GetUp(0);
			}
			return MakeFullPair(fst, this);
		}

		CTRef Rest::New(CTRef up) {
			return up->GraphRest();
		}

		CTRef Typed::GraphRest() const {
			return new Rest(this);
		}

		CTRef PairSimplify::MakeFullPair(CTRef fst, CTRef rst) {
			return new Pair(fst,rst);
		}

		Pair::Pair(CTRef f, CTRef r):TypedBinary(f,r) { }

		CTRef Pair::New(CTRef fst, CTRef rst) {
			IPairSimplifyFirst *simplifyFirst;
			if (fst->Cast(simplifyFirst)) {
				return simplifyFirst->PairWithRest(rst);
			}

			IPairSimplifyRest *simplifyRest;
			if (rst->Cast(simplifyRest)) {
				return simplifyRest->PairWithFirst(fst);
			}

			Native::Constant *ac, *bc;
			if (fst->Cast(ac) && rst->Cast(bc)) {
				std::vector<char> concat(ac->FixedResult().GetSize() + bc->FixedResult().GetSize());
				memcpy(concat.data(), ac->GetPointer(), ac->FixedResult().GetSize());
				memcpy(concat.data() + ac->FixedResult().GetSize(), bc->GetPointer(), bc->FixedResult().GetSize());
				return Native::Constant::New(Type::Pair(ac->FixedResult(), bc->FixedResult()), concat.data());
			}

			return new Pair(fst,rst);
		}

		CTRef Pair::GraphFirst() const {
			return GetUp(0);
		}

		CTRef Pair::GraphRest() const {
			return GetUp(1);
		}

		CTRef Pair::IdentityTransform(GraphTransform<const Typed, CTRef>& copyTable) const {
			return New(copyTable(GetUp(0)), copyTable(GetUp(1)));
		}

		Type Pair::Result(ResultTypeTransform& withArgument) const {
			return Type::Pair(GetUp(0)->Result(withArgument),GetUp(1)->Result(withArgument));
		}

		CTRef Typed::Nil() {
			return Native::ConstantNil::Get();
		}


		/* make and break */

		Specialization GenericMake::Specialize(SpecializationState &t) const {
			SPECIALIZE_ARGS(t,0,1);
			if (A0.result.IsTypeTag() && (A0.result.GetDescriptor()->IsBreakable() || allowInternalType)) {
				return Specialization(A1.node,Type::User(A0.result.GetDescriptor(),A1.result));
			} else {
				t.GetRep().Diagnostic(LogErrors,this,Error::TypeMismatchInSpecialization,A0.result,"Make requires a type tag");
				return t.GetRep().TypeError("Make", A0.result);
			}
		}

		bool GenericMake::InverseFunction(int branch, const Type& down, Type& out, SpecializationTransform& t) const {
			switch(branch)
			{
				case 0:out = Type(down.GetDescriptor());return true;
				case 1:out = down.UnwrapUserType();return true;break;
				default:INTERNAL_ERROR("Bad inverse function");return false;
			}				
		}

		Specialization GenericBreak::Specialize(SpecializationState &t) const {
			SPECIALIZE_ARGS(t,0,1);
			if (A0.result.IsTypeTag()) {
				if (A1.result.IsUserType() && (A0.result.GetDescriptor()->IsBreakable() || allowInternalType)) {
					if (A1.result.GetDescriptor() == A0.result.GetDescriptor()) {
						return Specialization(A1.node,A1.result.UnwrapUserType());
					} 
				} 
				t.GetRep().Diagnostic(LogErrors,this,Error::TypeMismatchInSpecialization, A1.result, A0.result, 
										  "Cannot destructure as a nominal type `%s`", A0.result.GetDescriptor()->GetName().c_str());
				return SpecializationFailure();
			} else {
				t.GetRep().Diagnostic(LogErrors,this,Error::TypeMismatchInSpecialization,A0.result,"Break requires a type tag");
				return t.GetRep().TypeError("Break", A0.result);
			}
		}

		bool GenericBreak::InverseFunction(int branch, const Type& down, Type& up,SpecializationTransform& t) const {
			up = Type::User(down.First().GetDescriptor(),down.Rest());
			return true;
		}
	};
}; 