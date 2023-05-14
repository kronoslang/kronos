#include "Stateful.h"
#include "Evaluate.h"
#include "TypeAlgebra.h"
#include "Invariant.h"
#include "TLS.h"
#include "UserErrors.h"
#include "LibraryRef.h"
#include "Native.h"
#include "Conversions.h"

namespace K3 {
	namespace Nodes {

		class InitializerSynthesisTransform : public Transform::Identity<const Generic> {
			std::unordered_set<const GenericRingBuffer*> decycle;
		public:
			InitializerSynthesisTransform( const GenericRingBuffer* rbuf) : Identity(rbuf->GetUp(2)) {
				decycle.emplace(rbuf);
			}
			CGRef operator()(CGRef src) {
				if (auto grb = ShallowCast<GenericRingBuffer>(src)) {
					static const std::int32_t zero = 0;
					if (!decycle.count(grb)) {
						decycle.emplace(grb);
						return GenericPair::New(
							grb->GetUp(0),
							GenericPair::New(
								(*this)(grb->GetUp(2)),
								Invariant::Constant::New(Type::Int32, &zero)));
					} else {
						return GenericPair::New(
							grb->GetUp(0),
							GenericPair::New(
								grb->GetUp(0),
								Invariant::Constant::New(Type::Int32, &zero)));
					}
				} else {
					return src->IdentityTransform(*this);
				}
			}
		};

		Specialization GenericRingBuffer::Specialize(SpecializationTransform& spec) const {
			assert(GetNumCons() == 3);

			SPECIALIZE(spec, init, GetUp(0));
			SPECIALIZE(spec, order, GetUp(1));

			if (spec.mode == SpecializationTransform::Configuration) {
				return spec.GetRep().TypeError(
					"RingBuffer",
					Type("Can not use stateful expressions in ring buffer configurator"));
			}

			bool generatedInitializer = false;

			if (init.result.IsInvariant()) {
				// attempt to autogenerate initializer
				InitializerSynthesisTransform removeCycle{ this };
				auto prototype = Invariant::Constant::New(init.result);
				auto apply = TLS::ResolveSymbol(":Eval");
				auto coerce = TLS::ResolveSymbol(":Coerce");
				auto sig = removeCycle.Go();

				auto initGen = Evaluate::New("Initializer", apply, GenericPair::New(coerce, GenericPair::New(sig, prototype)));

				SPECIALIZE(spec, generatedInit, initGen);

				init = generatedInit;
				generatedInitializer = true;
			}

			auto lenConfig = order.result;
			RingBuffer* rb = nullptr;
			Type et{ init.result.Fix() };

			if (lenConfig.IsInvariant()) {
				auto len = order.result.GetInvariant();
				
				if (len < 1) {
					spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::InvalidType, order.result, Type::InvariantI64(1), "Ring buffer size argument must be greater than zero");
					return spec.GetRep().TypeError("RingBuffer",
												   Type::Pair(
													   Type("Ring buffer order must be greater than zero; specified as "),
													   order.result));
				}

				rb = RingBuffer::New((size_t)len, init.node, et);

			} else {
				if (lenConfig.GetSize()) {
					spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::InvalidType, order.result,
											 "Ring buffer order can not refer to runtime signals");
					return spec.GetRep().TypeError("RingBuffer",
												   Type::Pair(
													   Type("Ring buffer order can not refer to runtime signals"),
													   order.result));
				}

				// attempt to specialize ring buffer length as a function of sample rate
				auto lenCfgFn =
					Convert::New(
						Convert::Int32,
						Evaluate::CallLib(
						":Eval",
						GenericPair::New(
							Invariant::Constant::New(lenConfig),
							GenericArgument::New())));

				// must disable cache during mode change
				auto specCache = TLS::GetCurrentInstance()->GetSpecializationCache();
				TLS::GetCurrentInstance()->SetSpecializationCache({});
				auto cfgFn = spec.Process(lenCfgFn, Type::Float32, SpecializationState::Configuration);
				TLS::GetCurrentInstance()->SetSpecializationCache(specCache);

				if (!cfgFn.second || cfgFn.first.IsInt32() == false || cfgFn.first.IsNativeVector()) {
					spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::InvalidType, Type::Int32, cfgFn.first, "Could not specialize Ring Buffer configurator as a function of sample rate");
					return spec.GetRep().TypeError("RingBuffer",
												   Type("Could not specialize ring buffer configurator as a function of sample rate"));
				}

				rb = RingBuffer::New(cfgFn.second, init.node, et);
			}

			spec.QueuePostProcessing([&spec,this, rb, et, init, generatedInitializer](Specialization result) -> Specialization {
				auto sig(spec(GetUp(2)));

				if (sig.node == 0) {
					spec.GetRep().Diagnostic(Verbosity::LogErrors,this,Error::BadStateDataSource,"Specialization failed for RingBuffer input signal");
					return sig;
				}

				if (sig.result.Fix() != et) {
					if (generatedInitializer) {
						spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::InvalidType, sig.result, init.result, "Could not automatically deduce ring buffer signal path datatype; please use explicit initialization.");
						return spec.GetRep().TypeError("Could not deduce cycle signal type", sig.result, Type::Pair(Type("Expected"), et));
					} else {
						spec.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::InvalidType, sig.result, init.result, "Ring buffer signal and initializer must be of the same type.");
						return spec.GetRep().TypeError("Cyclic graph has inconsistent types", sig.result, Type::Pair(Type("Expected"), et));
					}
				}

				// SideEffectsTransform puns additional connections to signal source when breaking cycles
				sig.node->HasInvisibleConnections();

				rb->Connect(sig.node);
				return result;
			});

			Type bufferType;
			if (lenConfig.IsInvariant()) {
				if (rb->GetLen() > 1) bufferType = Type::List(et, rb->GetLen());
				else bufferType = et;
			} else {
				bufferType = Type::ArrayView(et);
			}
	
			return Specialization(rb,
				Type::Tuple(
					bufferType,
					et,
					Type::Int32));
		}

		CGRef GenericRingBuffer::rbuf(CGRef rb) {
			return GenericFirst::New(GenericRest::New(rb));
		}

		CGRef GenericRingBuffer::cbuf(CGRef rb) {
			return GenericFirst::New(rb);
		}

		CGRef GenericRingBuffer::rcsbuf(CGRef rb) {
			return rb;
		}


		CGRef GenericRingBuffer::IdentityTransform(GraphTransform<const Generic,CGRef>& copy) const {
			GenericRingBuffer *n(ConstructShallowCopy());

			for(unsigned i(0);i<GetNumCons()-1;++i) n->Reconnect(i,copy(n->GetUp(i)));
			/* handle recursion */
			copy.QueuePostProcessing([n,&copy](CGRef res) {
				int i(n->GetNumCons()-1);
				n->Reconnect(i,copy(n->GetUp(i)));
				return res;
			});
			
			return n;
		}

		CTRef RingBuffer::IdentityTransform(GraphTransform<const Typed,CTRef>& copy) const {
			static const int recursiveInput(1);
			RingBuffer *n(ConstructShallowCopy());
			for(unsigned i(0);i<GetNumCons();++i) {
				if (i!=recursiveInput)
					n->Reconnect(i,copy(n->GetUp(i)));
			}

			assert(n->GetNumCons() > recursiveInput);
			copy.QueuePostProcessing([n,&copy](CTRef res) {
				n->Reconnect(recursiveInput,copy(n->GetUp(recursiveInput)));
				// SideEffectsTransform spoofs connections when breaking cycles
				n->GetUp(recursiveInput)->HasInvisibleConnections();
				return res;
			});
			return n;
		}

		unsigned RingBuffer::ComputeLocalHash() const {
			size_t h(TypedPolyadic::ComputeLocalHash());
			HASHER(h,len);
			HASHER(h,elementType.GetHash());
			return unsigned(h);
		}
	};
};

/*

n = z-1(n + 1i)
t = Table([4096] 
	n		: sig + out * feedback
)

out = Select-Wrap(t n)

out

n = Table(
	[#1 #0]  
	0 : n + 1i
)

Select-Wrap(t n #4)

len = Min(#4 Bound(t))
cpy1 = Bound(t) - n
cpy2 = len - cpy1
memcpy(sel, data + n, cpy1)
memcpy(sel + cpy1, data, cpy2)


*/

