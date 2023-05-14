#include "NativeVector.h"
#include "UserErrors.h"
#include "Evaluate.h"
#include "TypeAlgebra.h"
#include "Reactive.h"
#include "RepositoryBuilder.h"

namespace K3 {
	namespace Nodes{

		Specialization GenericPackVector::Specialize(SpecializationTransform& spec) const
		{
			SPECIALIZE_ARGS(spec,0);
			Type tuple = A0.result.Fix();
			if (tuple.IsPair() && tuple.First().IsNativeType() && tuple.First().IsNativeVector() == false) {
				size_t count = tuple.CountLeadingElements(tuple.First());
				bool nilEnd(tuple.Rest(count).IsNil());
				if (tuple.Rest(count) == tuple.First() || nilEnd) {
					if (nilEnd == false) count++;

					PackVector *pack = PackVector::New(unsigned(count),tuple.First());

					unsigned i(0);
					CTRef src(A0.node);
					for(;i<count-1;++i) {
						pack->Connect(src->GraphFirst());
						src = src->GraphRest();
					}

					if (nilEnd) src = src->GraphFirst();
					pack->Connect(src);

					if (count < 2) {
						spec.GetRep().Diagnostic(Verbosity::LogWarnings, this,
												 Error::SpecializationFailed, tuple, "Vector requires at least two elements");
						return SpecializationFailure();
					}

					return Specialization(pack,Type::Vector(tuple.First(),unsigned(count)));
				}
			}
			spec.GetRep().Diagnostic(Verbosity::LogWarnings,this,
				Error::SpecializationFailed,tuple,"Only a homogenic tuple can be packed into a vector");
			return SpecializationFailure();
		}

		Specialization GenericUnpackVector::Specialize(SpecializationTransform& spec) const {
			SPECIALIZE_ARGS(spec,0);
			// unsound??
			Type vec = A0.result.Fix(Type::GenerateNoRules);
			if (vec.IsNativeVector()) {
				CTRef list = nullptr; 
				for(int i(vec.GetVectorWidth() - 1); i>=0; --i) {
					CTRef elem = ExtractVectorElement::New(vec.GetVectorElement(), i, vec.GetVectorWidth(), A0.node);
					if (list) list = Pair::New(elem,list);
					else list = elem;
				}

				return Specialization(list, Type::Chain(vec.GetVectorElement(),vec.GetVectorWidth()-1,vec.GetVectorElement()));
			}
			spec.GetRep().Diagnostic(Verbosity::LogTrace,this,
				Error::SpecializationFailed,vec,"Operand is not a native vector");
			return SpecializationFailure();
		}

		Specialization GenericBroadcastVector::Specialize(SpecializationTransform& spec) const {
			SPECIALIZE_ARGS(spec,0,1);
			Type element = A1.result.Fix();
			Type count = A0.result.Fix();
			if (element.IsNativeType()) {
				if (count.IsInvariant() && count.GetInvariant() > 1 && count.GetInvariant() <= 256) {
					unsigned width((unsigned)count.GetInvariant());
					PackVector* pack = PackVector::New(width,element);
					for(unsigned i(0);i<width;++i) pack->Connect(A1.node);
					return Specialization(pack,Type::Vector(element,width));
				} else {
					spec.GetRep().Diagnostic(Verbosity::LogErrors,this,
						Error::BadInput,count,"Vector width must be an invariant constant from #2 up to #256");
				}
			} else {
				spec.GetRep().Diagnostic(Verbosity::LogWarnings,this,
					Error::SpecializationFailed,element,"Only native atoms can be broadcast into vectors");
			}
			return SpecializationFailure();
		}
	}

	void BuildVectorPrimitiveOps(Parser::RepositoryBuilder pack) {
		using namespace Nodes;
		auto arg = GenericArgument::New();
		auto b1 = GenericFirst::New(arg);
		auto b2 = GenericRest::New(arg);

		//pack.AddFunction("Tick",GenericTick::New(arg),"driver","Provides a reactive clock source with the specified driver.");
		pack.AddFunction("Pack",GenericPackVector::New(arg),"elements...","Converts a homogeneous tuple into a packed vector.");
		pack.AddFunction("Unpack",GenericUnpackVector::New(arg),"vector","Converts a packed vector into a tuple of elements.");
		pack.AddFunction("Broadcast",GenericBroadcastVector::New(b1,b2),"width element","Builds a packed vector by replicating 'element' 'width' times.");
		pack.AddMacro("Native-Vector",GenericTypeTag::New(&VectorTag), false);
	}
}