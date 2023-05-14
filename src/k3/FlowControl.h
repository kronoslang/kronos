#pragma once
#include "NodeBases.h"

namespace K3 {
	namespace Nodes{

		class FunctionCall;

		GENERIC_NODE(When,GenericPolyadic)
		public:
			static When* New() { return new When(); }
			END

				GENERIC_NODE(Polymorphic, GenericPolyadic)
		public:
			static Polymorphic* New() { return new Polymorphic; }
			END

				GENERIC_NODE(GenericCond, GenericUnary)
				GenericCond(CGRef a) :GenericUnary(a) {}
		public:
			static GenericCond* New(CGRef conditions) { return new GenericCond(conditions); }
			END

				GENERIC_NODE(GenericDispatch, GenericUnary)
				GenericDispatch(CGRef args) :GenericUnary(args) {}
			PUBLIC
				static GenericDispatch* New(CGRef args) { return new GenericDispatch(args); }
			END

				GENERIC_NODE(GenericUnionSubtype, GenericUnary)
				int sti;
			GenericUnionSubtype(CGRef data, int subTypeIndex) :sti(subTypeIndex), GenericUnary(data) {}
			PUBLIC
				static GenericUnionSubtype* New(CGRef data, int subTypeIndex) { return new GenericUnionSubtype(data, subTypeIndex); }
			END

				TYPED_NODE(UnsafePointerCast, DisposableTypedUnary, IFixedResultType)
				UnsafePointerCast(const Type t, CTRef p) : DisposableTypedUnary(p), t(t) {}
			Type t;
			PUBLIC
				static UnsafePointerCast* New(const Type& t, CTRef ptr) { return new UnsafePointerCast(t, ptr); }
			Type Result(ResultTypeTransform&) const override { return t; }
			Type FixedResult() const override { return t; }
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			END

				GENERIC_NODE(Raise, GenericUnary)
				Raise(CGRef ex) :GenericUnary(ex) {}
		public:
			static Raise* New(CGRef exception) { return new Raise(exception); }
			static Raise* NewFatalFailure(const char* msg, CGRef args = nullptr);
			END

				GENERIC_NODE(Handle, GenericBinary)
				Handle(CGRef ptry, CGRef pcatch) :GenericBinary(ptry, pcatch) {}
		public:
			static Handle* New(CGRef tryGraph, CGRef catchGraph) { return new Handle(tryGraph, catchGraph); }
			END

				GENERIC_NODE(GenericGetVectorLen, GenericUnary)
				GenericGetVectorLen(CGRef sig) :GenericUnary(sig) {}
			PUBLIC
				static GenericGetVectorLen* New(CGRef sig) { return new GenericGetVectorLen(sig); }
			END

				GENERIC_NODE(GenericSelect, GenericTernary)
				GenericSelect(CGRef vector, CGRef idx, CGRef max) : GenericTernary(vector, idx, max) {}
			PUBLIC
				static GenericSelect* New(CGRef vec, CGRef idx, CGRef max) { return new GenericSelect(vec, idx, max); }
			END

			GENERIC_NODE(GenericConstantSelect, GenericUnary)
				bool wrap;
			GenericConstantSelect(CGRef a, bool w) :wrap(w), GenericUnary(a) {}
			PUBLIC
				DEFAULT_LOCAL_COMPARE(GenericUnary, wrap);
			static GenericConstantSelect* New(CGRef vecidx, bool wrap) { return new GenericConstantSelect(vecidx, wrap); }
			END

			GENERIC_NODE(GenericSlice, GenericTernary)
				GenericSlice(CGRef vec, CGRef skip, CGRef take) :GenericTernary(vec, skip, take) {}
			PUBLIC
				static GenericSlice* New(CGRef vec, CGRef skip, CGRef take) { return new GenericSlice(vec, skip, take); }
			END

			GENERIC_NODE(GenericSliceArity, GenericUnary)
				GenericSliceArity(CGRef vec) : GenericUnary(vec) {}
			PUBLIC
				static GenericSliceArity* New(CGRef vec) { return new GenericSliceArity(vec); }
			END


		struct SliceSource {
			Type elementType;
			int sourceLen;
			Type GetElementType() const {
				return elementType;
			}

			Type GetSourceArrayType() const {
				return Type::List(elementType, sourceLen);				
			}
		};

		TYPED_NODE(Slice, DisposableTypedTernary, IFixedResultType)
			SliceSource src;
			Slice(int sl, Type et, CTRef vec, CTRef skip, CTRef take) 
				:src({et, sl})
				,DisposableTypedTernary(vec,skip,take) { }
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedTernary, src.elementType);
			static Slice* New(int staticLen, Type buffer, CTRef vector, CTRef skip, CTRef take) {
				return new Slice(staticLen, buffer, vector, skip, take);
			}

			Type FixedResult() const override { return Type::ArrayView(src.GetElementType()); }
			Type Result(ResultTypeTransform&) const override { return Type::ArrayView(src.GetElementType()); }
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
		END

		TYPED_NODE(SliceArity, TypedUnary, IFixedResultType)
			SliceArity(CTRef slc) :TypedUnary(slc) { }
		PUBLIC
			static SliceArity* New(CTRef slice) { return new SliceArity(slice); }
			Type FixedResult() const override { return Type::Int32; }
			Type Result(ResultTypeTransform&) const override { return Type::Int32; }
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
		END

		TYPED_NODE(SubArray, DisposableTypedBinary, IFixedResultType)
			int sliceLen;
			SliceSource src;
			SubArray(int len, int sl, Type et, CTRef vec, CTRef skip)
				:DisposableTypedBinary(vec, skip)
				, src({ et, sl })
				, sliceLen(len) {}
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedBinary, src.elementType, sliceLen);
			static SubArray* New(int len, int srcLen, Type bt, CTRef vec, CTRef skip) {
				return new SubArray(len, srcLen, bt, vec, skip);
			}
			Type FixedResult() const override {
				return Type::Tuple(src.GetElementType(), sliceLen);
			}
			Type Result(ResultTypeTransform&) const override {
				return FixedResult();
			}
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
		END

			
		GENERIC_NODE(GenericTernaryAtom,GenericTernary)
		GenericTernaryAtom(CGRef cond, CGRef whenTrue, CGRef whenFalse):GenericTernary(cond,whenTrue,whenFalse) {}
		PUBLIC
			static GenericTernaryAtom* New(CGRef vec, CGRef t, CGRef f) { return new GenericTernaryAtom(vec,t,f); }
		END

		TYPED_NODE(AtIndex, DisposableTypedBinary, IFixedResultType)
			Type vectorTy;
			Type elem;
			size_t size;
			AtIndex(Type v, Type e, size_t order, CTRef vector, CTRef index):DisposableTypedBinary(vector,index),vectorTy(v),elem(e.Fix()),size(order) {}
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedBinary,vectorTy);
			CODEGEN_EMITTER
			static CTRef New(Type vectorType, CTRef vector, CTRef index); 
			Type Result(ResultTypeTransform&) const override { return elem; }
			Type FixedResult() const override { return elem; }
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			CTRef IdentityTransform(GraphTransform<const Typed, CTRef>& transform) const override;
			virtual int GetWeight() const override { return 2; }
		END


		TYPED_NODE(Switch, TypedPolyadic, IFixedResultType)
			Type result;
			std::vector<int> branchResultSubtypeIndex;
			Switch(CTRef arg, const Type& r) :result(r) { Connect(arg); }
		PUBLIC
			static Switch* New(CTRef arg, const std::vector<Specialization>& branches, const Type& r); 
			int LocalCompare(const ImmutableNode&) const override;
			unsigned ComputeLocalHash() const override;
			Type Result(ResultTypeTransform&) const override { return result; }
			Type FixedResult() const override { return result; }
			CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			Switch* MakeMutableCopy() const { return ConstructShallowCopy(); }
			void CopyElision(Backends::CopyElisionTransform& sfx) const override;
			virtual int GetWeight() const  override { return (int)branchResultSubtypeIndex.size() * 5; }
		END


		class Subroutine;
		
		TYPED_NODE(MultiDispatch, DisposableTypedTernary, IFixedResultType)
			friend class Switch;
			std::vector<std::pair<const Subroutine*,int>> dispatchees;
			Type result;
			MultiDispatch(const Type &res, CTRef swIndex, CTRef subTypeIdx, CTRef statePtr) :DisposableTypedTernary(swIndex, subTypeIdx, statePtr),result(res) {}
		PUBLIC
			static MultiDispatch* New(const Type& r, CTRef swIndex, CTRef subtypeIndexDst, CTRef initialState) { return new MultiDispatch(r, swIndex, subtypeIndexDst, initialState); }
			CODEGEN_EMITTER
			Type Result(ResultTypeTransform&) const override { return FixedResult(); }
			Type FixedResult() const override { return result; }
			const std::vector<std::pair<const Subroutine*, int>>& GetDispatchees() const { return dispatchees; }
			int GetWeight() const  override{ return (int)dispatchees.size() * 5; }
		END
	};
};