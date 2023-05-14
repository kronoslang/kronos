#include "Evaluate.h"
#include "CompilerNodes.h"
#include "Native.h"
#include "Invariant.h"
// kludge
#include <iostream>

namespace K3
{
	namespace Nodes
	{
		static SpecializationDiagnostic nullDiags(0,LogErrors);
		struct TypedArgumentGenerator : public SpecializationTransform {
			CTRef arg;
			TypedArgumentGenerator(CGRef root,CTRef arg,SpecializationState::Mode m = SpecializationState::Normal):SpecializationTransform(root,Type::Nil,nullDiags,m),arg(arg) {}
			virtual Specialization operate(CGRef node) {
				if (IsOfExactType<GenericArgument>(node)) return Specialization(arg,Type::Int64);
				else return SpecializationTransform::operate(node);
			}
		};
		
		FunctionSequence::FunctionSequence(
			const char* l,CGRef argumentFormula, CGRef resultFormula,
			CTRef iterator, CTRef generator, 
			CTRef tailCall, size_t repeatCount, CTRef arg)
			:FunctionBase(arg),num(repeatCount),iterator(iterator),generator(generator),
			 closedArgument(argumentFormula),closedResult(resultFormula),
			 tailContinuation(tailCall),label(l)  {
			RegionAllocator tmp;
			typedArgument = Specialization(TypedArgumentGenerator(argumentFormula,SequenceCounter::New(0)).Go()).node;
			typedResult = Specialization(TypedArgumentGenerator(resultFormula,
				Native::MakeInt64("usub", Native::Sub, Native::Constant::New(int64_t(repeatCount)),SequenceCounter::New(0))).Go()).node;

			assert(typedArgument && typedResult);
			//std::cout << "* Typed Sequence *\narg: " << *typedArgument << "\nres: "<<*typedResult<<"\n";
		}

		Type FunctionSequence::Result(ResultTypeTransform& withArg) const {
#ifndef NDEBUG
			Type supplied(GetUp(0)->Result(withArg));
			Type deduced(SpecializationTransform::Infer(closedArgument,Type::InvariantI64(0)));
			//assert(supplied == deduced);
#endif
			return SpecializationTransform::Infer(closedResult,Type::InvariantI64(num));
		}

		unsigned FunctionSequence::ComputeLocalHash() const {
			size_t h(FunctionBase::ComputeLocalHash());
			HASHER(h,iterator->GetHash());
			HASHER(h,generator->GetHash());
			HASHER(h,closedArgument->GetHash());
			HASHER(h,closedResult->GetHash());
			HASHER(h,tailContinuation->GetHash());
			HASHER(h,num);
			return (unsigned)h;
		}

		struct ReplaceCounter : public Transform::Identity<const Typed> {
			int64_t counterOffsetDelta;
		public:
			ReplaceCounter(CTRef root, int64_t counterOffsetDelta):Identity(root),counterOffsetDelta(counterOffsetDelta) {}
			CTRef operate(CTRef node) {
				if (SequenceCounter* sc = node->Cast<SequenceCounter>()) return SequenceCounter::New(sc->GetCounterOffset() + counterOffsetDelta);
				else return node->IdentityTransform(*this);
			}
		};

		FunctionBase* FunctionSequence::RemoveIterationsFront(unsigned count) const {
			RegionAllocator peeledRegion(closedArgument->GetHostRegion());
			FunctionSequence *fs(ConstructShallowCopy());

			fs->num = num - count;

			fs->closedArgument = Evaluate::New("PeeledArgument",Invariant::Constant::New(closedArgument),
															Invariant::Add(Invariant::Constant::New(Type::InvariantI64(count)),
															GenericArgument::New()));

			fs->typedArgument = ReplaceCounter(fs->typedArgument,count).Go();
			fs->typedResult = ReplaceCounter(fs->typedResult,count).Go();
			
			assert(fs->typedArgument && fs->typedResult);
			return fs;
		}

		void FunctionSequence::SplitSequenceAt(unsigned count) {			
			if (num <= count) return;
			RegionAllocator outlined(closedResult->GetHostRegion());
			
			FunctionBase *tailFs = RemoveIterationsFront(count);
			tailFs->Reconnect(0,Argument::New());
			tailContinuation = tailFs;

			closedResult = Evaluate::New("PeeledResult",Invariant::Constant::New(closedResult),
											Invariant::Add(Invariant::Constant::New(Type::InvariantI64(num - count)),
														GenericArgument::New()));				
			if (count == 1) {
				closedArgument = Invariant::Constant::New(ArgumentType());
				typedArgument = Native::Constant::New(ArgumentType(),0);
				typedResult = Native::Constant::New(SpecializationTransform::Infer(closedResult, Type::InvariantI64(0)), 0);
			}

			assert(typedArgument && typedResult);
			num = count;
		}
	}
}