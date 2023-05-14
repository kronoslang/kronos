#pragma once
#include "NodeBases.h"

namespace K3 {
	namespace Nodes{
		GENERIC_NODE(GenericPackVector,GenericUnary)
			GenericPackVector(CGRef t):GenericUnary(t){}
		PUBLIC
			static GenericPackVector* New(CGRef tuple) {return new GenericPackVector(tuple);}
		END

		GENERIC_NODE(GenericUnpackVector,GenericUnary)
			GenericUnpackVector(CGRef v):GenericUnary(v){}
		PUBLIC
			static GenericUnpackVector* New(CGRef vector) {return new GenericUnpackVector(vector);}
		END

		GENERIC_NODE(GenericBroadcastVector,GenericBinary)
			GenericBroadcastVector(CGRef count, CGRef element):GenericBinary(count,element){}
		PUBLIC
			static GenericBroadcastVector* New(CGRef count, CGRef element) {return new GenericBroadcastVector(count,element);}
		END

		TYPED_NODE(PackVector,TypedPolyadic,IFixedResultType)
			PackVector(unsigned w,const Type &e):width(w),element(e){}
			unsigned width;
			const Type element;
		PUBLIC
			Type Result(ResultTypeTransform&) const override {return FixedResult();}
			Type FixedResult() const override {return Type::Vector(element,width);}
			static PackVector* New(unsigned width, const Type& element) {return new PackVector(width,element);}
			virtual int GetWeight() const override { return width; }
			CODEGEN_EMITTER
		END

		TYPED_NODE(UnpackVector,DisposableTypedUnary,IFixedResultType)
		UnpackVector(unsigned w, const Type& t, CTRef up) :DisposableTypedUnary(up), element(t.Fix( )),width(w) { }
			const Type element;
			unsigned width;
		PUBLIC
			Type Result(ResultTypeTransform&) const override {return Type::Chain(element,width-1,element);}
			Type FixedResult() const override {return Type::Chain(element,width-1,element);}
			static UnpackVector* New(unsigned width, const Type& t, CTRef up) {return new UnpackVector(width,t,up);}
//			CTRef SideEffects(Backends::SideEffectTransform&) const;
			virtual int GetWeight() const override { return width; }
		END

		TYPED_NODE(ExtractVectorElement,DisposableTypedUnary,IFixedResultType)
			const Type element;
			unsigned width;
			unsigned idx;
			ExtractVectorElement(const Type& v, unsigned idx, unsigned w, CTRef vec) :DisposableTypedUnary(vec), idx(idx), element(v.Fix( )),width(w) { }
			DEFAULT_LOCAL_COMPARE(DisposableTypedUnary, idx)
		PUBLIC
			static ExtractVectorElement* New(const Type& element, unsigned idx, unsigned maxidx, CTRef vector) {return new ExtractVectorElement(element, idx,maxidx,vector);}
			Type Result(ResultTypeTransform& rt) const override {return element;}
			CODEGEN_EMITTER
			Type FixedResult() const override {return element;}
			unsigned GetIndex() const {return idx;}
			unsigned GetMaxIndex() const { return width; }
			virtual int GetWeight() const override { return 1; }
		END
	};
};