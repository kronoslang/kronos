#pragma once

#include "NodeBases.h"
#include "TypeAlgebra.h"

#define EXPAND_KRONOS_NATIVE_OPCODES \
F(Add) \
F(Mul) \
F(Sub) \
F(Div) \
F(Equal) \
F(Not_Equal) \
F(Greater) \
F(Greater_Equal) \
F(Less) \
F(Less_Equal) \
F(And) \
F(AndNot) \
F(Or) \
F(Xor) \
F(BuildForVariantsOnly) \
F(Max) \
F(Min) \
F(BitShiftLeft) \
F(BitShiftRight) \
F(LogicalShiftRight) \
F(Neg) \
F(Abs) \
F(Not) \
F(ToInt32) \
F(ToInt64) \
F(ToFloat32) \
F(ToFloat64) \
F(ConstantLeaf) \
F(TernarySelect) \
F(FunctionCall) \
F(TailCall) \
F(Delay) \
F(DefinedForAllTypes) \
F(FunctionIntrinsics) \
F(Round) \
F(Truncate) \
F(Ceil) \
F(Floor) \
F(Cos) \
F(Sin) \
F(Exp) \
F(Log) \
F(Log10) \
F(Log2) \
F(Pow) \
F(Atan2) \
F(Sqrt) \
F(DefinedForFloatTypes) \
F(DefinedForFloat32) \
F(Modulo) \
F(ClampIndex) \
F(DefinedForIntegerTypes) \
F(Nop) \
F(NumOperands)

namespace K3 {
	namespace Nodes {
		namespace Native {
			enum Opcode {
#define F(OP) OP,
				EXPAND_KRONOS_NATIVE_OPCODES
#undef F
			};

			class IOpcodeNode : REFLECTING_CLASS
			{
			public:
				virtual Opcode GetOpcode() const = 0;
			};

			class IGenericBinary : public GenericBinary, public IOpcodeNode
			{
				INHERIT(IGenericBinary,GenericBinary,IOpcodeNode);
			public:
				IGenericBinary(CGRef l, CGRef r):GenericBinary(l,r){}
			};

			class ITypedBinary : public TypedBinary, public IOpcodeNode, public IFixedResultType
			{
				INHERIT(ITypedBinary,TypedBinary,IFixedResultType);
			public:
				CODEGEN_EMITTER
				ITypedBinary(CTRef l, CTRef r):TypedBinary(l,r){}
				Type Result(ResultTypeTransform&) const  override {return FixedResult();}
				int SchedulingPriority() const  override {return 1;}
				virtual int GetWeight() const  override { return 2; }
			};

			class IGenericUnary : public GenericUnary, public IOpcodeNode
			{
				INHERIT(IGenericUnary,GenericUnary,IOpcodeNode);
			public:
				IGenericUnary(CGRef up):GenericUnary(up){}
			};

			class ITypedUnary : public TypedUnary, public IOpcodeNode, public IFixedResultType
			{
				INHERIT(ITypedUnary,TypedUnary,IFixedResultType);
			public:
				CODEGEN_EMITTER
				ITypedUnary(CTRef up):TypedUnary(up){}
				Type Result(ResultTypeTransform&) const  override {return FixedResult();}
				virtual int GetWeight() const  override { return 2; }
			};

			TYPED_NODE(Constant,DisposableTypedLeaf, IFixedResultType, IPairSimplifyFirst)
				void *memory;
				size_t len;
				const Type type;
#ifndef NDEBUG
				std::string readableLabel;
#endif
protected:
				Constant(const void *mem, size_t len, const Type& t);
				int LocalCompare(const ImmutableNode& rhs) const override;
				unsigned ComputeLocalHash() const override;
			public:
				CODEGEN_EMITTER
				Constant(const Constant& src);
				~Constant();
				static Constant *New(float datum);
				static Constant *New(double datum);
				static Constant *New(int32_t datum);
				static Constant *New(int64_t datum);
				static Constant *New(const Type& type, const void* mem);
				Type Result(ResultTypeTransform&) const override { return type; }
				Type FixedResult() const override { return type; }
				CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
				void Output(std::ostream& strm) const override;
				CTRef GraphFirst() const override;
				CTRef GraphRest() const override;
				int SchedulingPriority() const override { return -1; }
				int64_t AsInteger() const;
				void *GetPointer() { return memory; }
				const void* GetPointer() const { return memory; }
				CTRef PairWithRest(CTRef rst) const override;
				virtual int GetWeight() const  override { return 2; }
#ifndef NDEBUG
				const char* GetLabel() const override {
					return readableLabel.c_str();
				}
#endif
			END

			class ConstantNil : public Constant {
				INHERIT_RENAME(ConstantNil, Constant);
				REGION_ALLOC(ConstantNil);
				const char *PrettyName() const override { return "Nil"; }
				ConstantNil(const ConstantNil&) = delete;
				virtual Constant *ConstructShallowCopy() const override;
			public:
				ConstantNil() : Constant(nullptr, 0, Type::Nil) {}
				static ConstantNil* Get();
				CTRef GraphFirst() const override;
				CTRef GraphRest() const override;
			};

			GENERIC_NODE(GenericBitcast, GenericUnary)
				bool fromInt;
				GenericBitcast(CGRef u, bool fi) :GenericUnary(u), fromInt(fi) { }
			public:
				static GenericBitcast* New(CGRef up, bool fromInt) { return new GenericBitcast(up, fromInt); }
			END

			TYPED_NODE(Select,TypedTernary,IOpcodeNode)
				Select(CTRef c, CTRef t, CTRef f):TypedTernary(c,t,f) {} 
			PUBLIC
				CODEGEN_EMITTER
				static Select* New(CTRef condition, CTRef whenTrue, CTRef whenFalse) {return new Select(condition,whenTrue,whenFalse);}
				Type Result(ResultTypeTransform& rt) const override {return rt(GetUp(1));}
				Opcode GetOpcode() const override {return TernarySelect;}
				virtual int GetWeight() const override { return 2; }
			END

			GENERIC_NODE(GenericFFI,GenericUnary)
				GenericFFI(CGRef args) :GenericUnary(args) {}
			PUBLIC
				static GenericFFI *New(CGRef args) { return new GenericFFI(args); }
			END

			TYPED_NODE(ForeignFunction,TypedPolyadic,IFixedResultType)
				ForeignFunction(const std::string& sym, const std::string& rv, bool compilerNode) : TypedPolyadic(), Symbol(sym), compilerNode(compilerNode) {
				bool out, ptr; ReturnValue = CTypeToKronosType(rv,out,ptr);
				}
				Type ReturnValue;
				std::string Symbol;
				std::vector<std::string> CTypes;
				std::vector<Type> KTypes;
				bool compilerNode;
				static Type CTypeToKronosType(const std::string&, bool& isOutputParameter, bool& isPointer);
				int LocalCompare(const ImmutableNode& rhs) const override;
				unsigned ComputeLocalHash() const override;
			PUBLIC
				CODEGEN_EMITTER
				Type Result(ResultTypeTransform&) const  override { return FixedResult(); }
				Type FixedResult() const override;
				static ForeignFunction* New(const std::string& returnValue, const std::string& sym, bool compilerNode = false) { return new ForeignFunction(sym, returnValue, compilerNode); }
				bool AddParameter(const std::string& ctype, CTRef source, Type ktype);
				CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
				virtual int GetWeight() const override { return 5; }
				void Output(std::ostream& strm) const override;
			END

			GENERIC_NODE(GenericSelf, GenericLeaf)
			PUBLIC
				static GenericSelf* New() { return new GenericSelf; }
			END

			TYPED_NODE(SelfID, TypedLeaf, IFixedResultType)
			PUBLIC
				static SelfID* New() { return new SelfID; }
				Type Result(ResultTypeTransform&) const override { return FixedResult(); }
				Type FixedResult() const override { return Type::Int64; }
				CTRef SideEffects(Backends::SideEffectTransform& sfx) const override;
			END

			Typed* MakeInt64(const char *label, int opcode, CTRef a, CTRef b);
			Typed* MakeFloat(const char *label, int opcode, CTRef a, CTRef b);
			Typed* MakeInt64(const char *label, int opcode, CTRef a);
			Typed* MakeFloat(const char *label, int opcode, CTRef a);
			Typed* MakeInt32(const char *label, int opcode, CTRef a, CTRef b);
		};

		TYPED_NODE(BitCast, DisposableTypedUnary, IFixedResultType)
			Type to;
            int vectorWidth;
			BitCast(const Type& t, int w, CTRef up) :to(t), vectorWidth(w), DisposableTypedUnary(up) { }
		PUBLIC
			DEFAULT_LOCAL_COMPARE(DisposableTypedUnary, to, vectorWidth);
			CODEGEN_EMITTER
			static BitCast* New(const Type& to, int w, CTRef up) { return new BitCast(to, w, up); }
			Type Result(ResultTypeTransform& rt) const override { return FixedResult(); }
			Type FixedResult() const override {
                return vectorWidth > 1 ? Type::Vector(to, vectorWidth) : to;
                
            }
		END
	};

	class Package;
	void BuildNativePrimitiveOps(Package pack);
};

