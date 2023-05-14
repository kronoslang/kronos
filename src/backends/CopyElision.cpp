#include "SideEffectCompiler.h"
#include "NodeBases.h"
#include "CompilerNodes.h"
#include "TypeAlgebra.h"
#include "Evaluate.h"
//#include "MultiDispatch.h"
#include "Reactive.h"
#include "Native.h"
#include "DynamicVariables.h"
#include "CompilerNodes.h"
#include "FlowControl.h"

namespace K3 {
	namespace Backends{

		static CTRef MergeSideEffects(CTRef oldsfx,CTRef newsfx)
		{
			if (IsOfExactType<Pair>(oldsfx))
			{
				return Pair::New(
					MergeSideEffects(oldsfx->GetUp(0), First::New(newsfx)),
					MergeSideEffects(oldsfx->GetUp(1), Rest::New(newsfx)));
			}
			else if (Typed::IsNil(oldsfx)) return newsfx;
			else if (Typed::IsNil(newsfx)) return oldsfx;
			else return newsfx;				
		}

		void CopyElisionTransform::operate(CTRef t)
		{
			t->CopyElision(*this);
		}

		CTRef& CopyElisionTransform::operator[](CTRef function)
		{
			auto f(sfx.find(function));
			if (f) return f->second;
			else
			{
				return sfx.insert(function,Typed::Nil()).second;
			}
		}
	};

	namespace Nodes{
		
		/* Copy elision rules */
		void Typed::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			/* default elision action is to abort elision for this branch of ast */
		}

		void Pair::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			if (sfx.GetElision() != Typed::Nil())
			{
				/* a pair node splits elision in two */
				sfx.Pass(First::New(sfx.GetElision()))(GetUp(0));
				sfx.Pass(Rest::New(sfx.GetElision()))(GetUp(1));
			}
		}

		void First::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			if (sfx.GetElision() != Typed::Nil())
			{
				/* first provides elision for the odd side of type tree */
				sfx.Pass(Pair::New(sfx.GetElision(),Typed::Nil()))(GetUp(0));
			}
		}

		void Rest::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			if (sfx.GetElision() != Typed::Nil())
			{
				/* rest provides elision for the even side of the type tree */
				sfx.Pass(Pair::New(Typed::Nil(),sfx.GetElision()))(GetUp(0));
			}
		}

		void Argument::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			//sfx.SetArgumentElision(sfx.elision);
		}

		void Deps::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			if (GetNumCons()) sfx(GetUp(0));
		}

		void Switch::CopyElision(Backends::CopyElisionTransform& sfx) const {
			/* switch consolidates the elision */
			auto f(sfx.sfx.find(this));
			if (f) f->second = Backends::MergeSideEffects(f->second, sfx.GetElision());
			else sfx.sfx.insert(this, sfx.GetElision());
		}

		void FunctionCall::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			/* function call is the endpoint of elision where strains are merged */
			auto f(sfx.sfx.find(this));
			if (f) f->second = Backends::MergeSideEffects(f->second,sfx.GetElision());
			else sfx.sfx.insert(this,sfx.GetElision());

#if 0 // not scalable
			if (sfx.GetElision() != Typed::Nil())
			{
				/* perform elision of data that passes through the function */
				Backends::CopyElisionTransform::ElisionMap tmp;
				CTRef argElision(Typed::Nil());
				Backends::CopyElisionTransform subElision(sfx.GetElision(),argElision,tmp);
				subElision(body);

				if (subElision.GetArgumentElision() != Typed::Nil())
				{
					/* elide some copies throughout a function */
					sfx.Pass(subElision.GetArgumentElision())(GetUp(0));
				}
			}
#endif // not scalable
		}
		void RecursionBranch::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			auto f(sfx.sfx.find(this));
			if (f) f->second = Backends::MergeSideEffects(f->second,sfx.GetElision());
			else sfx.sfx.insert(this,sfx.GetElision());
		}

		void FunctionSequence::CopyElision(Backends::CopyElisionTransform& sfx) const
		{
			auto f(sfx.sfx.find(this));
			if (f) f->second = Backends::MergeSideEffects(f->second,sfx.GetElision());
			else sfx.sfx.insert(this,sfx.GetElision());

#if 0 // not scalable

			auto curElision(sfx.GetElision());				
			if (curElision == Typed::Nil()) return;

			/* todo: closed form elision? */
			for(unsigned i(0);i<num;++i)
			{
				Backends::CopyElisionTransform::ElisionMap tmp;
				CTRef argElision(Typed::Nil());
				Backends::CopyElisionTransform subElision(curElision,argElision,tmp);
				subElision(generator);

				Pair *p;
				if (subElision.GetArgumentElision()->Cast(p)) 
				{
					if (i == 0 && *curElision == *p->GetUp(1)) break; // invariant elision
					curElision = p->GetUp(1);
				}
				else return;
			}

			if (curElision == Typed::Nil()) return;

			Backends::CopyElisionTransform::ElisionMap tmp;
			CTRef argElision(Typed::Nil());
			Backends::CopyElisionTransform tailElision(curElision,argElision,tmp);
			tailElision(tailContinuation);

			curElision = tailElision.GetArgumentElision();

			if (curElision == Typed::Nil()) return;

			for(unsigned i(0);i<num;++i)
			{
				Backends::CopyElisionTransform::ElisionMap tmp;
				CTRef argElision(Typed::Nil());
				Backends::CopyElisionTransform subElision(curElision,argElision,tmp);
				subElision(iterator);

				if (i == 0 && *curElision == *subElision.GetArgumentElision()) break; // invariant

				curElision = subElision.GetArgumentElision();
				if (curElision == Typed::Nil()) return;				
			}

			/* whoa, elision made it through the entire sequence */
			sfx.Pass(curElision)(GetUp(0));
#endif
		}
	};
};