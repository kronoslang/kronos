#pragma once

#include "NodeBases.h"
#include "TLS.h"
#include <cstdint>
#include <utility>

namespace K3 {
	namespace Nodes {

		enum RingBufferTimeBase {
			smp,
			sec
		};

		GENERIC_NODE(GenericRingBuffer, GenericPolyadic)
			RingBufferTimeBase timeBase;
			GenericRingBuffer(RingBufferTimeBase tb, CGRef initializer, CGRef order):timeBase(tb) { Connect(initializer);Connect(order); }
		PUBLIC
			DEFAULT_LOCAL_COMPARE(GenericPolyadic, timeBase)
			static GenericRingBuffer* New(RingBufferTimeBase timeBase, CGRef initializer, CGRef order) {return new GenericRingBuffer(timeBase, initializer,order);}
			static CGRef rbuf(CGRef rb);
			static CGRef cbuf(CGRef rb);
			static CGRef rcsbuf(CGRef rb);
			CGRef IdentityTransform(GraphTransform<const Generic,CGRef>&) const override;
			bool MayHaveRecursion() const override { return true; }
		END

		TYPED_NODE(RingBuffer,TypedPolyadic,IFixedResultType)
		PRIVATE
			size_t len = 0;
			Graph<Typed> lenConfigurator;
			Type elementType;
			RingBuffer(size_t len, CTRef initializer, Type et):len(len),elementType(std::move(et)) { Connect(initializer); }
			RingBuffer(Graph<Typed> len, CTRef initializer, Type et) :lenConfigurator(len), elementType(std::move(et)) { Connect(initializer); }
		PUBLIC
			size_t GetLen() const {return len;}
			DEFAULT_LOCAL_COMPARE(TypedPolyadic,len,lenConfigurator);
			Type Result(ResultTypeTransform&) const override {return FixedResult();}
			Type FixedResult() const override {return len>1?Type::Chain(elementType,len,Type(false)):elementType;}
			static RingBuffer* New(size_t len, CTRef initializer, Type elementType) {return new RingBuffer(len,initializer,std::move(elementType));}
			static RingBuffer* New(Graph<Typed> len, CTRef initializer, Type elementType) { return new RingBuffer(len, initializer, std::move(elementType)); }
			CTRef IdentityTransform(GraphTransform<const Typed,CTRef>&) const override;
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			virtual const Reactive::Node* ReactiveAnalyze(Reactive::Analysis&, const Reactive::Node**) const override;
			virtual CTRef ReactiveReconstruct(Reactive::Analysis&) const override;
			bool MayHaveRecursion( ) const override { return true; }
			unsigned ComputeLocalHash() const override;
			RingBuffer *PubConstructShallowCopy() const { return ConstructShallowCopy(); }
		END
	};
};