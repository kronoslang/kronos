#include "Conversions.h"
#include "Native.h"
#include "Errors.h"
#include "config/system.h"

namespace K3 {
	namespace Nodes{
		namespace Native {
			void* BinaryenConversion(Backends::BinaryenTransform& xfm, Backends::ActivityMaskVector* avm, const Type& dst, const Type& src, CTRef up);
		}

		template <typename T, typename SRC, int OPCODE>
		TYPED_NODE_INLINE(Cvt,Native::ITypedUnary)
			unsigned vecWidth;
			Cvt(CTRef up, unsigned width):ITypedUnary(up),vecWidth(width) {}
		public:
			DEFAULT_LOCAL_COMPARE(Native::ITypedUnary,vecWidth);
			
			static Typed* New(CTRef up, unsigned vec) {
				Native::Constant *c;
				if (up->Cast(c)) {
					std::vector<T> dest(vec);
					auto srcTy = Type::VectorFromNative<SRC>(vec);
					auto dstTy = Type::VectorFromNative<T>(vec);
					assert(c->FixedResult().OrdinalCompare(srcTy) == 0);
					SRC* ptr = (SRC*)c->GetPointer();
					for (int i(0);i < vec;++i) {
						dest[i] = (T)ptr[i];
					}
					return Native::Constant::New(dstTy, dest.data());
				}
				return new Cvt(up,vec);
			}

			CTRef IdentityTransform(GraphTransform<const Typed, CTRef>& transform) const override {
				auto nc = New(transform(GetUp(0)), vecWidth);
				nc->SetReactivity(GetReactivity());
				return nc;
			}

			const char *PrettyName() const override {return "Cvt";}
			Type FixedResult() const override {return vecWidth>1?Type::Vector(Type::FromNative<T>(),vecWidth):Type::FromNative<T>();}
			Native::Opcode GetOpcode() const override {return (Native::Opcode)OPCODE;}
#ifdef HAVE_LLVM
			Backends::LLVMValue Compile(Backends::LLVMTransform& lt,Backends::ActivityMaskVector*) const
			{
				return EmitCvt<T,SRC>(lt,(Typed*)GetUp(0),vecWidth);
			}
#endif

#ifdef HAVE_BINARYEN
			void* Compile(Backends::BinaryenTransform& xfm, Backends::ActivityMaskVector* avm) const override {
				return Native::BinaryenConversion(xfm, avm, Type::FromNative<T>(), Type::FromNative<SRC>(), GetUp(0));
			}
#endif
			const Reactive::Node* ReactiveAnalyze(Reactive::Analysis& t, const Reactive::Node** upRx) const override {
				return ITypedUnary::ReactiveAnalyze(t, upRx);
			}

			CTRef ReactiveReconstruct(Reactive::Analysis& t) const override {
				return ITypedUnary::ReactiveReconstruct(t);
			}
		END

		template <typename ID, int OPCODE> class Cvt<ID, ID, OPCODE> {
			public:
				static Typed* New(CTRef up, unsigned vec) {
					return (Typed*)up;
				}
		};

		int Convert::LocalCompare(const ImmutableNode& rhs) const
		{
			auto r((Convert&)rhs);
			if (targetType > r.targetType) return 1;
			if (targetType < r.targetType) return -1;
				return GenericUnary::LocalCompare(rhs);
		}

		unsigned Convert::ComputeLocalHash() const
		{
			auto h(GenericUnary::ComputeLocalHash());
			HASHER(h,targetType);
			return h;
		}

		template <typename DST, int OPCODE>
		Typed* _MakeConversionNode(CTRef up, const Type& src) {
			assert(!up->Cast<IFixedResultType>() || up->Cast<IFixedResultType>()->FixedResult() == src);
			if (src.IsFloat32()) return Cvt<DST, float, OPCODE>::New(up, 1);
			else if (src.IsFloat64()) return Cvt<DST, double, OPCODE>::New(up, 1);
			else if (src.IsInt32()) return Cvt<DST, int32_t, OPCODE>::New(up, 1);
			else if (src.IsInt64()) return Cvt<DST, int64_t, OPCODE>::New(up, 1);
			else if (src.IsNativeVector()) {
				Type e(src.GetVectorElement());
				unsigned w(src.GetVectorWidth());
				if (e.IsFloat32()) return Cvt<DST, float, OPCODE>::New(up, w);
				else if (e.IsFloat64()) return Cvt<DST, double, OPCODE>::New(up, w);
				else if (e.IsInt32()) return Cvt<DST, int32_t, OPCODE>::New(up, w);
				else if (e.IsInt64()) return Cvt<DST, int64_t, OPCODE>::New(up, w);
			}
			INTERNAL_ERROR("Bad native conversion");
		}

		template <typename DST, int OPCODE>
		CTRef MakeConversionNode(CTRef up, const Type& src, CRRef rx) {
			auto cvt = _MakeConversionNode<DST, OPCODE>(up, src);
			if (rx) cvt->SetReactivity(rx);
			return cvt;
		}

		template CTRef MakeConversionNode<int64_t, Native::ToInt64>(CTRef, const Type&, CRRef);

		Specialization Convert::Specialize(SpecializationState& t) const
		{
			SPECIALIZE(t,a,GetUp(0));
			if (a.result.IsInvariant())
			{
				switch(targetType)
				{
					case Float32:return Specialization(Native::Constant::New((float)a.result.GetInvariant()),Type::Float32);
					case Float64:return Specialization(Native::Constant::New((double)a.result.GetInvariant()),Type::Float64);
					case Int32:return Specialization(Native::Constant::New((int32_t)a.result.GetInvariantI64()),Type::Int32);
					case Int64:return Specialization(Native::Constant::New((int64_t)a.result.GetInvariantI64()),Type::Int64);
					default:INTERNAL_ERROR("Bad conversion");
				}
			}
			else
			{
				if (a.result.IsNativeType() || a.result.IsNativeVector()) {
					int w = a.result.GetVectorWidth();
					switch(targetType) {
						case Float32:return Specialization(MakeConversionNode<float,Native::ToFloat32>(a.node,a.result,nullptr),Type::Vector(Type::Float32,w));
						case Float64:return Specialization(MakeConversionNode<double,Native::ToFloat64>(a.node,a.result, nullptr),Type::Vector(Type::Float64,w));
						case Int32:return Specialization(MakeConversionNode<int32_t,Native::ToInt32>(a.node,a.result, nullptr),Type::Vector(Type::Int32,w));
						case Int64:return Specialization(MakeConversionNode<int64_t,Native::ToInt64>(a.node,a.result, nullptr),Type::Vector(Type::Int64,w));
					default:INTERNAL_ERROR("Bad conversion");
					}
				}
				else
				{
					t.GetRep().Diagnostic(LogErrors,this,Error::TypeMismatchInSpecialization,a.result,"No native type conversion to %s",NativeTypeName[targetType]);
					return SpecializationFailure();
				}
			}
		}
	};
};