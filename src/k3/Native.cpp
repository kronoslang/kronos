#include "Native.h"
#include "Errors.h"
#include "TypeAlgebra.h"
#include "Evaluate.h"
#include "Conversions.h"
#include "FlowControl.h"
#include "Invariant.h"
#include "Reactive.h"

#include "config/cxx.h"

#include <sstream>
#include <math.h>


using BigNum = ttmath::Big<1, 2>;

static int32_t _SQRT(int32_t) { KRONOS_UNREACHABLE; }
static int64_t _SQRT(int64_t) { KRONOS_UNREACHABLE; }

#define BN_EmuFPO(FUNC, CRT) static BigNum FUNC(BigNum x) { return CRT(x.ToDouble()); }

static BigNum _FLOOR(BigNum f) {
	f -= 0.5;
	f.Round();
	return f;
}

static BigNum _CEIL(BigNum f) {
	f += 0.5;
	f.Round();
	return f;
}

static BigNum _ROUND(BigNum r) {
	r.Round();
	return r;
}

static BigNum _TRUNC(BigNum t) {
	if (t > 0) t -= 0.5;
	else t += 0.5;
	t.Round();
	return t;
}

static BigNum _SQRT(BigNum x) {
	x.Sqrt();
	return x;
}

#include "CxxPrimitiveOps.inc"

template <> BigNum _COS<BigNum>(BigNum x) {
	return ttmath::Cos(x);
}

template <> BigNum _SIN<BigNum>(BigNum x) {
	return ttmath::Sin(x);
}

template <> BigNum _EXP<BigNum>(BigNum x) {
	BigNum result;
	result.Exp(x);
	return result;
}

template <> BigNum _ABS<BigNum>(BigNum x) {
	x.Abs();
	return x;
}

template <> BigNum _NOT<BigNum>(BigNum a) {
	ttmath::Int<4> ai;
	a.ToInt(ai);
	ai.BitNot();
	a.FromInt(ai);
	return a;
}

template <> BigNum _NEG<BigNum>(BigNum a) {
	a.ChangeSign();
	return a;
}

template <> BigNum _LOG<BigNum>(BigNum x) {
	BigNum result;
	result.Ln(x);
	return result;
}

template <> BigNum _LOG10<BigNum>(BigNum x) {
	BigNum result;
	result.Log(x, 10);
	return result;
}

template <> BigNum _POW<BigNum>(BigNum x, BigNum e) {
	x.Pow(e);
	return x;
}

template <> BigNum _ATAN2(BigNum y, BigNum x) {
	BigNum pi, zero;
	pi.SetPi();
	zero.SetZero();
	if (x > zero) {
		return ttmath::ATan(y / x);
	} else if (x < zero) {
		if (y < zero) {
			return ttmath::ATan(y / x) - pi;
		} else {
			return ttmath::ATan(y / x) + pi;
		}
	} else {
		if (y > zero) return pi / 2;
		else if (y < zero) return pi / 2;
		else return zero;
	}
}

template <> BigNum _AND<BigNum>(BigNum a, BigNum b) {
	ttmath::Int<4> ai, bi;
	a.ToUInt(ai); b.ToUInt(bi);
	a.FromInt(ai & bi);
	return a;
}

template <> BigNum _ANDNOT<BigNum>(BigNum a, BigNum b) {
	ttmath::Int<4> ai, bi;
	a.ToUInt(ai); b.ToUInt(bi);
	a.FromInt(~ai & bi);
	return a;
}

template <> BigNum _OR<BigNum>(BigNum a, BigNum b) {
	ttmath::Int<4> ai, bi;
	a.ToUInt(ai); b.ToUInt(bi);
	a.FromInt(ai | bi);
	return a;
}

template <> BigNum _XOR<BigNum>(BigNum a, BigNum b) {
	ttmath::Int<4> ai, bi;
	a.ToUInt(ai); b.ToUInt(bi);
	a.FromInt(ai ^ bi);
	return a;
}


namespace K3 {
	namespace Nodes {

		bool Typed::IsNil(CTRef n) {
			return IsOfExactType<Native::ConstantNil>(n);
		}

		namespace Native {
			static const char *Mnemonic[] = {
#define F(OP) #OP ,
				EXPAND_KRONOS_NATIVE_OPCODES
#undef F
				""
			};

			Constant::Constant(const void *mem, size_t len, const Type& t) :type(t) {
				if (mem && t.GetSize()) {
					memory = malloc(len);
					memcpy(memory, mem, len);
					this->len = len;
				} else {
					memory = 0;
					this->len = 0;
				}
#ifndef NDEBUG
				std::stringstream ss;
				Output(ss);
				readableLabel = ss.str();
#endif
			}


			static ConstantNil cn;
			ConstantNil* ConstantNil::Get() {
				cn.MakeStatic();
				return &cn;
			}

			int Constant::LocalCompare(const ImmutableNode& rhs) const {
				auto& r((const Constant&)rhs);
				auto t(ordinalCmp(type, r.type));
				if (t) return t;
				t = ordinalCmp(len, r.len);
				if (t) return t;
				
				if (memory == nullptr && r.memory != nullptr) return 1;
				else if (memory != nullptr && r.memory == nullptr) return -1;
				else t = memcmp(memory, r.memory, len);
				if (t) return t;
				return DisposableTypedLeaf::LocalCompare(rhs);
			}

			unsigned Constant::ComputeLocalHash() const {
				auto h(DisposableTypedLeaf::ComputeLocalHash());
				HASHER(h, (unsigned)len);
				for (unsigned i(0); i < len / sizeof(uint32_t); i++) HASHER(h, ((uint32_t*)memory)[i]);
				return h;
			}

			CTRef Constant::PairWithRest(CTRef rst) const {
				Constant *c;

				if (rst->Cast(c)) {
					if (GetReactivity() != rst->GetReactivity()) {
						return MakeFullPair(this, rst);
					}

					if (memory || c->memory) {
						assert((memory == nullptr) == (FixedResult().GetSize() == 0));
						assert((c->memory == nullptr) == (c->FixedResult().GetSize() == 0));
						std::vector<std::uint8_t> buf(FixedResult().GetSize() + c->FixedResult().GetSize());
						if (GetPointer()) 
							memcpy(buf.data(), GetPointer(), FixedResult().GetSize());
						if (c->GetPointer()) 
							memcpy(buf.data() + FixedResult().GetSize(), c->GetPointer(), c->FixedResult().GetSize());
						return New(Type::Pair(FixedResult(), c->FixedResult()), buf.data());
					} else {
						return New(Type::Pair(FixedResult(), c->FixedResult()), nullptr);
					}
				} else return MakeFullPair(this, rst);
			}

			Constant* Constant::New(float datum) {
				assert(sizeof(float) == 4);
				return new Constant(&datum, sizeof(float), Type::Float32);
			}

			Constant* Constant::New(double datum) {
				assert(sizeof(double) == 8);
				return new Constant(&datum, sizeof(double), Type::Float64);
			}

			Constant* Constant::New(int32_t datum) {
				assert(sizeof(int32_t) == 4);
				return new Constant(&datum, sizeof(int32_t), Type::Int32);
			}

			Constant* Constant::New(int64_t datum) {
				assert(sizeof(int64_t) == 8);
				return new Constant(&datum, sizeof(int64_t), Type::Int64);
			}

			int64_t Constant::AsInteger() const {
				if (type.IsInt32()) return *(int32_t*)memory;
				else if (type.IsInt64()) return *(int64_t*)memory;
				else INTERNAL_ERROR("Native::Constant should be an integer but isn't");
			}

			Constant* Constant::New(const Type& type, const void *mem = 0) {
				if (type.GetSize() == 0) {
					return ConstantNil::Get();
				} 
				return new Constant(mem, type.GetSize(), type.Fix());
			}

			Constant::Constant(const Constant& src) : DisposableTypedLeaf(src),type(src.type) {
				assert(IsNil(&src) == false);
				if (src.memory && src.FixedResult().GetSize() > 0) {
					memory = malloc(src.len);
					len = src.len;
					memcpy(memory, src.memory, len);
				} else { 
					memory = 0; 
					len = 0; 
				}
#ifndef NDEBUG
				readableLabel = src.readableLabel;
#endif
			}

			void Constant::Output(std::ostream& strm) const {
				if (type.IsFloat32()) {
					if (memory) strm << *((float*)memory); else strm << "f32";
				} else if (type.IsFloat64()) {
					if (memory) strm << *((double*)memory); else strm << "f64";
				} else if (type.IsInt32()) {
					if (memory) strm << *((int32_t*)memory); else strm << "i32";
				} else if (type.IsInt64()) {
					if (memory) strm << *((int64_t*)memory); else strm << "i64";
				} else strm << type;
			}

			CTRef ConstantNil::GraphFirst() const {
				return this;
			}

			CTRef ConstantNil::GraphRest() const {
				return this;
			}

			Constant* ConstantNil::ConstructShallowCopy() const {
				return new ConstantNil();
			}


			Constant::~Constant() {
				free(memory);
			}

			CTRef Constant::GraphFirst() const {
				if (GetReactivity() == nullptr) {
					if (memory == nullptr) return Constant::New(type.First(), 0);
				}
				return Constant::New(type.First(), memory);
			}

			CTRef Constant::GraphRest() const {
				if (GetReactivity() == nullptr) {
					if (memory == 0) return Constant::New(type.Rest(), 0);
				}
				return Constant::New(type.Rest(), (const char *)memory + type.First().GetSize());
			}

			//CTRef ConstantNil::ReactiveReconstruct(Reactive::Analysis& a) const {
			//	return Get();
			//}

			template <typename T> bool TypeCheck(CTRef t, int vw = 1) {
				if (auto c = t->Cast<IFixedResultType>()) {
					auto t = c->FixedResult();
					while (t.IsUserType()) t = t.UnwrapUserType();
					if (t.IsNativeVector()) {
						return 
							t.GetVectorElement() == Type::FromNative<T>() &&
							t.GetVectorWidth() == vw;
					}
					return (c->FixedResult() == Type::FromNative<T>() && vw == 1);
				}
				return true;
			}

			template <typename T> class TBin : public  ITypedBinary {
				REGION_ALLOC(TBin)
				INHERIT_RENAME(TBin, ITypedBinary)
				T(*action)(T, T);
				std::uint8_t vec;
				Native::Opcode opcode;
				TBin *ConstructShallowCopy() const override { 
					return new TBin(*this); 
				}
			public:
				DEFAULT_LOCAL_COMPARE(ITypedBinary, opcode, vec)

				unsigned int ComputeLocalHash() const override {
					unsigned int h(ITypedBinary::ComputeLocalHash());
					HASHER(h, vec); HASHER(h, opcode);
					return h;
				}

				static Typed* New(CTRef a, CTRef b, Native::Opcode op, std::uint8_t vectorWidth, T(*action)(T, T)) {
					assert(TypeCheck<T>(a, vectorWidth) && TypeCheck<T>(b, vectorWidth));
					Constant *ac, *bc;
					if (a->Cast(ac) && b->Cast(bc) && ac->GetPointer() && bc->GetPointer()) {
						T* src1 = (T*)ac->GetPointer();
						T* src2 = (T*)bc->GetPointer();
						assert(src1 && src2);
						if (action && src1 && src2) {
							std::vector<T> dest(vectorWidth);

							for (int i(0);i < vectorWidth;++i) dest[i] = action(src1[i], src2[i]);
							return Constant::New(vectorWidth > 1 ? Type::VectorFromNative<T>(vectorWidth) : Type::FromNative<T>(), dest.data());
						}
					}
					return new TBin(a, b, op, vectorWidth, action);
				}

				CTRef IdentityTransform(GraphTransform<const Typed, CTRef>& st) const override {
					auto n = New(st(GetUp(0)), st(GetUp(1)), GetOpcode(), vec, action); 
					n->SetReactivity(GetReactivity());
					return n;
				}

				CTRef ReactiveReconstruct(Reactive::Analysis& a) const override {
					// short-circuit the zero-of-type idiom after reactive analysis
					if (GetOpcode() == Native::Sub &&
						*GetUp(0) == *GetUp(1)) {
						auto c = Constant::New(T(0));
						c->SetReactivity(a.ReactivityOf(this));
						return c;
					}
					// when division rates diverge, split into reciprocal
					if (GetOpcode() == Native::Div) {
						auto dividendRx = a.ReactivityOf(GetUp(0));
						auto divisorRx = a.ReactivityOf(GetUp(1));
						auto myRx = a.ReactivityOf(this);
						if (myRx != divisorRx) {
							Typed* one;
							if (vec > 1) {
								std::vector<T> vbuf(vec);
								for (int i = 0; i < vec; ++i) vbuf[i] = (T)1;
								one = Constant::New(Type::VectorFromNative<T>(vec), vbuf.data());
							} else {
								one = Constant::New((T(1)));
							}
							auto recip = New(one, a(GetUp(1)), Opcode::Div, vec, nullptr);
							auto mul = New(a.Boundary(a(GetUp(0)), myRx, dividendRx),
										   a.Boundary(recip, myRx, divisorRx), 
										   Opcode::Mul, vec, nullptr);

							one->SetReactivity(divisorRx);
							recip->SetReactivity(divisorRx);
							mul->SetReactivity(myRx);

							return mul;
						}
					}
					return ITypedBinary::ReactiveReconstruct(a);
				}

				TBin(CTRef a, CTRef b, Native::Opcode opcode, std::uint8_t vec, T(*action)(T, T)) :ITypedBinary(a, b), vec(vec), opcode(opcode), action(action) {}
				const char *PrettyName() const override { return Mnemonic[GetOpcode()]; }
				Opcode GetOpcode() const override { return opcode; }

				Type FixedResult() const override {
					return Type::VectorFromNative<T>(vec);
				}
			};

			template <typename T> Typed* MakeTyped(
				Opcode opcode,
				T(*action)(T, T),
				CTRef a, CTRef b,
				unsigned vectorWidth) {
				return TBin<T>::New(a, b, opcode, vectorWidth, action);
			}

			template <typename T> class TUn : public ITypedUnary {
				REGION_ALLOC(TUn)
				INHERIT_RENAME(TUn, ITypedUnary)
				T(*action)(T);
				std::uint8_t vec;
				Native::Opcode opcode;
				TUn* ConstructShallowCopy() const override { return new TUn(*this); }
			public:
				DEFAULT_LOCAL_COMPARE(ITypedUnary, opcode, vec)
				static Typed* New(CTRef a, Native::Opcode op, std::uint8_t vectorWidth, T(*action)(T)) {
					assert(TypeCheck<T>(a, vectorWidth));
					Constant *ac(nullptr);
					if (action && a->Cast(ac) && ac->GetPointer()) {
						std::vector<T> dest(vectorWidth);
						T* src = (T*)ac->GetPointer();
						for (int i(0);i < vectorWidth;++i) dest[i] = action(src[i]);
						return Constant::New(vectorWidth > 1 ? Type::VectorFromNative<T>(vectorWidth) : Type::FromNative<T>(), dest.data());
					}
					return new TUn(a, op, vectorWidth, action);
				}

				TUn(CTRef a, Native::Opcode opcode, std::uint8_t vec, T(*action)(T)) :ITypedUnary(a), vec(vec), opcode(opcode), action(action) {}
				const char *PrettyName() const override { return Mnemonic[GetOpcode()]; }
				Opcode GetOpcode() const override { return opcode; }

				Type FixedResult() const override {
					return Type::VectorFromNative<T>(vec);
				}
			};

			template <typename T> Typed* MakeTyped(Native::Opcode opcode,
												   T(*action)(T),
												   CTRef a,
												   std::uint8_t vectorWidth) {
				return TUn<T>::New(a, opcode, vectorWidth, action);
			}

			template <int OPCODE> CGRef Make(
				const char *label,
				float(*actf)(float, float),
				double(*actd)(double, double),
				int32_t(*acti)(int32_t, int32_t),
				int64_t(*actq)(int64_t, int64_t),
				BigNum(*actb)(BigNum, BigNum),
				CGRef a, CGRef b) {
				static auto f_func(actf);
				static auto d_func(actd);
				static auto i_func(acti);
				static auto q_func(actq);
				static auto b_func(actb);

				GENERIC_NODE_INLINE(GBin, IGenericBinary)
					PUBLIC
					static GBin* New(CGRef a, CGRef b) { return new GBin(a, b); }
				GBin(CGRef a, CGRef b) :IGenericBinary(a, b) {}
				const char *PrettyName() const override { return Mnemonic[GetOpcode()]; }
				Opcode GetOpcode() const override { return (Opcode)OPCODE; }
				Specialization Specialize(SpecializationState& spec) const override {
					SPECIALIZE(spec, a, GetUp(0));
					SPECIALIZE(spec, b, GetUp(1));
					a.result.Fix();
					b.result.Fix();
					if (OPCODE > Native::BuildForVariantsOnly && d_func &&
						a.result.IsInvariant() && b.result.IsInvariant()) {
						// these opcodes have simple implementation for invariants
						return Specialization(Typed::Nil(), Type(b_func(a.result.GetBigNum(), b.result.GetBigNum())));
					} else if ((a.result.IsNativeType() && b.result.IsNativeType()) ||
							   (a.result.IsNativeVector() && b.result.IsNativeVector())) {
						if (a.result == b.result) {
							if (OPCODE > DefinedForAllTypes) {
                                
                                Type e = a.result;
                                if (e.IsNativeVector()) e = e.GetVectorElement();
                                
								if (OPCODE < DefinedForFloatTypes) {
									if (e.IsFloat32() == false && e.IsFloat64() == false) {
										spec.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization,
																 a.result, "'%s' only supports floating point types", PrettyName());
										return SpecializationFailure();
									}
								} else if (OPCODE < DefinedForFloat32) {
									if (e.IsFloat32() == false) {
										spec.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization,
																 a.result, "'%s' only supports single precision floating point",PrettyName());
										return SpecializationFailure();
									}
								} else if (OPCODE < DefinedForIntegerTypes) {
									if (e.IsInt32() == false && e.IsInt64() == false) {
										spec.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization,
																 a.result, "'%s' only supports integer types", PrettyName());
										return SpecializationFailure();
									}
								}
							}

							unsigned vecWidth(a.result.GetVectorWidth());
							Type e(a.result.GetVectorElement());

							/* type match */
							if (e.IsFloat32()) return Specialization(MakeTyped<float>((Opcode)OPCODE, f_func, a.node, b.node, vecWidth), a.result.Fix());
							else if (e.IsFloat64()) return Specialization(MakeTyped<double>((Opcode)OPCODE, d_func, a.node, b.node, vecWidth), a.result.Fix());
							else if (e.IsInt32()) return Specialization(MakeTyped<int32_t>((Opcode)OPCODE, i_func, a.node, b.node, vecWidth), a.result.Fix());
							else if (e.IsInt64()) return Specialization(MakeTyped<int64_t>((Opcode)OPCODE, q_func, a.node, b.node, vecWidth), a.result.Fix());
							else INTERNAL_ERROR("Bad type in native operator");
						} else {
							spec.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization,
													 Type::Pair(a.result, b.result), "Type mismatch in binary operator");
							return SpecializationFailure();
						}
					} else {
						if (!a.result.IsNativeType() && !a.result.IsNativeVector()) spec.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization,
																											 a.result, "Cannot '%s' these types natively", PrettyName());
						else if (!b.result.IsNativeType() && !b.result.IsNativeVector()) spec.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization,
																												  b.result, "Cannot '%s' these types natively", PrettyName());
						return SpecializationFailure();
					}
				}
				END

					return GBin::New(a, b);
			}

			template <int OPCODE> CGRef Make(
				const char *label,
				float(*actf)(float),
				double(*actd)(double),
				int32_t(*acti)(int32_t),
				int64_t(*actq)(int64_t),
				BigNum(*actb)(BigNum),
				CGRef a) {
				static auto f_func(actf);
				static auto d_func(actd);
				static auto i_func(acti);
				static auto q_func(actq);
				static auto b_func(actb);

				GENERIC_NODE_INLINE(GUn, IGenericUnary)
					PUBLIC
					static GUn* New(CGRef a) { return new GUn(a); }
				GUn(CGRef a) :IGenericUnary(a) {}
				const char *PrettyName() const override { return Mnemonic[GetOpcode()]; }
				Opcode GetOpcode() const override { return (Opcode)OPCODE; }
				Specialization Specialize(SpecializationState& spec) const override {
					SPECIALIZE(spec, a, GetUp(0));
					if (OPCODE > BuildForVariantsOnly && a.result.IsInvariant() && d_func) {
						// this opcode has a simple implementation for invariants
						return Specialization(Typed::Nil(), Type(b_func(a.result.GetBigNum())));
					} if (a.result.IsNativeType() || a.result.IsNativeVector()) {
						a.result.Fix();
						unsigned vecWidth(a.result.GetVectorWidth());
						Type e(a.result.GetVectorElement());
						if (OPCODE > DefinedForAllTypes) {
							if ((e.IsFloat32() == false && e.IsFloat64() == false)
								|| (OPCODE > DefinedForFloatTypes && e.IsFloat32() == false)) {
								spec.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization,
														 e, "Cannot '%s' this type natively", PrettyName());
								return SpecializationFailure();
							}
						}

						/* type match */
						if (e.IsFloat32()) return Specialization(MakeTyped<float>((Opcode)OPCODE, f_func, a.node, vecWidth), a.result.Fix());
						else if (e.IsFloat64()) return Specialization(MakeTyped<double>((Opcode)OPCODE, d_func, a.node, vecWidth), a.result.Fix());
						else if (e.IsInt32()) return Specialization(MakeTyped<int32_t>((Opcode)OPCODE, i_func, a.node, vecWidth), a.result.Fix());
						else if (e.IsInt64()) return Specialization(MakeTyped<int64_t>((Opcode)OPCODE, q_func, a.node, vecWidth), a.result.Fix());
						else INTERNAL_ERROR("Bad type in native operator");
					} else {
						spec.GetRep().Diagnostic(LogErrors, this, Error::TypeMismatchInSpecialization,
												 a.result, "Cannot '%s' this type natively", PrettyName());
						return SpecializationFailure();
					}
				}
				END

					return GUn::New(a);
			}

			Typed *MakeInt64(const char *label, int opcode, CTRef a, CTRef b) {
				int64_t(*action)(int64_t, int64_t) = nullptr;
				switch (opcode) {
				case Add:
					action = [](int64_t a, int64_t b) { return a + b; };
					break;
				case Mul:
					action = [](int64_t a, int64_t b) { return a * b; };
					break;
				case Sub:
					action = [](int64_t a, int64_t b) { return a - b; };
					break;
				case Min:
					action = [](int64_t a, int64_t b) { return std::min(a, b); };
					break;
				}
				return MakeTyped<std::int64_t>((Opcode)opcode, action, a, b, 1);
			}

			Typed *MakeFloat(const char *label, int opcode, CTRef a, CTRef b) {
				return MakeTyped<float>((Opcode)opcode, nullptr, a, b, 1);
			}

			Typed *MakeInt32(const char *label, int opcode, CTRef a, CTRef b) {
				int32_t(*action)(int32_t, int32_t) = nullptr;
				switch (opcode) {
				case Add: 
					action = [](int32_t a, int32_t b) { return a + b; };
					break;
				case Mul:
					action = [](int32_t a, int32_t b) { return a * b; };
					break;
				case Sub:
					action = [](int32_t a, int32_t b) { return a - b; };
					break;
				case Min:
					action = [](int32_t a, int32_t b) { return std::min(a, b); };
					break;
				}

				return MakeTyped<std::int32_t>((Opcode)opcode, action, a, b, 1);
			}

			Typed* MakeInt64(const char *label, int opcode, CTRef a) {
				return MakeTyped<std::int64_t>((Opcode)opcode, nullptr, a, 1);
			}

			Typed* MakeFloat(const char *label, int opcode, CTRef a) {
				return MakeTyped<float>((Opcode)opcode, nullptr, a, 1);
			}

			Specialization GenericBitcast::Specialize(SpecializationTransform& t) const {
				SPECIALIZE(t, a, GetUp(0));
				Type from = a.result.Fix();
                int vectorWidth = 1;
                
                if (from.IsNativeVector()) {
                    vectorWidth = from.GetVectorWidth();
                    from = from.GetVectorElement();
                }

                Type dstTy;

				if (!fromInt) {
					if (from.IsInvariant()) {
                        assert(vectorWidth == 1);
						double dv = from.GetInvariant();
						std::int64_t iv = 0;
						memcpy((char*)&iv, (const char*)&dv, sizeof(std::int64_t));
						return Specialization(ConstantNil::Get(), Type::InvariantLD(dv));
					}
					if (from.IsFloat32()) {
                        dstTy = Type::Int32;
					} else if (from.IsFloat64()) {
                        dstTy = Type::Int64;
                    } else {
                        t.GetRep().Diagnostic(LogErrors, this, Error::SpecializationFailed, "Bitcast source value is not an integer");
                        return SpecializationFailure();
                    }
				} else {
					if (from.IsInvariant()) {
                        assert(vectorWidth == 1);
						std::int64_t iv = from.GetInvariantI64();
						double dv = 0.0;
						memcpy((char*)&dv, (const char*)&iv, sizeof(std::int64_t));
						return Specialization(ConstantNil::Get(), Type::InvariantI64(iv));
					}
					if (from.IsInt32()) {
                        dstTy = Type::Float32;
					} else if (from.IsInt64()) {
                        dstTy = Type::Float64;
                    } else {
                        t.GetRep().Diagnostic(LogErrors, this, Error::SpecializationFailed, "Bitcast source value is not a floating point value");
                        return SpecializationFailure();
                    }
                }
                return Specialization(BitCast::New(dstTy, vectorWidth, a.node),
                                      vectorWidth > 1 ? Type::Vector(dstTy, vectorWidth) : dstTy);

			}

			Specialization GenericFFI::Specialize(SpecializationTransform& t) const {
				SPECIALIZE(t, a, GetUp(0));
				Type at = a.result.Fix();
				CTRef ag = a.node;
				if (at.IsPair() == false || at.First().IsInvariantString() == false) {
					t.GetRep().Diagnostic(LogErrors, this, Error::BadInput, "Foreign function call requires at least a return type and a name");
					return t.GetRep().TypeError("Foreign-Function", at);
				}
				auto rv = at.First();
				at = at.Rest();
				ag = ag->GraphRest();
				if (at.IsPair() == false || at.First().IsInvariantString() == false) {
					t.GetRep().Diagnostic(LogErrors, this, Error::BadInput, "Foreign function call requires at least a return type as a C type attribute string and a symbol name");
					return t.GetRep().TypeError("Foreign-Function", at);
				}

				auto ffi = ForeignFunction::New(*rv.GetString(), *at.First().GetString());
				at = at.Rest();
				ag = ag->GraphRest();
				while (at.IsPair()) {
					Type att = at.First();
					at = at.Rest();
					ag = ag->GraphRest();
					Type val; CTRef valg;
					if (at.IsPair()) {
						val = at.First();
						valg = ag->GraphFirst();
						at = at.Rest();
						ag = ag->GraphRest();
					} else {
						val = at;
						valg = ag;
					}
					if (att.IsInvariantString() == false) {
						t.GetRep().Diagnostic(LogErrors, this, Error::BadInput, "Foreign function call must provide C type attributes as strings.");
						return t.GetRep().TypeError("Foreign-Function", att);
					}
					if (ffi->AddParameter(*att.GetString(), valg, val) == false) {
						t.GetRep().Diagnostic(LogErrors, this, Error::BadInput, att, val, "Foreign function call: Can't marshal.");
						return t.GetRep().TypeError("Foreign-Function", att);
					}
				}
				if (at.IsNil() == false) {
					t.GetRep().Diagnostic(LogErrors, this, Error::BadInput, at, "Foreign function call argument list must be nil-terminated");
					return t.GetRep().TypeError("Foreign-Function", at);
				}
				return Specialization(ffi, ffi->FixedResult());
			}

			Type ForeignFunction::FixedResult() const {
				if (compilerNode) {
					return ReturnValue;
				}
				Type rv = Type::Nil;
				auto kti = KTypes.rbegin();
				for (auto pri = CTypes.rbegin();pri != CTypes.rend();++pri, ++kti) {
					bool out(false), ptr(false);
					CTypeToKronosType(*pri, out, ptr);
					if (out && ptr) rv = Type::Pair(*kti, rv);
				}
				return Type::Pair(ReturnValue, rv);
			}

			void ForeignFunction::Output(std::ostream& s) const {
				s << Symbol;
			}

			Type ForeignFunction::CTypeToKronosType(const std::string& cty, bool &isOutput, bool &isPointer) {
				auto str = cty;
				isOutput = false;
				isPointer = false;
				if (str.size()) {
					if (str.substr(0, 6) == "const ") {
						isOutput = false;
						str = str.substr(6);
					} else isOutput = true;

					if (str.back() == '*') {
						isPointer = true;
						str.pop_back();
					} else isOutput = isPointer = false;

					if (str == "float") return Type::Float32;
					if (str == "double") return Type::Float64;
					if (str == "int32") return Type::Int32;
					if (str == "int64") return Type::Int64;
				}
				return Type::Nil;
			}

			int ForeignFunction::LocalCompare(const ImmutableNode& rhsi) const {
				auto& rhs = (ForeignFunction&)rhsi;
				int cmp;
				if ((cmp = ordinalCmp(Symbol, rhs.Symbol))) return cmp;
				if ((cmp = ordinalCmp(ReturnValue, rhs.ReturnValue))) return cmp;
				if ((cmp = ordinalCmp(KTypes.size(), rhs.KTypes.size()))) return cmp;
				if ((cmp = ordinalCmp(CTypes.size(), rhs.CTypes.size()))) return cmp;
				if ((cmp = ordinalCmp(compilerNode, rhs.compilerNode))) return cmp;
				for (int i = 0;i < KTypes.size();++i) {
					if ((cmp = ordinalCmp(KTypes[i], rhs.KTypes[i]))) return cmp;
				}

				for (int i = 0;i < CTypes.size();++i) {
					if ((cmp = ordinalCmp(CTypes[i], rhs.CTypes[i]))) return cmp;
				}

				return TypedPolyadic::LocalCompare(rhsi);
			}

			unsigned ForeignFunction::ComputeLocalHash() const {
				auto h = TypedPolyadic::ComputeLocalHash();
				HASHER(h, ReturnValue.GetHash());
				HASHER(h, std::hash<std::string>()(Symbol));
				return h;
			}

			bool IsArrayOfType(Type scalarType, Type arrayType) {
				if (arrayType.IsPair()) {
					if (arrayType.First() != scalarType &&
						IsArrayOfType(scalarType, arrayType.First()) == false) return false;
					// is homogenous?
					size_t um = arrayType.CountLeadingElements(arrayType.First());
					auto le = arrayType.Rest(um);
					if (le.IsNil() == false && le != arrayType.First()) return false;
					return true;
				} else return scalarType == arrayType;
			}

			bool ForeignFunction::AddParameter(const std::string& cty, CTRef source, Type kty) {
				if(cty == "sizeof" || cty == "typeof" || cty == "void*" || cty == "const void*") {
				} else if(cty == "const char*") {
					if(!kty.IsInvariantString()) return false;
				} else {
					bool out, ptr;
					auto mty = CTypeToKronosType(cty, out, ptr);
					if (mty.IsNil()) return false;
					// typecheck that marshaling will succeed

					if (kty != mty) {
						if (IsArrayOfType(mty, kty)) {
							if (ptr == false) return false;
						} else return false;
					}
				}

				CTypes.push_back(cty);
				KTypes.push_back(kty);
				Connect((kty.GetSize() > 0 || source->GetNumCons() == 0) ? source : Typed::Nil());
				return true;
			}

			Specialization GenericSelf::Specialize(SpecializationState& st) const {
				return { SelfID::New(), Type::Int64 };
			}
		};
	};

	std::string RemoveUnderscores(std::string input) {
		for (auto &c : input) if (c == '_') c = '-';
		return input;
	}

#define AddNativeNode(OPCODE,FUNC,ARGLIST,COMMENT,FALLBACK,...) pack.AddFunction(RemoveUnderscores(#OPCODE).c_str(),Nodes::Native::Make<OPCODE>(#FUNC,FUNC<float>,FUNC<double>,FUNC<int32_t>,FUNC<int64_t>,FUNC<BigNum>,__VA_ARGS__),ARGLIST,COMMENT,FALLBACK)

#define NATIVE_BUILDER_BIN(OPCODE,FUNCTION,COMMENT) AddNativeNode(OPCODE,FUNCTION,"a b",COMMENT,":Fallback:Binary-Op",b1,b2);
#define NATIVE_BUILDER_BINF(OPCODE,FUNCTION,COMMENT,FALLBACK) AddNativeNode(OPCODE,FUNCTION,"a b",COMMENT,FALLBACK,b1,b2);
#define NATIVE_BUILDER_UN(OPCODE,FUNCTION,COMMENT) AddNativeNode(OPCODE,FUNCTION,"a",COMMENT,":Fallback:Unary-Op",arg);
#define NATIVE_UNWRAP(x) NATIVE_BUILDER_ ## x

	template <typename T> T T_SQRT(T a) { return _SQRT(a); }
	template <typename T> T T_FLOOR(T a) { return _FLOOR(a); }
	template <typename T> T T_CEIL(T a) { return _CEIL(a); }
	template <typename T> T T_ROUND(T a) { return _ROUND(a); }
	template <typename T> T T_TRUNC(T a) { return _TRUNC(a); }
	template <typename T> T T_RCPA(T a) { return _RCPA(a); }
	template <typename T> T T_RSQRTA(T a) { return _RSQRTA(a); }

	void BuildNativeMathFuncs(Parser::RepositoryBuilder pack) {
		using namespace Nodes;
		using namespace Nodes::Native;
		auto arg = GenericArgument::New();
		auto b1 = GenericFirst::New(arg);
		auto b2 = GenericRest::New(arg);

		META_MAP(NATIVE_UNWRAP,
				 UN(Cos, _COS, "Take cosine of an angle in radians"),
				 UN(Sin, _SIN, "Take sine of an angle in radians"),
				 UN(Exp, _EXP, "Compute exponential function"),
				 UN(Log, _LOG, "Compute the natural logarithm"),
				 UN(Log10, _LOG10, "Compute the base 10 logarithm"),
				 //UN(Log2,_LOG2,"Compute the base 2 logartihm"),
				 UN(Sqrt, T_SQRT, "Takes the square root of a floating point number"),

				 BIN(Pow, _POW, "Compute a binary power function"),
				 BIN(Atan2, _ATAN2, "Compute the arcus tangent of (y x)")
				 )
	}

	void BuildNativePrimitiveOps(Parser::RepositoryBuilder pack) {
		using namespace Nodes;
		using namespace Nodes::Native;
		auto arg = GenericArgument::New();
		auto b1 = GenericFirst::New(arg);
		auto b2 = GenericRest::New(arg);

		META_MAP(NATIVE_UNWRAP,
				 BIN(Add, _ADD, "Adds two numbers of matching type"),
				 BIN(Mul, _MUL, "Multiplies two numbers of matching type"),
				 BIN(Sub, _SUB, "Substracts two numbers of matching type"),
				 BIN(Div, _DIV, "Divides two numbers of matching type"),
				 BIN(Modulo, _MODULO, "Implements modulo arithmetic on integers"),
				 BIN(ClampIndex, _CLAMPINDEX, "Clamps index to 0 if it is negative or greater or equal to the right hand side"),
				 BIN(Max, _MAX, "Returns the greater of two numbers of matching type"),
				 BIN(Min, _MIN, "Returns the lesser of two numbers of matching type"),
				 BIN(Equal, _CMPEQ, "Tests two numbers of matching type for equality, returning a bit mask"),
				 BIN(Not_Equal, _CMPNEQ, "Tests two numbers of matching type for nonequality, returning a bit mask"),
				 BIN(Greater, _CMPGT, "Tests if a number is greater than another, returning a bit mask"),
				 BIN(Greater_Equal, _CMPGE, "Tests if a number is greater or equal to another, returning a bit mask"),
				 BIN(Less, _CMPLT, "Tests if a number is less than another, returning a bit mask"),
				 BIN(Less_Equal, _CMPLE, "Tests if a number is less or equal to another, returning a bit mask"),
				 BIN(And, _AND, "Performs a bitwise and on two numbers of matching type"),
				 BIN(AndNot, _ANDNOT, "Performs a bitwise and two numbers, inverting the left hand side"),
				 BIN(Or, _OR, "Performs a bitwise or on two numbers of matching type"),
				 BIN(Xor, _XOR, "Performs a bitwise exclusive or on two numbers of matching type"),

				 UN(Neg, _NEG, "Inverts the sign of a number"),
				 UN(Not, _NOT, "Inverts all the bits of a number or a bitmask"),
				 UN(Abs, _ABS, "Takes the absolute value of a number"),

				 UN(Floor, T_FLOOR, "Returns the largest integer that is less or equal to a number"),
				 UN(Ceil, T_CEIL, "Returns the smallest integer that is greater or equal to a number"),
				 UN(Round, T_ROUND, "Returns the nearest integer to a number"),
				 UN(Truncate, T_TRUNC, "Discards any fractional decimals from a number")
				 );

		pack.AddMacro("Float", GenericTypeTag::New(Type::Float32.TypeOf().GetDescriptor()), false);
		pack.AddMacro("Double", GenericTypeTag::New(Type::Float64.TypeOf().GetDescriptor()), false);
		pack.AddMacro("Int32", GenericTypeTag::New(Type::Int32.TypeOf().GetDescriptor()), false);
		pack.AddMacro("Int64", GenericTypeTag::New(Type::Int64.TypeOf().GetDescriptor()), false);
		pack.AddMacro("Exception", GenericTypeTag::New(&UserException), false);

		pack.AddMacro("Vector", GenericTypeTag::New(&VectorTag), false);

		pack.AddFunction("Cvt-Int32", Convert::New(Convert::Int32, arg), "a", "");
		pack.AddFunction("Cvt-Int64", Convert::New(Convert::Int64, arg), "a", "");
		pack.AddFunction("Cvt-Float", Convert::New(Convert::Float32, arg), "a", "");
		pack.AddFunction("Cvt-Double", Convert::New(Convert::Float64, arg), "a", "");

		pack.AddFunction("Cast-Int", GenericBitcast::New(arg, false), "a", "Reinterpret a floating point bitset as an integer or similar bit width", ":Fallback:Unary-Op");
		pack.AddFunction("Cast-Float", GenericBitcast::New(arg, true), "a", "Reinterpret an integer bitset as a floating point value of similar bit width", ":Fallback:Unary-Op");

		pack.AddFunction("BitShiftRight", Make<BitShiftRight>("BitShiftRight", nullptr, nullptr, [](int32_t a, int32_t b) {return a >> b; }, [](int64_t a, int64_t b) {return a >> b; }, nullptr, b1, b2), "a b", "Shifts the integer value 'a' right by 'b' bits");
		pack.AddFunction("LogicalShiftRight", Make<LogicalShiftRight>("LogicalShiftRight", nullptr, nullptr, [](int32_t a, int32_t b) { return (int32_t)((uint32_t)a >> b); }, [](int64_t a, int64_t b) { return (int64_t)((uint64_t)a >> b); }, nullptr, b1, b2), "a b", "Shifts the integer value 'a' right by 'b' bits including the sign bit.");
		pack.AddFunction("BitShiftLeft", Make<BitShiftLeft>("BitShiftLeft", nullptr, nullptr, [](int32_t a, int32_t b) {return a >> b; }, [](int64_t a, int64_t b) {return a << b; }, nullptr, b1, b2), "a b", "Shifts the integer value 'a' left by 'b' bits");

		Make<Native::Abs>("abs", _ABS<float>, _ABS<double>, _ABS<std::int32_t>, _ABS<std::int64_t>, _ABS<BigNum>, arg);

		BuildNativeMathFuncs(pack.AddPackage("Math"));

		auto fb(pack.AddPackage("Fallback"));
		static TypeDescriptor noOverload("Type mismatch", false);
		fb.AddMacro("No-Overload", GenericTypeTag::New(&noOverload), false);
		fb.AddFunction("No-Match", Raise::New(GenericMake::New(
			GenericTypeTag::New(&noOverload),
				GenericPair::New(
					Invariant::Constant::New(Type("Could not find a valid form of '")),
					GenericPair::New(
					b1,
						GenericPair::New(
							Invariant::Constant::New(Type("' for arguments of type '")),
							GenericPair::New(
								b2,
								Invariant::Constant::New(Type("'")))))), 
			true)));
		fb.AddFunction("Binary-Op", Evaluate::CallLib(":Fallback:No-Match", b2));
		fb.AddFunction("Unary-Op", Evaluate::CallLib(":Fallback:No-Match", b2));

		pack.AddFunction("Foreign-Function", GenericFFI::New(arg), "return-type symbol parameters...", "");

		pack.AddFunction("Self-ID", GenericSelf::New());

	}
};
