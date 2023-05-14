#pragma once

#include "common/ssstring.h"
#include <ostream>
#include <cstdint>
#include <vector>
#include <initializer_list>
#include <numeric>

#pragma warning(disable:4244)
#include "ttmath/ttmath.h"
#pragma warning(default:4244)

#include "common/Hasher.h"


template <typename T> int ordinalCmp(const T& a, const T&b) { if (a < b) return -1; if (b < a) return 1; return 0; }

namespace K3 {

	namespace Nodes {
		class Generic;
		using CGRef = const Generic*;
	};

	class TypeRuleGenerator;
	class TupleData;
	class UnionData;
	struct InvariantData;

	class TypeDescriptor {
		std::string name;
		bool breakable;
		int printLimit;
	public:
		TypeDescriptor(const std::string& n, bool breakable = true, int printLimit = -1) :name(n), breakable(breakable), printLimit(printLimit) {}
		bool IsBreakable() { return breakable; }
		int GetPrintLimit() { return printLimit; }
		const std::string& GetName() const { return name; }
	};

	class Type;
	template <typename RESULT> RESULT ExtractValue(const Type& t);
	template <typename RESULT> bool IsOfType(const Type& t);

	class Type {
		friend class TupleData;
		friend class UnionData;
		friend class TypeRuleGenerator;
		friend class TypeRuleSet;
        friend struct AxiomRule;
		enum Kind : int8_t {
			LowSentinel = -10,
			ArrayViewType = -9,
			InternalRefcountType = -8,
			InvariantStringType = -7,
			UnionType = -6,
			TupleType = -5,
			UserType = -4,
			InvariantGraphType = -3,
			InvariantType = -2,
			RuleGeneratorType = -1,
			NilType = 0,
			TrueType,
			TypeTagType,
			Float32Type,
			Float64Type,
			Int32Type,
			Int64Type,
			VectorType,
			Moved,
			InternalUsageType,
			TypeErrorType,
			HighSentinel
		};


		union {
			InvariantData* InvariantValue;
			const SString *InvariantString;
			struct { TupleData *Data; size_t fstArity; } Tuple;
			struct { RefCounted<Type> *Content; TypeDescriptor *tag; } UserType;
			RefCounted<Type> *ArrayElement;
			const TypeRuleGenerator *RGen;
			TypeDescriptor *TypeTag;
			UnionData *Union;
			const void* internalUsage;
			struct { Kind element; uint16_t size; } Vector;
			RefCounting *RefObj;
		} data;

		Kind kind;

		Type(Kind element, uint32_t vectorSize);
		Type(RefCounted<Type> *Element, size_t Count, bool hasNil);
		Type(Kind typeTag);

		Type(const Type& content, TypeDescriptor *tag);
		Type(const Type& fst, const Type& rst, size_t repeatFirst);
		Type(UnionData*);
	public:
		static const char *TypeTagNames[];
		/* size assumptions */
		static const size_t SizeOfFloat32 = 4;
		static const size_t SizeOfFloat64 = 8;
		static const size_t SizeOfInt32 = 4;
		static const size_t SizeOfInt64 = 8;

		/* constructors */
		explicit Type(bool TrueOrNil = false);
		explicit Type(const SString *string);
		explicit Type(double invariant);
		explicit Type(std::int64_t invariant);
		explicit Type(Nodes::CGRef graph);
		explicit Type(TypeDescriptor *tag);
		explicit Type(const char *string);
		explicit Type(const void *internalUsage);
		explicit Type(RefCounting* unknownRefCounted);
		explicit Type(ttmath::Big<1, 2>);
		explicit Type(int v) :Type((std::int64_t) v) { }

		/* internal types used in recurrence reasoning */
		explicit Type(const TypeRuleGenerator *ruleGen);

		/* named ctor idiom */
		static Type InternalUse(const void *usage) { return Type(usage); }
		static Type InvariantLD(double value) { return Type(value); }
		static Type InvariantI64(int64_t value);
		static Type InvariantU64(size_t value);
		static Type InvariantString(const SString *text) { return Type(text); }
		static Type InvariantGraph(Nodes::CGRef node) { return Type(node); }
		static Type User(TypeDescriptor *tag, const Type& content) { return Type(content, tag); }
		static Type Union(Type maybeUnion, Type additionalSubtype, bool mergeIntoUnion);

		static Type Tuple(const Type& element, size_t repeatCount);
		static Type List(const Type& element, size_t repeatCount);
		static Type Chain(const Type& element, size_t repeat, const Type& trailing);
		static Type Vector(const Type& element, unsigned size);
		static Type Append(const Type& head, const Type& tail);
		static Type BigNumber(const char *);
		static Type ArrayView(const Type& element);
		Type Element(size_t index) const;

		static Type TypeError(TypeDescriptor *desc);
		static Type TypeError(TypeDescriptor *desc, Type content);

		/* Cxx interop ctors */
		template <typename T> static Type FromNative(T tmp = 0);
		template <typename ELEM, size_t ARITY> static Type FromNative(const ELEM(&c_array)[ARITY]) {
			return Tuple(FromNative(c_array[0]), ARITY);
		}
		template <typename T> static Type VectorFromNative(uint16_t width) { return width > 1 ? Vector(FromNative<T>(), width) : FromNative<T>(); }

		/* Assignment and copy */
		Type& operator=(Type&& src);
		Type& operator=(const Type& src) { *this = Type(src); return *this; }

		Type(const Type& src);
		Type(Type&& src);
		~Type();

		/* type data accessors */
		bool IsKind(Kind type) const;

		bool IsPair() const;
		bool IsRuleGenerator() const { return kind == RuleGeneratorType; }
		bool IsInvariant() const { return IsKind(InvariantType); }
		bool IsInvariantString() const { return IsKind(InvariantStringType); }
		bool IsTrue() const { return IsKind(TrueType); }
		bool IsNil() const { return IsKind(NilType); }
		bool IsGraph() const { return IsKind(InvariantGraphType); }
		bool IsTypeTag() const { return IsKind(TypeTagType); }
		bool IsUserType() const;
		bool IsUserType(const TypeDescriptor& td) const;
		bool IsFloat32() const { return IsKind(Float32Type); }
		bool IsFloat64() const { return IsKind(Float64Type); }
		bool IsInt32() const { return IsKind(Int32Type); }
		bool IsInt64() const { return IsKind(Int64Type); }
		bool IsNativeVector() const { return IsKind(VectorType); }
		bool IsNativeType() const { return IsFloat32() || IsFloat64() || IsInt32() || IsInt64() || IsNativeVector(); }
		bool IsTuple() const { return kind == TupleType; }
		bool IsNilTerminated() const;
		bool IsInternalTag() const { return kind == InternalUsageType; }
		bool IsUnion() const { return kind == UnionType; }
		bool IsArrayView() const { return kind == ArrayViewType; }

		bool IsInLocalScope() const;

		Type Arity() const;
		size_t CountLeadingElements(const Type& ofType) const;

		Type TypeOf() const;
		Type First() const;
		Type Rest(size_t order = 1) const;
		Type UnwrapUserType() const;// {assert(IsUserType());return *data.UserType.Content;}

		// use care when not generating rules!
		enum TypeFixingRules {
			GenerateNoRules,
			GenerateRulesForSizedTypes,
			GenerateRulesForAllTypes
		};
		Type Fix(TypeFixingRules generateRules = GenerateRulesForAllTypes) const;
		bool IsFixed() const;

		const void *GetInternalTag() const { return data.internalUsage; }
		Counter *GetUnknownManagedObject() const { return data.RefObj; }
		double GetInvariant() const;
		std::int64_t GetInvariantI64() const;
		ttmath::Big<1, 2> GetBigNum() const; 
		bool GetTrueOrNil() const;
		CRef<SString> GetString() const;
		TypeDescriptor* GetDescriptor() const;
		Nodes::CGRef GetGraph() const;
		uint16_t GetVectorWidth() const;
		Type GetVectorElement() const;
		int GetNumUnionTypes() const;
		Type GetUnionType(int) const;
		Type GetArrayViewElement() const;
		const TypeRuleGenerator* GetRuleGenerator() const { return data.RGen; }

		static TypeDescriptor* TagForKind(int kind);

		/* abstract invariant operations for type rule lifting */
		Type operator+(const Type& rhs) const;
		Type operator-(const Type& rhs) const;

		/* Algebraic operators */
		static Type First(const Type& pair) { return pair.First(); }
		static Type Rest(const Type& pair) { return pair.Rest(); }
		static Type Pair(const Type& fst, const Type& rst, bool disableRuleGenExtension = false);

		static Type Tuple(const Type& e1, const Type& e2) { return Pair(e1, e2); }
		static Type Tuple(const Type& e1, const Type& e2, const Type& e3) { return Pair(e1, Tuple(e2, e3)); }
		static Type Tuple(const Type& e1, const Type& e2, const Type& e3, const Type& e4) { return Pair(e1, Tuple(e2, e3, e4)); }
		static Type Tuple(const Type& e1, const Type& e2, const Type& e3, const Type& e4, const Type& e5) { return Pair(e1, Tuple(e2, e3, e4, e5)); }
		static Type Tuple(const Type& e1, const Type& e2, const Type& e3, const Type& e4, const Type& e5, const Type& e6) { return Pair(e1, Tuple(e2, e3, e4, e5, e6)); }

		bool Tie() const;
		template <typename ARG, typename... ARGS> bool Tie(ARG& arg, ARGS&... args) const {
			if (IsOfType<ARG>(Element(0))) {
				arg = ExtractValue<ARG>(Element(0));
				return Rest().Tie(args...);
			} else return false;
		}

		/* constant */
		static const Type Nil;
		static const Type True;
		static const Type Float32;
		static const Type Float64;
		static const Type Int32;
		static const Type Int64;

		/* elementary type facilities */
		size_t GetHash() const;
		size_t GetSize() const;
		bool operator==(const Type& rhs) const;
		bool operator!=(const Type& rhs) const { return !operator==(rhs); }
		bool operator<(const Type& rhs) const { return OrdinalCompare(rhs) < 0; }
		bool operator>(const Type& rhs) const { return OrdinalCompare(rhs) > 0; }
		bool operator<=(const Type& rhs) const { return !operator>(rhs); }
		bool operator>=(const Type& rhs) const { return !operator<(rhs); }
		int OrdinalCompare(const Type& rhs) const;
		void OutputText(std::ostream& stream, const void *instance = nullptr, bool nestedPair = false) const;
		void OutputFormatString(std::ostream& stream, bool nestedPair = false) const;
		void OutputJSONTemplate(std::ostream& stream, bool nestedPair = false) const;

	private:
		int OrdinalCompare(const Type& rhs, bool generateRules) const;
		bool IsKind(Kind kind, bool generateRules) const;
	};

	class TupleData : public RefCounting {
	public:
		Type fst, rst;
		size_t cachedSize;
		size_t cachedHash;
		bool cachedFixed, cachedNoReturn;
		TupleData(Type f, Type r) :fst(std::move(f)), rst(std::move(r)) {
			cachedHash = 1337;
			HASHER(cachedHash, fst.GetHash());
			HASHER(cachedHash, rst.GetHash());
			cachedSize = fst.GetSize() + rst.GetSize();
			cachedFixed = fst.IsFixed() && rst.IsFixed();
			cachedNoReturn = fst.IsInLocalScope() || rst.IsInLocalScope();
		}
	};

	class UnionData : public RefCounting {
	public:
		std::vector<Type> subTypes;
		size_t cachedSize;
		size_t cachedHash;
		void Final() {
			cachedHash = 1338;
			cachedSize = 0; 
			for (auto& t : subTypes) {
				HASHER(cachedHash, t.GetHash());
				cachedSize = t.GetSize() > cachedSize ? t.GetSize() : cachedSize; 
			}
		}
	};

	struct InvariantData : public RefCounting {
	public:
		ttmath::Big<1, 2> value;
#ifndef NDEBUG
		double vis;
#endif
	};

	extern TypeDescriptor SpecializationFailure;
	extern TypeDescriptor UserException;
	extern TypeDescriptor FatalFailure;
	extern TypeDescriptor RecursionTrap;
	extern TypeDescriptor PropagateFailure;
	extern TypeDescriptor NoEvalFallback;
	extern TypeDescriptor MonitoredError;
	extern TypeDescriptor FunctionTag;
	extern TypeDescriptor ReactiveRateTag;

	extern TypeDescriptor Float32Tag;
	extern TypeDescriptor Float64Tag;
	extern TypeDescriptor Int32Tag;
	extern TypeDescriptor Int64Tag;
	extern TypeDescriptor VectorTag;
	extern TypeDescriptor PairTag;
	extern TypeDescriptor VectorTag;
	extern TypeDescriptor TypeTagTag;
	extern TypeDescriptor TrueTag;
	extern TypeDescriptor NilTag;
	extern TypeDescriptor GraphTag;
	extern TypeDescriptor InvariantTag;
	extern TypeDescriptor InvariantStringTag;
	extern TypeDescriptor UserTypeTag;
	extern TypeDescriptor UnionTag;
	extern TypeDescriptor AudioFileTag; // ("Audio File");
};


namespace std {
	template <> struct hash<K3::Type> { size_t operator()(const K3::Type& t) const { return t.GetHash(); } };
	template <> struct hash<const K3::Type> { size_t operator()(const K3::Type& t) const { return t.GetHash(); } };
	static ostream& operator<<(ostream& out, const K3::Type &t) { t.OutputText(out); return out; }
};
