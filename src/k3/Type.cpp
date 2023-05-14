#include "lithe/grammar/json.h"
#include "NodeBases.h"
#include "Graph.h"
#include "TypeRuleGenerator.h"
#include "Errors.h"
#include <string>
#include <limits>
#include <sstream>
#include <cmath>
#include "TLS.h"
#include "math.h"

#define InternalError(msg) assert(0 && msg)

//#define typecheck_validate(condition)
//#define typecheck_validate(condition) assert(condition)
#define ASSERT_KIND(type) assert(IsKind(type ## Type,false))
#define ASSERT_KIND2(type1,type2) assert(IsKind(type1 ## Type,false) || IsKind(type2 ## Type,false))
#define ASSERT_KIND3(type1,type2,type3) assert(IsKind(type1 ## Type,false) || IsKind(type2 ## Type,false) || IsKind(type3 ## Type,false))


namespace K3 {

	const char *Type::TypeTagNames[] = {
		"InvariantString",
		"Union",
		"Tuple",
		"User",
		"TypeTag",
		"InvariantGraph",
		"RuleGenerator",
		"Invariant",
		"Float32",
		"Float64",
		"Int32",
		"Int64",
		"True",
		"Nil",
		"Moved",
		"TypeError"
	};

	Type::~Type() {
		if (kind < 0) data.RefObj->Detach();
	}

	// move ctor
	Type::Type(Type&& src) {
		kind = src.kind;
		data = src.data;
		src.kind = Moved;
	}

	Type& Type::operator=(Type&& src) {
		std::swap(src.data, data);
		std::swap(src.kind, kind);
		return *this;
	}

	// copy ctor
	Type::Type(const Type& src) {
		kind = src.kind;
		data = src.data;
		if (kind < 0) data.RefObj->Attach();
	}

	bool Type::IsPair() const {
		switch (kind) {
		case RuleGeneratorType:return data.RGen->IsPair();
		case TupleType:return true;
		default:return false;
		}
	}

	Type Type::Fix(TypeFixingRules generateRules) const {
		if (kind == RuleGeneratorType) {
			return data.RGen->GetTemplateType(generateRules);
		} else if (IsPair()) {
			if (data.Tuple.Data->cachedFixed) return *this;
			auto f = data.Tuple.Data->fst.Fix(generateRules);
			auto r = data.Tuple.Data->rst.Fix(generateRules);
			auto c = Type::Chain(f, data.Tuple.fstArity, r);
			c.data.Tuple.Data->cachedFixed = true;
			return c;
		} else if (IsUserType()) {
			return Type::User(data.UserType.tag, data.UserType.Content->Fix(generateRules));
		} else return *this;
	}

	bool Type::IsFixed() const {
		if (kind == RuleGeneratorType) {
			return false;
		} else if (IsPair()) {
//			return data.Tuple.Data->fst.IsFixed() && data.Tuple.Data->rst.IsFixed();
			return data.Tuple.Data->cachedFixed;
		} else if (IsUserType()) {
			return data.UserType.Content->IsFixed();
		} else return true;
	}

	Type Type::First() const {
		switch (kind) {
		case TupleType:
			assert(data.Tuple.fstArity > 0);
			return data.Tuple.Data->fst;
			break;
		case UserType:
			return data.UserType.Content->First();
			break;
		case RuleGeneratorType:
			return data.RGen->First();
			break;
		default:
			InternalError("First of non-pair");
		}
		KRONOS_UNREACHABLE;
	}

	Type Type::Rest(size_t n) const {
		if (n == 0) return *this;
		switch (kind) {
		case TupleType:
			assert(data.Tuple.fstArity > 0);
			if (data.Tuple.fstArity > n) {
				Type tmp(*this);
				tmp.data.Tuple.fstArity -= n;
				return tmp;
			} else if (data.Tuple.fstArity == n) return data.Tuple.Data->rst;
			else return data.Tuple.Data->rst.Rest(n - data.Tuple.fstArity);
		case UserType:
			return data.UserType.Content->Rest(n);
		case RuleGeneratorType:
			return n == 1 ? data.RGen->Rest() : data.RGen->GetTemplateType(GenerateRulesForAllTypes).Rest(n);
		default:
			InternalError("Rest of non-pair");
		}
		KRONOS_UNREACHABLE;
	}

	Type Type::InvariantI64(int64_t integer_value) {
		return Type(integer_value);
	}

	Type Type::InvariantU64(size_t size_t_value) {
		assert(size_t_value <= (size_t)std::numeric_limits<int64_t>::max() && "Integer too big to be represented as invariant constant");
		return InvariantI64(static_cast<int64_t>(size_t_value));
	}

	Type Type::UnwrapUserType() const {
		if (IsRuleGenerator()) return data.RGen->UnwrapUserType();
		ASSERT_KIND(User);
		return *data.UserType.Content;
	}

	Type Type::Vector(const Type& element, unsigned count) {
		return count > 1 ? Type((Kind)element.Fix().kind, count) : element;
	}

	Type Type::Append(const Type & head, const Type & tail) {
		assert(head.IsFixed() && tail.IsFixed());
		std::vector<Type> hv;
		Type ht(head);
		while (ht.IsPair()) {
			hv.emplace_back(ht.First());
			ht = ht.Rest();
		}
		if (!ht.IsNil()) hv.emplace_back(ht);
		ht = tail;
		for (auto ti = hv.crbegin();ti != hv.crend();++ti) {
			ht = Pair(*ti, ht);
		}
		return ht;
	}

	bool Type::IsUserType(const TypeDescriptor& id) const {
		return (IsUserType() && GetDescriptor() == &id);
	}

	Type Type::Element(size_t index) const {
		return (index > 0 ? Rest(index) : *this).First();
	}

	Type Type::Union(Type maybeUnion, Type addToUnion, bool merge) {
		Ref<UnionData> ud = new UnionData;
		auto& st = ud->subTypes;
		if (merge && maybeUnion.IsUserType(UnionTag)) {
			maybeUnion = maybeUnion.UnwrapUserType().First();
			st.insert(st.end(), maybeUnion.data.Union->subTypes.begin(), maybeUnion.data.Union->subTypes.end());
		} else st.push_back(maybeUnion);
		st.push_back(addToUnion);
		for (auto& t : st) t = t.Fix();
		std::sort(st.begin(), st.end());
		st.erase(std::unique(st.begin(), st.end()), st.end());

		if (st.size() == 1) return st.front();

		ud->Final();
		return User(&UnionTag, Type::Pair(Type(ud), Type::Int32));
	}

	int Type::GetNumUnionTypes() const {
		ASSERT_KIND(Union);
		return (int)data.Union->subTypes.size();
	}

	Type Type::GetUnionType(int i) const {
		ASSERT_KIND(Union);
		return data.Union->subTypes[i];
	}

	bool Type::Tie() const { return true; }

	Type Type::Pair(const Type& fst, const Type& rst, bool disableRuleGen) {
		/* Check if it should be an optimized tuple */
		if (!disableRuleGen && rst.IsRuleGenerator()) {
			// maybe this is extending a recurring vector?
			return rst.data.RGen->PairTo(fst);
		} else if (rst.kind == TupleType) {
			if (fst.OrdinalCompare(rst.data.Tuple.Data->fst, false) == 0) {
				auto tmp(rst);
				if (fst.IsRuleGenerator()) {
					return Type(fst, rst, 1);
				}
				tmp.data.Tuple.fstArity++;
				return tmp;
			}
		}
		return Type(fst, rst, 1);
	}

	Type Type::Tuple(const Type& element, size_t repeatCount) {
		switch (repeatCount) {
		case 1:return element;
		default:
			{
				return Type(element, element, repeatCount - 1);
			   }
		}
	}

	Type Type::Chain(const Type& fst, size_t replicate, const Type& tail) {
		size_t n = tail.CountLeadingElements(fst);
		assert(replicate + n< std::numeric_limits<int>::max());
		return Type(fst, n>0 ? tail.Rest(n) : tail, replicate + n);
	}

	size_t Type::CountLeadingElements(const Type& ofType) const {
		if (IsRuleGenerator()) return data.RGen->GetTemplateType(GenerateRulesForAllTypes).CountLeadingElements(ofType);
		else if (IsPair()) {
			if (data.Tuple.Data->fst.OrdinalCompare(ofType, false) == 0) {
				return data.Tuple.fstArity;
			} else return 0;
		} else return 0;
	}

	Type Type::List(const Type& element, size_t repeatCount) {
		switch (repeatCount) {
		case 0:return Type(false);
		default:return Type(element, Type(false), repeatCount);
		}
	}

	bool Type::operator==(const Type& rhs) const {
		if (kind == RuleGeneratorType) return data.RGen->IsEqual(rhs, true);
		else return OrdinalCompare(rhs) == 0;
	}

	int Type::OrdinalCompare(const Type& rhs) const {
		int result = OrdinalCompare(rhs, true); 
		return result;
	}

	int Type::OrdinalCompare(const Type& rhs, bool generateRules) const {
		if (kind == RuleGeneratorType) return data.RGen->OrdinalCompare(rhs, generateRules);
		if (rhs.kind == RuleGeneratorType) return -rhs.data.RGen->OrdinalCompare(*this, generateRules);

		if (kind < rhs.kind) return 1;
		if (kind > rhs.kind) return -1;
		switch (kind) {
		case InvariantGraphType:
			// todo
			return GetGraph()->Compare(*(rhs.GetGraph()));
			break;
		case InvariantType:
			return ordinalCmp(data.InvariantValue->value, rhs.data.InvariantValue->value);
		case InvariantStringType:
			return data.InvariantString->compare(*rhs.data.InvariantString);
		case TupleType: {
			int t = data.Tuple.Data->fst.OrdinalCompare(rhs.data.Tuple.Data->fst, generateRules);
			if (t) return t;
			t = ordinalCmp(data.Tuple.fstArity, rhs.data.Tuple.fstArity);
			if (t) return t;
			return data.Tuple.Data->rst.OrdinalCompare(rhs.data.Tuple.Data->rst, generateRules);
						} break;
		case UserType: {
			int t = ordinalCmp(data.UserType.tag, rhs.data.UserType.tag);
			if (t) return t;
			return data.UserType.Content->OrdinalCompare(*rhs.data.UserType.Content, generateRules);
					   } break;
		case TypeTagType: {
			auto tmp(ordinalCmp(data.TypeTag->GetName(), rhs.data.TypeTag->GetName()));
			if (tmp) return tmp;
			else return ordinalCmp(data.TypeTag, rhs.data.TypeTag);
						  } break;
		case VectorType: {
			auto tmp(ordinalCmp(data.Vector.element, rhs.data.Vector.element));
			if (tmp) return -tmp;
			else return ordinalCmp(data.Vector.size, rhs.data.Vector.size);
						 } break;
		case InternalUsageType:
			return ordinalCmp(data.internalUsage, rhs.data.internalUsage);
			break;
		default:break;
		case UnionType: {
			auto tmp(ordinalCmp(data.Union->subTypes.size(), rhs.data.Union->subTypes.size()));
			if (tmp) return tmp;
			for (size_t i(0); i < data.Union->subTypes.size();++i) {
				tmp = ordinalCmp(data.Union->subTypes[i], rhs.data.Union->subTypes[i]);
				if (tmp) return tmp;
			}
			return 0;
		}
		}
		return 0;
	}

	size_t Type::GetHash() const {
		size_t h(kind);
		switch (kind) {
		case TupleType:HASHER(h, data.Tuple.fstArity); HASHER(h, data.Tuple.Data->cachedHash); break;
		case InvariantStringType:
		{
			return data.InvariantString->hash(1);
		}
		case InvariantType:
		{			
			size_t h(0);
			for (auto &w : data.InvariantValue->value.exponent.table) {
				HASHER(h, w);
			}

			for (auto &w : data.InvariantValue->value.mantissa.table) {
				HASHER(h, w);
			}
			return h;
		}
		case UserType:
			HASHER(h, (unsigned)(uintptr_t)data.UserType.tag);
			HASHER(h, data.UserType.Content->GetHash());
			HASHER(h, UserType);
			break;
		case InvariantGraphType:
			HASHER(h, GetGraph()->GetHash());
			HASHER(h, InvariantGraphType);
			break;
		case RuleGeneratorType:
			HASHER(h, data.RGen->GetHash());
			HASHER(h, RuleGeneratorType);
			break;
		case InternalUsageType:
			HASHER(h, (uint64_t)data.internalUsage);
			break;
		case VectorType:
			HASHER(h, data.Vector.size);
			HASHER(h, data.Vector.element);
			HASHER(h, VectorType);
			break;
		case ArrayViewType:
			HASHER(h, data.ArrayElement->GetHash());
			HASHER(h, ArrayViewType);
			break;
		case UnionType:
			return data.Union->cachedHash;
		default:break;
		}
		return h;
	}

	size_t Type::GetSize() const {
		switch (kind) {
		case TupleType:
			return data.Tuple.fstArity == 1 ? data.Tuple.Data->cachedSize :
				data.Tuple.fstArity * data.Tuple.Data->fst.GetSize() + data.Tuple.Data->rst.GetSize();
		case UserType:
			return data.UserType.Content->GetSize();
			break;
		case Float32Type:return SizeOfFloat32;
		case Int32Type:return SizeOfInt32;
		case Float64Type:return SizeOfFloat64;
		case Int64Type:return SizeOfInt64;
		case VectorType:return data.Vector.size * Type(data.Vector.element).GetSize();
		case RuleGeneratorType:return data.RGen->GetSize();
		case UnionType:return data.Union->cachedSize;
		case ArrayViewType:return SizeOfInt64 + SizeOfInt32 + SizeOfInt32;
		default:
			return 0;
		}
	}

	/* ctors */
	Type::Type(const Type& content, TypeDescriptor *desc):kind(UserType) {
		data.UserType.Content = new RefCounted<Type>(content);
		data.UserType.Content->Attach();
		data.UserType.tag = desc;
	}

	Type::Type(UnionData* ud) :kind(UnionType) { data.Union = ud; ud->Attach(); }
	Type::Type(const void *internal):kind(InternalUsageType) { data.internalUsage = internal; }
	Type::Type(TypeDescriptor *desc) : kind(TypeTagType) { data.TypeTag = desc; }
	Type::Type(bool truth) : kind(truth ? TrueType : NilType) { }

	Type::Type(double val) : kind(InvariantType) { 
		data.InvariantValue = new InvariantData;
		data.InvariantValue->Attach();
		data.InvariantValue->value = val;
#ifndef NDEBUG
		data.InvariantValue->vis = val;
#endif
	}

	Type::Type(std::int64_t val) : kind(InvariantType) { 
		data.InvariantValue = new InvariantData;
		data.InvariantValue->Attach();
		#ifdef TTMATH_PLATFORM32
		data.InvariantValue->value = ttmath::slint(val);
		#else
		data.InvariantValue->value = ttmath::sint(val);
		#endif
#ifndef NDEBUG
		data.InvariantValue->vis = val;
#endif
	}

	Type::Type(ttmath::Big<1, 2> val) : kind(InvariantType) {
		data.InvariantValue = new InvariantData;
		data.InvariantValue->Attach();
		data.InvariantValue->value = val;
#ifndef NDEBUG
		data.InvariantValue->vis = val.ToDouble();
#endif
	}

	Type Type::BigNumber(const char *val) {
		return Type{ ttmath::Big<1,2>(val) };
	}

	Type Type::ArrayView(const Type& element) {
		Type av;
		av.kind = ArrayViewType;
		av.data.ArrayElement = new RefCounted<Type>(element);
		av.data.ArrayElement->Attach();
		return av;
	}

	Type::Type(Kind k) : kind(k) { 
		assert(IsNativeType() && !IsNativeVector() && "Invalid vector element type");
	}
	Type::Type(const TypeRuleGenerator *gen) : kind(RuleGeneratorType) {
		data.RGen = gen;
		gen->Attach();
	}

	Type::Type(Kind k, unsigned count) : kind(VectorType) {
		data.Vector.element = k;
		data.Vector.size = count;
	}

	Type::Type(RefCounting* unknown) : kind(InternalRefcountType) {
		data.RefObj = unknown;
		unknown->Attach();
	}


	CRef<SString> empty = SString::cons("string");

	Type::Type(const SString *str):kind(InvariantStringType) {
		if (str == 0) str = empty;
		data.InvariantString = str;
		data.InvariantString->Attach();
	}

	Type::Type(const char *str):kind(InvariantStringType) {
		CRef<SString> s;
		if (str == 0) s = empty;
		else s = SString::cons(str);

		data.InvariantString = s;
		data.InvariantString->Attach();
	}

	Type::Type(const Type& fst, const Type& rst, size_t fstArity):kind(TupleType) {
		data.Tuple.Data = new TupleData(fst, rst);
		data.Tuple.Data->Attach();
		assert(fstArity > 0);
		data.Tuple.fstArity = fstArity;
	}

	Type::Type(Nodes::CGRef ast):kind(InvariantGraphType) {
		data.RefObj = new RefCounted<Graph<Nodes::Generic>>(ast);
		data.RefObj->Attach();
	}

	const Type Type::Nil(false);
	const Type Type::True(true);
	const Type Type::Float32(Type::Float32Type);
	const Type Type::Float64(Type::Float64Type);
	const Type Type::Int32(Type::Int32Type);
	const Type Type::Int64(Type::Int64Type);

	void EscapeString(std::ostream& strm, const SString *msg, size_t limit) {
		auto str(MakeRef(msg));
		bool tooLong(str->utf8len() > limit);
		if (tooLong) str = str->take(limit - 3);
		auto beg(str->begin()), end(str->end());
		strm << "\"";
		for (auto i(beg); i != end; ++i) {
			switch (*i) {
			case '\n':strm << "\\n"; break;
			case '\r':strm << "\\r"; break;
			case '\t':strm << "\\t"; break;
			case '\"':strm << "\\\""; break;
			case '\\': strm << "\\\\"; break;
			default:strm << (char)*i; break;
			}
		}
		if (tooLong) strm << "...";
		strm << "\"";
	}

	template <typename STR> void EscapeStringXML(std::ostream& strm, const STR& str) {
		auto beg(str.begin());
		auto end(str.end());
		for (auto i(beg); i != end; ++i) {
			switch (*i) {
			case '\"':strm << "&quot;"; break;
			case '<':strm << "&lt;"; break;
			case '>':strm << "&gt;"; break;
			case '&':strm << "&amp;"; break;
			case '\'': strm << "&apos;"; break;
			default:strm << (char)*i; break;
			}
		}
	}

	bool Type::IsInLocalScope() const {
		switch (kind) {
		case RuleGeneratorType:
			return data.RGen->GetTemplateType(Type::GenerateNoRules).IsInLocalScope();
			break;
		case ArrayViewType:
			return true;
		case TupleType:
			return data.Tuple.Data->cachedNoReturn;
		case UserType:
			return data.UserType.Content->IsInLocalScope();
		case UnionType:
			for (auto& st : data.Union->subTypes) {
				if (st.IsInLocalScope()) return true;
			}
			return false;
		default:
			return false;
		}
	}

	void Type::OutputText(std::ostream& stream, const void *instance, bool pairExtension) const {
		switch (kind) {
		case InvariantType:
			stream << "#" << data.InvariantValue->value.ToString();
			break;
		case InvariantStringType:
			//			EscapeString(stream,data.InvariantString,20);
			stream << *data.InvariantString;
			break;
		case UnionType: {
			stream << "{{";			
			for (size_t i(0);i < data.Union->subTypes.size();++i) {
				if (i) stream << "|";
				data.Union->subTypes[i].OutputText(stream, nullptr, false);
			}
			stream << "}}";
			break;
		}
		case TupleType:
			if (instance) {
				char *ptr = (char *)instance;
				if (pairExtension == false) stream << "(";
				for (unsigned i(0); i < data.Tuple.fstArity; ++i) {
					if (i) stream << " ";
					data.Tuple.Data->fst.OutputText(stream, ptr);
					ptr += data.Tuple.Data->fst.GetSize();
				}
				stream << " ";
				data.Tuple.Data->rst.OutputText(stream, ptr, true);
				if (pairExtension == false) stream << ")";
			} else {
				if (pairExtension == false) stream << "(";
				data.Tuple.Data->fst.OutputText(stream, instance);
				if (data.Tuple.fstArity > 1) {
					if (data.Tuple.Data->fst.OrdinalCompare(data.Tuple.Data->rst, false) == 0) {
						stream << "x" << data.Tuple.fstArity + 1; 
						if (pairExtension == false) stream << ")";
						break;
					} else {
						stream << "x" << data.Tuple.fstArity;
					}
				}
				stream << " ";
				data.Tuple.Data->rst.OutputText(stream, instance, true);
				if (pairExtension == false) stream << ")";
			}
			break;
		case Float32Type:
			stream.precision(std::numeric_limits<float>::max_digits10);
			if (instance) stream << *(float*)instance; else stream << "Float";
			break;
		case Float64Type:
			stream.precision(std::numeric_limits<double>::max_digits10);
			if (instance) stream << *(double*)instance; else stream << "Double";
			break;
		case Int32Type:
			if (instance) stream << *(int32_t*)instance; else stream << "Int32";
			break;
		case Int64Type:
			if (instance) stream << *(int64_t*)instance; else stream << "Int64";
			break;
		case TrueType:stream << "True";
			break;
		case NilType:stream << "nil";
			break;
		case RuleGeneratorType:data.RGen->OutputText(stream, instance, pairExtension); break;
		case TypeTagType:stream << "#" << data.TypeTag->GetName(); break;
		case InvariantGraphType:
			stream << "Anon-Fn"; break;
		case UserType:
			if (data.UserType.tag == &UnionTag && instance) {
				auto unsafeUnion = data.UserType.Content->First();
				assert(unsafeUnion.IsUnion());
				char *ptr = (char *)instance;
				int subType = *(std::uint32_t*)(ptr + data.UserType.Content->First().GetSize());
				unsafeUnion.data.Union->subTypes[subType].OutputText(stream, instance, pairExtension);
			} else {
				int limit = data.UserType.tag->GetPrintLimit();
				if (limit >= 0 && data.UserType.Content->Arity().GetInvariant() > limit) {
					stream << data.UserType.tag->GetName();
					if (limit > 0) {
						stream << "{";
						Type lt = *data.UserType.Content;
						for (int i = 0; i < limit; ++i) {
							if (i) stream << " ";
							lt.First().OutputText(stream, instance, false);
							instance = (char*)instance + lt.First().GetSize();
							lt = lt.Rest();
						}
						stream << "}";
					}
				} else {
					stream << data.UserType.tag->GetName() << "{";
					data.UserType.Content->OutputText(stream, instance, true);
					stream << "}";
				}
			}
			break;
		case InternalUsageType:stream << "<native>"; break;
		case VectorType:
			if (instance) {
				stream << "<";
				for (int n = data.Vector.size; n > 0; --n, stream << (n ? " " : "")) {
					Type tmp(data.Vector.element);
					tmp.OutputText(stream, instance);
					instance = (const char*)instance + tmp.GetSize();
				}
				stream << ">";
			} else {
				stream << "<" << data.Vector.size << " x " << Type(data.Vector.element) << ">"; break;
			}
			break;
		default:
			stream << "#pretty-print missing#";
		}
	}

	std::string EscapeTypeTag(std::string qualifiedName) {
		if (qualifiedName.front() != ':') qualifiedName = ":" + qualifiedName;
		for (size_t i(1); i < qualifiedName.size(); ++i) {
			if (qualifiedName[i] == ':') qualifiedName[i] = '.';
		}
		return qualifiedName;
	}

	void Type::OutputFormatString(std::ostream &os, bool pairExtension) const {
		std::stringstream stream;
		if (GetSize() || kind == UserType) {
			switch (kind) {
				case TupleType:
					if (pairExtension == false) os << "(";
                    
                    if (data.Tuple.fstArity > 1) os << "%[" << data.Tuple.fstArity << ":";
                    data.Tuple.Data->fst.OutputFormatString(os); os << " ";
                    if (data.Tuple.fstArity > 1) os << "%]";

                    data.Tuple.Data->rst.OutputFormatString(os, true);
					if (pairExtension == false) os << ")";
					return;
				case Float32Type: os << "%f"; return;
				case Float64Type: os << "%d"; return;
				case Int32Type: os << "%i"; return;
				case Int64Type: os << "%q"; return;
				case VectorType: os << "<";
					for (int i(0);i < data.Vector.size;++i) {
						if (i) os << " ";
						switch (data.Vector.element) {
							case Float32Type: os << "%f"; break;
							case Float64Type: os << "%d"; break;
							case Int32Type: os << "%i"; break;
							case Int64Type: os << "%q"; break;
							default:
								std::clog << "Unexpected type " << data.Vector.element;
								assert(0 && "Unexpected vector type in format string generation");
						}
					}
					os << ">"; return;
				case UserType:
					os << "{" << data.UserType.tag->GetName().substr(1) << " ";
					data.UserType.Content->OutputFormatString(os, true);
					os << "}"; 
					return;
				case ArrayViewType:
					os << "#Array-View{%q %i %i}"; 
					return;
				default:
					std::clog << "Unexpected type " << kind;
					assert(0 && "Unexpected sized type in format string generation");						
			}
			return;
		} else {
			OutputText(stream, nullptr, pairExtension);
			for (auto c : stream.str()) {
				switch (c) {
				case '%': os << "%%"; break;
				default:  os << c; break;
				}
			}
		}
	}

	void Type::OutputJSONTemplate(std::ostream& os, bool pairExtension) const {
		switch (kind) {
			case TupleType:
				if (pairExtension == false) os << "[";
                
                if (data.Tuple.fstArity > 1) os << "%[" << data.Tuple.fstArity << ":";
                data.Tuple.Data->fst.OutputJSONTemplate(os); os << ",";
                if (data.Tuple.fstArity > 1) os << "%]";

				data.Tuple.Data->rst.OutputJSONTemplate(os, true);
				if (pairExtension == false) os << "]";
				return;
			case Float32Type: os << "%f"; return;
			case Float64Type: os << "%d"; return;
			case Int32Type: os << "%i"; return;
			case Int64Type: os << "{\"Int64\":\"%q\"}"; return;
			case VectorType: os << "{\"Vec\":[";
				for (int i(0);i < data.Vector.size;++i) {
					if (i) os << ",";
					switch (data.Vector.element) {
						case Float32Type: os << "%f"; break;
						case Float64Type: os << "%d"; break;
						case Int32Type: os << "%i"; break;
						case Int64Type: os << "%q"; break;
						default:
							assert(0 && "Unexpected vector type in JSON string generation");
					}
				}
				os << "]}"; 
				return;
			case TrueType:
				os << "{\"true\": true}"; return;
			case NilType:
				os << "{}"; return;
			case UnionType:
				os << "UNION TYPES NOT SUPPORTED"; return;
			case UserType:
				os << "{\"" << data.UserType.tag->GetName() << "\":";
				data.UserType.Content->OutputJSONTemplate(os, false);
				os << "}";
				return;
			case InvariantStringType:
				EscapeString(os, data.InvariantString, std::numeric_limits<size_t>::max());
				return;
			case InvariantGraphType:
				os << "{\"fn\": {}}";
				return;
			case TypeTagType:
				os << "{\"tag\": " <<
					lithe::grammar::json::encode_string(data.TypeTag->GetName()) 
					<< "}";
				return;
			case InvariantType:
				os << "{\"#\": " << data.InvariantValue->value.ToString() << "}";
				break;
			case ArrayViewType:
				os << "{\"arrayview\": [%q, %i, %i]}";
				break;
			default:
				{
					std::stringstream s;
					OutputFormatString(s);
					os << "{\"unknown\": " << lithe::grammar::json::encode_string(s.str()) << "}";
					return;
				}
				return;
		}
	}

	Type Type::GetArrayViewElement() const {
		ASSERT_KIND(ArrayView);
		return *data.ArrayElement;
	}

	double Type::GetInvariant() const {
		ASSERT_KIND2(RuleGenerator, Invariant);
		if (IsRuleGenerator()) return data.RGen->GetTemplateType().GetInvariant();
		return data.InvariantValue->value.ToDouble();
	}

	std::int64_t Type::GetInvariantI64() const {
		ASSERT_KIND2(RuleGenerator, Invariant);
		if (IsRuleGenerator())
			return data.RGen->GetTemplateType().GetInvariantI64();
		else 
			return strtoll(data.InvariantValue->value.ToString().c_str(), nullptr, 10);
	}

	ttmath::Big<1, 2> Type::GetBigNum() const {
		ASSERT_KIND2(Invariant, RuleGenerator);
		if (IsRuleGenerator()) return data.RGen->GetTemplateType().GetBigNum();
		return data.InvariantValue->value;
	}

	bool Type::GetTrueOrNil() const {
		ASSERT_KIND3(RuleGenerator, True, Nil);
		return IsRuleGenerator() ? data.RGen->GetTemplateType().GetTrueOrNil() : (kind == TrueType);
	}

	CRef<SString> Type::GetString() const {
		ASSERT_KIND2(RuleGenerator, InvariantString);
		return IsRuleGenerator() ? data.RGen->GetTemplateType().GetString() : MakeRef(data.InvariantString);
	}

	TypeDescriptor* Type::GetDescriptor() const {
		ASSERT_KIND3(RuleGenerator, TypeTag, User);
		return IsRuleGenerator() ? data.RGen->GetTemplateType(GenerateNoRules).GetDescriptor() :
			(IsTypeTag() ? data.TypeTag : data.UserType.tag);
	}

	Nodes::CGRef Type::GetGraph() const {
		ASSERT_KIND2(InvariantGraph, RuleGenerator);
		return IsRuleGenerator() ? data.RGen->GetTemplateType().GetGraph() :
			*((RefCounted < Graph < Nodes::Generic >> *)data.RefObj);
	}

	uint16_t Type::GetVectorWidth() const {
		return IsRuleGenerator() ? data.RGen->GetTemplateType().GetVectorWidth() : (IsNativeVector() ? data.Vector.size : 1);
	}

	Type Type::GetVectorElement() const {
		return IsRuleGenerator() ? data.RGen->GetTemplateType().GetVectorElement() : (IsNativeVector() ? Type(data.Vector.element) : *this);
	}

	bool Type::IsKind(Kind type) const {
		return IsKind(type, true);
	}

	bool Type::IsUserType() const {
		return kind == UserType || (IsRuleGenerator() && data.RGen->IsUserType());
	}

	bool Type::IsKind(Kind type, bool generateRules) const {
		if (IsRuleGenerator()) {
			if (generateRules) {
				if (type == TupleType && data.RGen->IsPair( )) return true;
				if (type == InvariantType && data.RGen->IsInvariant()) return true;
			}
			return data.RGen->GetTemplateType(generateRules?GenerateRulesForAllTypes:GenerateNoRules).IsKind(type, generateRules);
		} else return kind == type;
	}

	Type Type::operator+(const Type& rhs) const {
		if (IsRuleGenerator()) return data.RGen->Add(rhs);
		else if (rhs.IsRuleGenerator()) return rhs.data.RGen->Add(*this);
		return Type(GetInvariant() + rhs.GetInvariant());
	}

	Type Type::operator-(const Type& rhs) const {
		if (IsRuleGenerator()) return data.RGen->Sub(rhs);
		return Type(GetInvariant() - rhs.GetInvariant());
	}

	bool Type::IsNilTerminated() const {
		if (IsNil()) return true;
		if (IsPair()) {
			assert(kind == TupleType);
			return data.Tuple.Data->rst.IsNilTerminated();

		} else return false;
	}

	Type Type::Arity() const {
		assert(!IsUnion());
		if (IsRuleGenerator()) return data.RGen->GetTemplateType().Arity();
		else if (IsNil()) return InvariantLD(0.0l);

		size_t count(0);

		const Type *ptr(this);
		while (ptr->kind == TupleType) {
			count += ptr->data.Tuple.fstArity;
			ptr = &ptr->data.Tuple.Data->rst;
		}

		count += ptr->IsNil() ? 0 : 1;

		return InvariantU64(count);
	}

	TypeDescriptor SpecializationFailure("Specialization failure", false);
	TypeDescriptor UserException("Exception", true);
	TypeDescriptor FatalFailure("Fatal", false);
	TypeDescriptor RecursionTrap("Recursive", false);
	TypeDescriptor PropagateFailure("Reject Parent", false);
	TypeDescriptor NoEvalFallback("Fail without :Fallback:Eval", false);
	TypeDescriptor PackageUseDirective("Using");
	TypeDescriptor MonitoredError("Upstream Error");

	TypeDescriptor Float32Tag(":Float", false);
	TypeDescriptor Float64Tag(":Double", false);
	TypeDescriptor Int32Tag(":Int", false);
	TypeDescriptor Int64Tag(":Int64", false);
	TypeDescriptor PairTag(":Pair", false);
	TypeDescriptor VectorTag(":Vector", false);
	TypeDescriptor TypeTagTag(":Type-ID", false);
	TypeDescriptor TrueTag(":True", false);
	TypeDescriptor NilTag(":Nil", false);
	TypeDescriptor GraphTag(":Graph", false);
	TypeDescriptor InvariantTag(":Constant", false);
	TypeDescriptor InvariantStringTag(":String", false);
	TypeDescriptor UserTypeTag(":Compound", false);
	TypeDescriptor UnionTag(":Union", false);
	TypeDescriptor FunctionTag(":Fn", true, 1);
	TypeDescriptor ViewTag(":Array-View", false);
	TypeDescriptor ReactiveRateTag("#Rate", true);
	TypeDescriptor AudioFileTag("Audio File");

	TypeDescriptor* Type::TagForKind(int kind) {
		switch (kind) {
		case TupleType:return &PairTag;
		case Float32Type:return &Float32Tag;
		case Float64Type:return &Float64Tag;
		case Int32Type:return &Int32Tag;
		case Int64Type:return &Int64Tag;
		case TrueType:return &TrueTag;
		case NilType:return &NilTag;
		case TypeTagType:return &TypeTagTag;
		case InvariantGraphType:return &GraphTag;
		case InvariantType:return &InvariantTag;
		case InvariantStringType:return &InvariantStringTag;
		case UserType:return &UserTypeTag;
		case VectorType:return &VectorTag;
		case ArrayViewType: return &ViewTag;
		default:assert(0 && "Unknown typetag"); return 0;
		}
	}

	Type Type::TypeOf() const {
		if (IsRuleGenerator()) return data.RGen->TypeOf();
		else if (IsPair()) return Type(&PairTag);
		else if (IsFloat32()) return Type(&Float32Tag);
		else if (IsFloat64()) return Type(&Float64Tag);
		else if (IsInt32()) return Type(&Int32Tag);
		else if (IsInt64()) return Type(&Int64Tag);
		else if (IsTypeTag()) return Type(&TypeTagTag);
		else if (IsTrue()) return Type(&TrueTag);
		else if (IsNil()) return Type(&NilTag);
		else if (IsGraph()) return Type(&GraphTag);
		else if (IsInvariantString()) return Type(&InvariantStringTag);
		else if (IsInvariant()) return Type(&InvariantTag);
		else if (IsUserType()) return Type(data.UserType.tag);
		else if (IsNativeVector()) return Type(&VectorTag);
		else if (IsArrayView()) return Type(&ViewTag);
		assert(0 && "Missing type tag annotation");
		KRONOS_UNREACHABLE;
	}

	template <> Type Type::FromNative<float>(float tmp) { return Type(Float32Type); }
	template <> Type Type::FromNative<double>(double tmp) { return Type(Float64Type); }
	template <> Type Type::FromNative<int32_t>(int32_t tmp) { return Type(Int32Type); }
	template <> Type Type::FromNative<int64_t>(int64_t tmp) { return Type(Int64Type); }

	template <> double ExtractValue<double>(const Type& t) { return t.GetInvariant( ); }
	template <> bool ExtractValue<bool>(const Type& t) { return t.GetTrueOrNil( ); }
	template <> CRef<SString> ExtractValue<CRef<SString>>(const Type& t) { return t.GetString( ); }
	template <> TypeDescriptor* ExtractValue<TypeDescriptor*>(const Type& t) { return t.GetDescriptor( ); }
	template <> Nodes::CGRef ExtractValue<Nodes::CGRef>(const Type& t) { return t.GetGraph( ); }
	template <> Type ExtractValue<Type>(const Type& t) { return t; }
	template <> ttmath::Big<1, 2> ExtractValue<ttmath::Big<1, 2>>(const Type &t) {
		return t.GetBigNum();
	}

	template <> bool IsOfType<double>(const Type& t) { return t.IsInvariant( ); }
	template <> bool IsOfType<bool>(const Type& t) { return t.IsTrue( ) || t.IsNil( ); }
	template <> bool IsOfType<CRef<SString>>(const Type& t) { return t.IsInvariantString( ); }
	template <> bool IsOfType<TypeDescriptor*>(const Type& t) { return t.IsUserType( ) || t.IsTypeTag( ); }
	template <> bool IsOfType<Nodes::CGRef>(const Type& t) { return t.IsGraph( ); }
	template <> bool IsOfType<Type>(const Type& t) { return true; }
};
