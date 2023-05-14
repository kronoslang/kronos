#include "common/PlatformUtils.h"
#include "Invariant.h"
#include "Graph.h"
#include "Errors.h"
#include "TypeAlgebra.h"
#include "Evaluate.h"
#include "CompilerNodes.h"
#include "Native.h"
#include "FlowControl.h"
#include "tinyxml.h"
#include "LibraryRef.h"
#include "DynamicVariables.h"
#include <iostream>
#include <sstream>
#include <string>
#include <memory>

#include "config/system.h"
#include "ttmath/ttmath.h"

#include <codecvt>
#include <locale>
static std::basic_string<wchar_t> ToUnicode(std::string utf8) {
	
	std::wstring_convert<std::codecvt_utf8<wchar_t>, wchar_t> decoder;
	return decoder.from_bytes(utf8.c_str( ));
}

namespace K3 {
	typedef ttmath::Big<1,2> ivar;
	typedef CRef<SString> istr;
	struct Any : public Type {
		Any(Type t = Type()) :Type(t) {}
	};
	template <> Any ExtractValue<Any>(const Type& t) { return t; }
	namespace Nodes{
		namespace Invariant{
			Specialization GenericNoFallback::Specialize(SpecializationState&) const {
				return Specialization(nullptr, Type(&NoEvalFallback));
			}

			Specialization GenericClassOf::Specialize(SpecializationState& t) const {
				SPECIALIZE_ARGS(t,0);
				return Specialization(Typed::Nil(),A0.result.TypeOf());
			}

			Specialization GenericEqualType::Specialize(SpecializationState& t) const {
				SPECIALIZE_ARGS(t,0,1);
				return Specialization(Typed::Nil(),Type(A0.result == A1.result));
			}

			Specialization GenericOrdinalCompareType::Specialize(SpecializationTransform& t) const {
				SPECIALIZE_ARGS(t, 0, 1);
				// extract as little information from types as possible, in order to
				// minimize the type rules created during recursive specialization
				if (expectOrdinal == 0)
					return Specialization(Typed::Nil(), Type(A0.result == A1.result));

				return Specialization(Typed::Nil(), Type(A0.result.OrdinalCompare(A1.result) == expectOrdinal));
			}

			Specialization GenericRequire::Specialize(SpecializationState& t) const {
				SPECIALIZE_ARGS(t,0,1);
				return A1;
			}

			Specialization GenericTrace::Specialize(SpecializationState& t) const {
				SPECIALIZE_ARGS(t, 0, 1);
				std::cout << A0.result << A1.result << "\n";
				return A1;
			}

			Specialization ExplainConstraint::Specialize(SpecializationState& t) const {
				auto block = t.GetRep().Block(Verbosity::LogAlways, "constr");
				SPECIALIZE_ARGS(t, 1, 2);
				auto spec = t(GetUp(0));
				if (spec.node == nullptr) {
					std::stringstream explain;
					A1.result.OutputText(explain, nullptr, true);
					t.GetRep().Diagnostic(Verbosity::LogAlways, GetUp(0), Error::TypeMismatchInSpecialization, A2.result, "%s", explain.str().c_str());
				}
				return spec;
			}

			int Constant::LocalCompare(const ImmutableNode& rhs) const
			{
				auto& rc = (const Constant&)rhs;
				auto tmp = VAL.OrdinalCompare(rc.VAL);
				if (tmp) return tmp;
				tmp = ordinalCmp(memory.size(), rc.memory.size());
				if (tmp) return tmp;
				return memcmp(memory.data(), rc.memory.data(), memory.size());
			}

			unsigned Constant::ComputeLocalHash() const {
				auto h = DisposableGenericLeaf::ComputeLocalHash();
				HASHER(h, VAL.GetHash());
				for (int i = 0;i < memory.size();++i) {
					HASHER(h, std::hash<char>()(memory[i]));
				}
				return h;
			}

			Specialization Constant::Specialize(SpecializationState &t) const
			{
				return VAL.GetSize() > 0 ? Specialization(Native::Constant::New(VAL, memory.size() ? memory.data() : nullptr),Type(VAL)) 
										 : Specialization(Typed::Nil(),Type(VAL));
			}

			void Constant::Output(std::ostream& stream) const
			{
				stream << VAL;
			}

			template <typename> bool Check(const Type& t);

			template <> bool Check<ivar>(const Type& t)
			{
				return t.IsInvariant();
			}

			template <> bool Check<bool>(const Type& t)
			{
				return t.IsTrue() || t.IsNil();
			}

			template <> bool Check<Type>(const Type& t)
			{
				return t.IsInvariant();
			}

			template <> bool Check<istr>(const Type& t)
			{
				return t.IsInvariantString();
			}

			template <> bool Check<Any>(const Type& t) {
				return true;
			}

			template <class RESULT,class ARG>
			class UnaryNode : public DisposableGenericUnary
			{
				typedef UnaryNode<RESULT,ARG> _Myt;
				const char *name;
				INHERIT_RENAME(UnaryNode, DisposableGenericUnary);
			public:
				REGION_ALLOC(_Myt);
				UnaryNode(const char *name,CGRef up):DisposableGenericUnary(up),name(name) {}
				const char *PrettyName() const override {return name;}

				virtual RESULT Operate(ARG) const = 0;

				Specialization Specialize(SpecializationState &t) const override
				{
					SPECIALIZE(t,x,GetUp(0));
					if (Check<ARG>(x.result) == false)
					{
						t.GetRep().Diagnostic(LogTrace,this,Error::TypeMismatchInSpecialization,
							Type(ARG()),
							"Cannot '%s' this type at compile time", PrettyName());
						return SpecializationFailure();
					}
					return Specialization(Typed::Nil(),Type(Operate(ExtractValue<ARG>(x.result))));
				}
			};

			template <class RESULT, class ARG1, class ARG2, int UTILOPCODE>
			class BinaryNode : public DisposableGenericBinary
			{
				const char *name;
				INHERIT_RENAME(BinaryNode, DisposableGenericBinary);
				typedef BinaryNode<RESULT,ARG1,ARG2,UTILOPCODE> _Myt;
			public:

				REGION_ALLOC(_Myt);
				const char *PrettyName() const override {return name;}
				BinaryNode(const char *name, CGRef lhs, CGRef rhs):DisposableGenericBinary(lhs,rhs),name(name) {}
				virtual RESULT Operate(ARG1 a, ARG2 b) const = 0;

				Specialization Specialize(SpecializationState &t) const override
				{
					SPECIALIZE(t,a,GetUp(0));
					SPECIALIZE(t,b,GetUp(1));
					if (Check<ARG1>(a.result) == false ||
						Check<ARG2>(b.result) == false)
					{
						if (UTILOPCODE != Native::Nop && (a.result.IsInt64() || b.result.IsInt64()))
						{
							/* utils are used internally for sequence count arithmetic */
							if (a.result.IsInvariant()) a.node = Native::Constant::New(int64_t(a.result.GetInvariant()));
							if (b.result.IsInvariant()) b.node = Native::Constant::New(int64_t(b.result.GetInvariant()));
							return Specialization(Native::MakeInt64(name,UTILOPCODE,a.node,b.node),Type::Int64);
						}

						t.GetRep().Diagnostic(LogTrace,this,Error::TypeMismatchInSpecialization,
							Type::Pair(a.result,b.result),
							"Cannot '%s' these types at compile time", PrettyName());
						return SpecializationFailure();
					}

					return Specialization(Typed::Nil(),Type(Operate(ExtractValue<ARG1>(a.result),ExtractValue<ARG2>(b.result))));
				}

			};

			Specialization GenericPropagateFailure::Specialize(SpecializationState& t) const {
				return Specialization(nullptr, Type(&PropagateFailure));
			}

			Specialization Arity::Specialize(SpecializationState& t) const
			{
				SPECIALIZE(t,a,GetUp(0));
				return Specialization(Typed::Nil(),Type::InvariantLD(a.result.Arity().GetInvariant()));
			}

			CGRef Arity::New(CGRef up)
			{
				std::int64_t delta(0);
				while(IsOfExactType<GenericPair>(up))
				{
					delta ++;
					up = up->GetUp(1);
				}

				if (delta>0) return(Invariant::Add(Arity::New(up),Constant::New(delta)));

				ReplicateFirst *rf;
				if ((rf = ShallowCast<ReplicateFirst>(up)))
				{
					return Invariant::Add(
						rf->GetUp(0),
						Arity::New(rf->GetUp(1)));
				}
				return new Arity(up);
			}

			bool Arity::InverseFunction(int branch, const Type& down, Type& up, SpecializationTransform& t) const
			{
				auto fwd(t(GetUp(0)));
				if (fwd.node == 0) return false;
				if (fwd.result.IsPair()) {
					up = Type::Tuple(fwd.result.First(),(size_t)down.GetInvariant());
				} else {
					up = Type::InvariantI64(1);
				}
				return true;
			}

			CGRef ReplicateFirst::GetFirst() const {
				auto w = When::New();
				auto z = Invariant::Constant::New(0);
				w->Connect(Invariant::CmpGe(GetUp(0), z));
				w->Connect(Invariant::Constant::New(GetElement()));
				w->Connect(Invariant::CmpEq(GetUp(0), z));
				w->Connect(GenericFirst::New(GetUp(1)));
				return w;
			}

			Specialization ReplicateFirst::Specialize(SpecializationState& t) const
			{
				SPECIALIZE(t,A0,GetUp(0))
				SPECIALIZE(t,A1,GetUp(1))
				if (A0.result.IsInvariant())
				{
					auto count = (int)A0.result.GetInvariant();
					if (count < 1) {
						if (count < 0) A1.result = A1.result.Rest(-count);
						return A1;
					}
					return Specialization(Typed::Nil(),Type::Chain(element,(size_t)A0.result.GetInvariant(),A1.result));
				}
				else if (A0.result.IsInt64())
				{
					return Specialization(VariantTuple::New(Native::Constant::New(element,0),A0.node,Native::Constant::New(A1.result,0),GetRecurrenceDelta()),
						Type::Chain(element,std::numeric_limits<int32_t>::max()-1,A1.result));
				}
				else return SpecializationFailure();
			}

			bool ReplicateFirst::InverseFunction(int branch, const Type& down, Type& up, SpecializationTransform& t) const
			{
				if (branch == 0) {
					auto chain(t(GetUp(1)).result);
					if (down == chain) {
						up = Type::InvariantI64(0);
						return true;
					}
					else if (down.IsPair()) {
						up = Type::InvariantI64(down.CountLeadingElements(down.First()) - chain.CountLeadingElements(down.First()));
						return true;
					}
				}
				return false;
			}

			void ReplicateFirst::Output(std::ostream& stream) const
			{
				stream << " Replicate<" << element << ">";
			}

			CGRef ReplicateFirst::New(CGRef count, const Type& element, CGRef chain, int delta)
			{
				return new ReplicateFirst(count,element,chain,delta);
			}

			CGRef CountLeadingElements::New(const Type& templ,  CGRef data) {
				GenericPair *p;
				if ((p = ShallowCast<GenericPair>(data))) {
					Invariant::Constant *c;
					if ((c = ShallowCast<Invariant::Constant>(p->GetUp(0)))) {
						if (c->GetType() == templ) 
							return Invariant::Add(Invariant::Constant::New(Type::InvariantI64(1)),
							CountLeadingElements::New(templ,p->GetUp(1)));
						else return Invariant::Constant::New(Type::InvariantI64(0));
					}
				}

				ReplicateFirst *r;
				if ((r = ShallowCast<ReplicateFirst>(data))) {
					if (r->GetElement() == templ) return data->GetUp(0);
					else return Invariant::Constant::New(Type::InvariantI64(0));
				}

				return new CountLeadingElements(templ,data);
			}

			void CountLeadingElements::Output(std::ostream& text) const
			{
				text << "CountLeading<"<<GetElement()<<">";
			}

			Specialization CountLeadingElements::Specialize(SpecializationState& t) const
			{
				SPECIALIZE_ARGS(t,0);
				return Specialization(Typed::Nil(),Type::InvariantU64(A0.result.CountLeadingElements(element)));
			}

			template <class RETURN,class ARG>
			class UnaryPluggable : public UnaryNode<RETURN,ARG>
			{
				RETURN(*func)(ARG);
				using _Myt = UnaryPluggable<RETURN,ARG>;
                using SuperTy = UnaryNode<RETURN,ARG>;
			public:
				REGION_ALLOC(_Myt);
				DEFAULT_LOCAL_COMPARE(SuperTy,func);
				UnaryPluggable(RETURN(*f)(ARG),const char *symbol, CGRef up)
					:UnaryNode<RETURN,ARG>(symbol,up),func(f) {}
				RETURN Operate(ARG a) const override {return func(a);}
				Generic *ConstructShallowCopy() const override {return new UnaryPluggable(*this);}
			};

			template <class RETURN,class ARG1,class ARG2,int UTILOPCODE>
			class BinaryPluggable : public BinaryNode<RETURN,ARG1,ARG2,UTILOPCODE>
			{
				RETURN(*func)(ARG1,ARG2);
				using _Myt = BinaryPluggable<RETURN,ARG1,ARG2,UTILOPCODE>;
                using SuperTy = BinaryNode<RETURN,ARG1,ARG2,UTILOPCODE>;
			public:
				REGION_ALLOC(_Myt);
				DEFAULT_LOCAL_COMPARE(SuperTy,func);
				BinaryPluggable(RETURN(*f)(ARG1,ARG2),const char *symbol, CGRef lhs, CGRef rhs)
					:BinaryNode<RETURN,ARG1,ARG2,UTILOPCODE>(symbol,lhs,rhs),func(f) {}
				RETURN Operate(ARG1 a, ARG2 b) const override {
					return func(a,b);
				}
				Generic *ConstructShallowCopy() const override {return new BinaryPluggable(*this);}
			};

			template <class RETURN,class ARG>
			class UnaryInversible : public UnaryPluggable<RETURN,ARG>, public IInversible
			{
				ARG(*inverse)(RETURN);
                using _Myt = UnaryInversible<RETURN,ARG>;
                using SuperTy = UnaryPluggable<RETURN,ARG>;
            public:
				REGION_ALLOC(_Myt);
				DEFAULT_LOCAL_COMPARE(SuperTy,inverse);
				INHERIT(UnaryInversible,SuperTy,IInversible);
			public:
				UnaryInversible(RETURN(*func)(ARG),ARG(*inverse)(RETURN),const char *symbol, CGRef up)
					:SuperTy(func,symbol,up),inverse(inverse) {}

				Type Inverse(const Type& down) const
				{
					return inverse(down);
				}
				Generic *ConstructShallowCopy() const override {return new UnaryInversible(*this);}
			};

			template <class RETURN,class ARG1,class ARG2, int UTILOPCODE>
			class BinaryInversible : public BinaryPluggable<RETURN,ARG1,ARG2,UTILOPCODE>, public IInversible
			{
				using SuperTy = BinaryPluggable<RETURN,ARG1,ARG2,UTILOPCODE>;
				using _Myt = BinaryInversible<RETURN,ARG1,ARG2,UTILOPCODE>;
				ARG1(*inverse_left)(RETURN,ARG2);
				ARG2(*inverse_right)(RETURN,ARG1);
			public:
				REGION_ALLOC(_Myt);
				DEFAULT_LOCAL_COMPARE(SuperTy,inverse_left,inverse_right);
				INHERIT_RENAME(_Myt,IInversible,SuperTy);
			public:
				const char *PrettyName() const  override {return SuperTy::PrettyName();}
				BinaryInversible(RETURN(*func)(ARG1,ARG2),
					ARG1(*inverse_l)(RETURN,ARG2),
					ARG2(*inverse_r)(RETURN,ARG1),
					const char *symbol, CGRef lhs,CGRef rhs)
					:SuperTy(func,symbol,lhs,rhs),inverse_left(inverse_l),inverse_right(inverse_r) 
				{}

				bool InverseFunction(int branch, const Type& down, Type& up, SpecializationTransform& t) const  override
				{
					Specialization b;
					switch(branch)
					{
					case 0:
						b=t(this->GetUp(1));
						if (b.node==0) return false;
						up=Type(inverse_left(ExtractValue<RETURN>(down),ExtractValue<ARG2>(b.result)));break;
					case 1:
						b=t(this->GetUp(0));
						if (b.node==0) return false;
						up=Type(inverse_right(ExtractValue<RETURN>(down),ExtractValue<ARG1>(b.result)));break;
					case 2:assert(0&&"Bad inverse function");break;
					}
					return true;
				}

				Generic *ConstructShallowCopy() const override
				{
					return new BinaryInversible(*this);
				}
			};
		};


		template <class RETURN, class ARG1, class ARG2>
		void AddBinary(Parser::RepositoryBuilder& pack, const char *symbol, RETURN(*func)(ARG1,ARG2),const char *arglist = 0, const char *comment = 0)
		{
			using namespace Invariant;
			if (arglist == 0) arglist = "a b";
			pack.AddFunction(symbol,
				new BinaryPluggable<RETURN,ARG1,ARG2,Native::Nop>
				(func,symbol,GenericFirst::New(GenericArgument::New()),GenericRest::New(GenericArgument::New())),
				arglist,comment);
		}


		template <class RETURN, class ARG1, class ARG2, int UTILOPCODE = Native::Nop>
		Generic* MakeBinaryInversible(const char *symbol, 
			RETURN(*func)(ARG1,ARG2),
			ARG1(*inv_l)(RETURN,ARG2),
			ARG2(*inv_r)(RETURN,ARG1),
			CGRef a, CGRef b)
		{
			using namespace Invariant;
			return new BinaryInversible<RETURN,ARG1,ARG2,UTILOPCODE>
				(func,inv_l,inv_r,symbol,a,b);
		}

		void AddBinary(Parser::RepositoryBuilder& pack, const char *symbol, CGRef graph, const char *arglist, const char *comment)
		{
			if (arglist == 0) arglist = "a b";
			pack.AddFunction(symbol,graph,arglist,comment);
		}

		template <class RETURN, class ARG1, class ARG2>
		void AddBinaryInversible(Parser::RepositoryBuilder& pack, const char *symbol, 
			RETURN(*func)(ARG1,ARG2),
			ARG1(*inv_l)(RETURN,ARG2),
			ARG2(*inv_r)(RETURN,ARG1),
			const char *arglist = 0, const char *comment = 0)
		{
			using namespace Invariant;
			AddBinary(pack,symbol,MakeBinaryInversible<RETURN,ARG1,ARG2>
				(func,inv_l,inv_r,symbol,
				GenericFirst::New(GenericArgument::New()),
				GenericRest::New(GenericArgument::New())),arglist,comment);

		}


		template <class RETURN,class ARG> 
		void AddUnary(Parser::RepositoryBuilder& pack, const char *symbol, RETURN(*func)(ARG), const char *arglist = 0, const char *comment = 0)
		{
			using namespace Invariant;
			if (arglist == 0) arglist = "a";
			pack.AddFunction(symbol,
				new UnaryPluggable<RETURN,ARG>(func,symbol,GenericArgument::New())
				,arglist,comment);
		}
	};
};

#include "TypeAlgebra.h"
#include "Evaluate.h"

namespace K3 {
	namespace Nodes {

		template <class A1, class A2, class RET> struct BinaryCallableMetadata {
			typedef A1 lhs_t;
			typedef A2 rhs_t;
			lhs_t lhs;
			rhs_t rhs;
		};

		BinaryCallableMetadata<ivar, ivar, ivar> Inspect(ivar(*lmbd)(ivar, ivar)) {
			return BinaryCallableMetadata<ivar, ivar, ivar>();
		}

		GENERIC_NODE(GenericDecodeString, GenericUnary)
			GenericDecodeString(CGRef str) :GenericUnary(str) {}
		PUBLIC
			static GenericDecodeString* New(CGRef str) { return new GenericDecodeString(str); }
		END

		GENERIC_NODE(GenericEncodeString, GenericUnary)
			GenericEncodeString(CGRef tuple) : GenericUnary(tuple) {}
		PUBLIC
			static GenericEncodeString* New(CGRef str) { return new GenericEncodeString(str); }
		END

		GENERIC_NODE(GenericGetSymbolSource, GenericUnary)
			GenericGetSymbolSource(CGRef path) : GenericUnary(path) {}
		PUBLIC
			static GenericGetSymbolSource* New(CGRef pack) { return new GenericGetSymbolSource(pack); }
		END

		GENERIC_NODE(GenericGetLibraryMetadata, GenericUnary)
			GenericGetLibraryMetadata(CGRef path) : GenericUnary(path) {}
		PUBLIC
			static GenericGetLibraryMetadata* New(CGRef pack) { return new GenericGetLibraryMetadata(pack); }
		END

		GENERIC_NODE(Trace, GenericUnary)
			Trace(CGRef fun) : GenericUnary(fun) {}
		PUBLIC
			static Trace* New(CGRef fn) { return new Trace(fn); }
		END

		GENERIC_NODE(GenericDependency, GenericBinary)
			GenericDependency(CGRef data, CGRef effect) : GenericBinary(data, effect) {}
		PUBLIC
			static GenericDependency* New(CGRef data, CGRef effect) { return new GenericDependency(data, effect); }
		END

		GENERIC_NODE(GenericKeyToType, GenericUnary)
			GenericKeyToType(CGRef key) : GenericUnary(key) {}
		PUBLIC
			static GenericKeyToType* New(CGRef key) { return new GenericKeyToType(key); }
		END

		Specialization GenericKeyToType::Specialize(SpecializationState& t) const {
			SPECIALIZE(t, key, GetUp(0));
			auto ty = TLS::GetCurrentInstance()->Recall(key.result.GetInternalTag());
			std::vector<char> zeroes(ty.GetSize());
			return Specialization(Native::Constant::New(ty, zeroes.data()), ty);
		}

		Specialization GenericDependency::Specialize(SpecializationState& t) const {
			SPECIALIZE(t, val, GetUp(0));
			SPECIALIZE(t, effect, GetUp(1));
			return Specialization(Deps::New(val.node, effect.node), val.result.Fix());
		}

		Specialization GenericSpecializationCallback::Specialize(SpecializationState& t) const {
			SPECIALIZE(t, key, GetUp(0));
			auto val = t(GetUp(1)); // also monitor specialization failure
			auto valTy = val.result.Fix(Type::GenerateNoRules);
			Type keyTy = key.result.Fix(Type::GenerateNoRules);

			std::stringstream keyStr;
			if (keyTy.IsPair()) {
				valTy = Type::Pair(keyTy.Rest(), valTy);
				keyStr << keyTy.First();
			} else {
				keyStr << keyTy;
			}

			auto typeUid = val.node ? TLS::GetCurrentInstance()->Memoize(valTy) : 0;
			// void* to int64 conversion
			TLS::GetCurrentInstance()->SpecializationCallback(t.GetRep().IsActive(), keyStr.str(), valTy, (std::int64_t)typeUid);

			if (val.node == nullptr) {
				if (val.result.GetDescriptor() == &K3::SpecializationFailure) return val;
				// strip error details coming downstream
				if (!val.result.IsUserType() ||
					val.result.GetDescriptor() != &MonitoredError) {
					val.result = Type::User(&MonitoredError, keyTy);
				}
			}
			
			return val;
		}


		Specialization GenericGetSymbolSource::Specialize(SpecializationState& t) const {
			SPECIALIZE(t, package, GetUp(0));
			if (package.result.IsInvariantString() == false) {
				t.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::TypeMismatchInSpecialization, package.result, "Symbol identifier must be a string");
				return SpecializationFailure();
			}
			std::stringstream ss;
			ss << package.result;

			INTERNAL_ERROR("todo");
			/*		auto sym = K3::TLS::GetCurrentInstance()->GetRepository()->Get().Find(ss.str());

					if (sym) {
						auto str = sym->Source;
						if (str.empty()) str = "; no source available";
						return Specialization(Typed::Nil(), Type(str.c_str()));
					} else {
						t.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::SymbolNotFound, package.result, "Symbol not found");
						return SpecializationFailure();
					}*/
		}


		Specialization GenericGetLibraryMetadata::Specialize(SpecializationState& t) const {
			SPECIALIZE(t, package, GetUp(0));
			if (package.result.IsInvariantString() == false) {
				t.GetRep().Diagnostic(Verbosity::LogErrors, this, Error::TypeMismatchInSpecialization, package.result, "Package identifier must be a string");
				return SpecializationFailure();
			}
			std::stringstream ss;
			ss << package.result;
			return Specialization(Typed::Nil(), TLS::GetRepositoryMetadata(ss.str()));
		}

		struct cps {
			virtual std::unique_ptr<cps> operator()(cps& c) = 0;
			virtual operator bool() const = 0;
		};

		using cc = std::unique_ptr<cps>;

		template <typename FN> struct cps_erased : cps {
			FN f;
			cps_erased(FN&& f) :f(std::move(f)) {}
			cc operator()(cps& c) { return f(c); }
			operator bool() { return f; }
		};

		template <typename F> static cc make_cps(F&& f) {
			return new cps_erased<F>(std::forward<F>(f));
		}

		static Type GetXmlAttributes(TiXmlElement* elt) {
			Type t(false);
			for (auto a = elt->FirstAttribute();a;a = a->Next()) {
				t = Type::Tuple(
					Type::InvariantString(abstract_string::cons(a->Name())),
					Type::InvariantString(abstract_string::cons(a->Value())), t);
			}
			return t;
		}

		static Type EvalNode(TiXmlElement* elt);

		std::vector<TiXmlElement*> GetChildren(TiXmlElement* parent, const char *name) {
			std::vector<TiXmlElement*> elts;
			for (auto e = parent->FirstChildElement(name);e;e = e->NextSiblingElement(name)) {
				elts.push_back(e);
			}
			std::reverse(elts.begin(), elts.end());
			return elts;
		}

		static Type FormNode(TiXmlElement* elt) {
			auto fe = elt->FirstChildElement("eval");
			auto ferr = elt->FirstChildElement("err");

			Type t;
			if (fe) {
				for (auto fe : GetChildren(elt, "eval")) {
					t = Type::Pair(EvalNode(fe), t);
				}
			}

			if (ferr) {
				// WTF??
				for (auto fe : GetChildren(elt, "err")) {
                    (void)fe;
					int code;
					const char* position;
					ferr->Attribute("c", &code);
					position = (const char*)strtoull(ferr->Attribute("at"), nullptr, 16);
					auto pos = TLS::GetModuleAndLineNumberText(position,nullptr);
					t = Type::Pair(Type::Tuple(Type::InvariantI64(code),
											   Type::InvariantString(abstract_string::cons(pos.c_str())),
											   Type::InvariantString(abstract_string::cons(ferr->GetText())),
											   Type::Nil), t);
				}
			}

			return t;
		}

		static Type EvalNode(TiXmlElement* elt) {
			Type t(false);
			for (auto c : GetChildren(elt, "form")) {
				auto f = FormNode(c);
				if (f.IsNil() == false)
					t = Type::Pair(f, t);
			}

			const char *name = elt->Value();
			if (elt->Attribute("label")) name = elt->Attribute("label");

			auto derivation = elt->FirstChildElement("td");
			if (derivation) {
				t = Type::Pair(Type::Tuple(
					Type::InvariantString(abstract_string::cons(derivation->Attribute("a"))),
					Type::InvariantString(abstract_string::cons(derivation->Attribute("r")))),
					t);
			} else t = Type::Pair(Type(), t);

			return Type::Pair(Type::InvariantString(abstract_string::cons(name)), t);
		}

		Specialization Trace::Specialize(SpecializationState& t) const {
			SPECIALIZE(t, fn, GetUp(0));

			RegionAllocator trace;
			auto eval = Evaluate::New("trace", GenericArgument::New(), Invariant::Constant::New(Type::Nil));

			std::stringstream log;
			log << "<form>";
			SpecializationDiagnostic diags(&log, LogEverything);
			SpecializationTransform spec(eval, fn.result, diags, SpecializationState::Normal);
			spec.Go();
			log << "</form>";

			TiXmlDocument traceXml;
			traceXml.Parse(log.str().c_str());

			if (traceXml.Error()) {
				INTERNAL_ERROR("Bad specialization trace");
			}

			return Specialization(Typed::Nil(), FormNode(traceXml.RootElement()));
		}

		Specialization GenericDecodeString::Specialize(SpecializationState& t) const {
			SPECIALIZE_ARGS(t, 0);
			if (A0.result.IsInvariantString()) {
				std::stringstream s;
				A0.result.GetString()->stream(s);
				Type result(false);
				auto unicode = ToUnicode(s.str());
				for (auto u = unicode.rbegin(); u != unicode.rend(); ++u) {
					result = Type::Pair(Type::InvariantU64(*u), result);
				}
				return Specialization(Typed::Nil(), result);
			} else {
				t.GetRep().Diagnostic(LogWarnings, this, Error::TypeMismatchInSpecialization, "Requires a string parameter");
				return SpecializationFailure();
			}
		}
	}
		
	void BuildInvariantStringOps(Parser::RepositoryBuilder pack)
	{
		using namespace K3::Nodes;
		using namespace K3::Nodes::Invariant;

		AddBinary<istr,istr,istr>(pack,"Append",[](istr a, istr b) -> istr {return SString::append(a,b);},"a b","Append invariant string b to a");
		AddBinary<istr,istr,ivar>(pack,"Take",[](istr a, ivar b) -> istr {return a->take(b.ToInt());},"str num-chars","Take num-chars first characters from string");
		AddBinary<istr,istr,ivar>(pack,"Skip",[](istr a, ivar b) -> istr {return a->skip(b.ToInt());},"str num-chars","Skip num-chars first characters of string");
		AddUnary<istr, Any>(pack, "Convert", [](Any a) -> istr { std::stringstream ss; a.Fix().OutputText(ss); return abstract_string::cons(ss.str().c_str());}, "val", "obtain the string representation of the type of 'var'");
		AddUnary<istr, Any>(pack, "Interop-Format", [](Any a) -> istr {std::stringstream ss; a.Fix().OutputFormatString(ss, true); return abstract_string::cons(ss.str().c_str()); }, "val", "obtain a type descriptor string for native interop");
		AddUnary<istr, Any>(pack, "Interop-Format-JSON", [](Any a) -> istr { std::stringstream ss; a.Fix().OutputJSONTemplate(ss, false); return abstract_string::cons(ss.str().c_str()); }, "val", "obtain a JSON template descriptor string for native interop");
		AddUnary<ivar,istr>(pack,"Length",[](istr a){return ivar((ttmath::uint)a->utf8len());},"str","Compute the length of a string");
		AddBinary<ivar,istr,istr>(pack,"Find",[](istr a,istr b){return ivar((int)a->find(b));},"text pattern","Return the first character offset in 'text' where 'pattern' occurs or negative if no matches");

		pack.AddFunction("Decode",
			GenericDecodeString::New(GenericArgument::New()), "string", "returns a list of Unicode code points in string");
	}

	namespace Nodes
	{
		namespace Invariant
		{
			CGRef CmpEq(CGRef a, CGRef b) {
				return new BinaryPluggable<bool,ivar,ivar,Native::Nop>([](ivar a, ivar b) {return a==b;},"CmpEQ",a,b);
			}

			CGRef CmpGt(CGRef a, CGRef b) {
				return new BinaryPluggable<bool,ivar,ivar,Native::Nop>([](ivar a, ivar b) {return a>b;},"CmpGT",a,b);
			}

			CGRef CmpLt(CGRef a, CGRef b) {
				return new BinaryPluggable<bool,ivar,ivar,Native::Nop>([](ivar a, ivar b) {return a<b;},"CmpLT",a,b);
			}

			CGRef CmpGe(CGRef a, CGRef b) {
				return new BinaryPluggable<bool,ivar,ivar,Native::Nop>([](ivar a, ivar b) {return a>=b;},"CmpGE",a,b);
			}

			CGRef CmpLe(CGRef a, CGRef b) {
				return new BinaryPluggable<bool,ivar,ivar,Native::Nop>([](ivar a, ivar b) {return a<=b;},"CmpLE",a,b);
			}

			CGRef Not(CGRef a) {
				return new UnaryPluggable<bool,bool>([](bool a){return !a;},"Not",a);
			}

			CGRef Custom(ivar (*func)(ivar, ivar), CGRef a, CGRef b) {
				return new BinaryPluggable<ivar,ivar,ivar,Native::Nop>(func,"custom",a,b);
			}

			CGRef Custom(bool (*func)(ivar, ivar), CGRef a, CGRef b) {
				return new BinaryPluggable<bool,ivar,ivar,Native::Nop>(func,"custom",a,b);
			}

			CGRef Custom(ivar (*func)(ivar), CGRef a) {
				return new UnaryPluggable<ivar,ivar>(func,"custom",a);
			}

		};
	};

	void BuildInvariantLogic(Parser::RepositoryBuilder& pack)
	{
		using namespace K3::Nodes;
		using namespace K3::Nodes::Invariant;

		AddBinary<bool,Type,Type>(pack,"Greater",[](Type a, Type b){return a>b;},"a b","Compare two invariant constants and yield #True if a is greater");
		AddBinary<bool,Type,Type>(pack,"Less",[](Type a, Type b){return a<b;},"a b","Compare two invariant constants and yield #True if a is less");
		AddBinary<bool,Type,Type>(pack,"Greater-Equal",[](Type a, Type b){return a>=b;},"a b","Compare two invariant constants and yield #True if a is greater or equal");
		AddBinary<bool,Type,Type>(pack,"Less-Equal",[](Type a, Type b){return a<=b;},"a b","Compare two invariant constants and yield #True if a is less or equal");
		AddBinary<bool,Type,Type>(pack,"Equal",[](Type a, Type b){return a==b;},"a b","Compare two invariant constants and yield #True if equal");
		AddBinary<bool,Type,Type>(pack,"Not-Equal",[](Type a, Type b){return a!=b;},"a b","Compare two invariant constants and yield #True if not equal");		

		AddBinary<bool,bool,bool>(pack,"And",[](bool a, bool b){return a&&b;},"a b","Yield #True if both a and b are #True");
		AddBinary<bool,bool,bool>(pack,"Or",[](bool a, bool b){return a||b;},"a b","Yield #True if either a or b is #True");
		AddBinary<bool,bool,bool>(pack,"Xor",[](bool a, bool b){return a^b?true:false;},"a b","Perform exclusive or operation on a and b");
		AddUnary<bool,bool>(pack,"Not",[](bool a){return !a;},"a","Yield #True if a is nil and vice versa");
	}

	namespace Nodes
	{
		namespace Invariant
		{
			Specialization GenericSCEVAdd::Specialize(SpecializationState& st) const {
				SPECIALIZE(st, a, GetUp(0));
				return Specialization(Typed::Nil(), a.result + Type((std::int64_t)delta));
			}

			Specialization GenericSCEVForceInvariant::Specialize(SpecializationState& st) const {
				// this node controls the behavior of sequence length arithmetic vs invariant SCEV
				SPECIALIZE(st, a, GetUp(0));
				if (a.result.IsInvariant()) return a;
				else return Specialization(Typed::Nil(), Type::InvariantI64(0));
			}

			CGRef Add(CGRef a, CGRef b)
			{
				return MakeBinaryInversible<Type,Type,Type>("Add",
					[](Type a, Type b){return a+b;},
					[](Type d, Type r){return d-r;},
					[](Type d, Type l){return d-l;},
					a,b);
			}

			CGRef Sub(CGRef a, CGRef b)
			{
				return MakeBinaryInversible<Type,Type,Type>("Sub",
					[](Type a, Type b){return a-b;},
					[](Type d, Type b){return d+b;},
					[](Type d, Type a){return a-d;},
					a,b);
			}

			CGRef Mul(CGRef a, CGRef b)
			{
				return MakeBinaryInversible<ivar,ivar,ivar>("Mul",
					[](ivar a, ivar b){return a*b;},
					[](ivar d, ivar r){return d/r;},
					[](ivar d, ivar l){return d/l;},
					a,b);
			}

			CGRef Div(CGRef a, CGRef b)
			{
				return MakeBinaryInversible<ivar,ivar,ivar>("Div",
					[](ivar a, ivar b){return a/b;},
					[](ivar d, ivar r){return d*r;},
					[](ivar d, ivar l){return l/d;},
					a,b);
			}
		}

		namespace Util
		{
			using namespace Nodes::Invariant;
			CGRef Add(CGRef a, CGRef b)
			{
				// could return a rule generator!
				return MakeBinaryInversible<Type,Type,Type,(int)Native::Add>("Add",
					[](Type a, Type b){return a+b;},
					[](Type d, Type r){return d-r;},
					[](Type d, Type l){return d-l;},
					a,b);
			}

			CGRef Sub(CGRef a, CGRef b)
			{
				return MakeBinaryInversible<ivar,ivar,ivar,(int)Native::Sub>("Sub",
					[](ivar a, ivar b){return a-b;},
					[](ivar d, ivar b){return d+b;},
					[](ivar d, ivar a){return a-d;},
					a,b);
			}

			CGRef Mul(CGRef a, CGRef b)
			{
				return MakeBinaryInversible<ivar,ivar,ivar,(int)Native::Mul>("Mul",
					[](ivar a, ivar b){return a*b;},
					[](ivar d, ivar r){return d/r;},
					[](ivar d, ivar l){return d/l;},
					a,b);
			}

			CGRef Div(CGRef a, CGRef b)
			{
				return MakeBinaryInversible<ivar,ivar,ivar,(int)Native::Div>("Div",
					[](ivar a, ivar b){return a/b;},
					[](ivar d, ivar r){return d*r;},
					[](ivar d, ivar l){return l/d;},
					a,b);
			}
		};
	};

	void BuildInvariantArithmetic(Parser::RepositoryBuilder& pack)
	{
		using namespace K3::Nodes;
		using namespace K3::Nodes::Invariant;
		auto Arg(GenericArgument::New());

		AddBinary(pack,"Add",Nodes::Invariant::Add(
			GenericFirst::New(Arg),
			GenericRest::New(Arg)),
			"a b","Add two invariant constants");

		AddBinary(pack,"Sub",Nodes::Invariant::Sub(
			GenericFirst::New(Arg),
			GenericRest::New(Arg)),
			"a b","Substract two invariant constants");

		AddBinary(pack,"Mul",Nodes::Invariant::Mul(
			GenericFirst::New(Arg),
			GenericRest::New(Arg)),
			"a b","Multiply two invariant constants");

		AddBinary(pack,"Div",Nodes::Invariant::Div(
			GenericFirst::New(Arg),
			GenericRest::New(Arg)),
			"a b","Divide two invariant constants");
	}


	void BuildInvariantPrimitiveOps(Parser::RepositoryBuilder& pack)
	{
		BuildInvariantArithmetic(pack);
		BuildInvariantLogic(pack);

		using namespace K3::Nodes;
		using namespace K3::Nodes::Invariant;
		auto Arg(GenericArgument::New());

		auto propagate = GenericPropagateFailure::New();

		AddBinary(pack,"Make",GenericMake::New(
			GenericFirst::New(Arg),
			GenericRest::New(Arg)),
			"type-tag contents","Wraps a raw tuple into a user type.");
		
		pack.AddFunction("Break", propagate);
		AddBinary(pack,"Break",GenericBreak::New(
			GenericFirst::New(Arg),
			GenericRest::New(Arg)),
			"type-tag instance","Unwraps an user type into a raw tuple.");

		AddBinary(pack,"Arity",Invariant::Arity::New(Arg),"tuple","Returns the arity of the argument.");

		pack.AddFunction("Require", propagate);
		AddBinary(pack,"Require",Invariant::GenericRequire::New(
						GenericFirst::New(Arg),
						GenericRest::New(Arg)),
						"required pass-through","Returns 'pass-through', unless specialization of 'required' fails, in which case the failure is propagated.");

		AddBinary(pack, "Debug-Trace", Invariant::GenericTrace::New(
			GenericFirst::New(Arg),
			GenericRest::New(Arg)),
			"label pass-through", "");

		AddBinary(pack, "Class-of", Invariant::GenericClassOf::New(Arg),
						"instance","Returns a type tag describing the type class of 'instance'");

		auto typeCmp = [Arg](int expectOrdinal) {
				return Invariant::GenericOrdinalCompareType::New(
					expectOrdinal,
					GenericFirst::New(Arg),
					GenericRest::New(Arg));
		};

		AddBinary(pack,"Equal-Type",typeCmp(0),"a b","performs compile time deep comparison of two type structures and returns true if identical");
		AddBinary(pack, "Less-Type", typeCmp(-1), "a b", "provides ordering between two types at compile time");
		AddBinary(pack, "Greater-Type", typeCmp(1), "a b", "provides ordering between two types at compile time");

		pack.AddMacro("Constant",GenericTypeTag::New(&InvariantTag), false);
		BuildInvariantStringOps(pack.AddPackage("String"));

//		AddBinary(pack, "Get-Library-Metadata", GenericGetLibraryMetadata::New(Arg), "package", "");
//		AddBinary(pack, "Get-Symbol-Source", GenericGetSymbolSource::New(Arg), "symbol", "");
		AddBinary(pack, "Specialization-Trace", Trace::New(Arg), "fn", "");

		pack.AddMacro("Reject-All-Forms", GenericNoFallback::New(), false);
		pack.AddMacro("Pattern-Match-Failure", GenericPropagateFailure::New(), false);

		AddBinary(pack, "Resolve", Lib::Symbol::New(Arg), "symbol", "Tries to retrieve 'symbol' from the code repository");

		pack.AddFunction("With-Binding", GenericRebindSymbol::New(
			GenericFirst::New(Arg),
			GenericFirst::New(GenericRest::New(Arg)),
			GenericRest::New(GenericRest::New(Arg))), "qualified-name binding closure",
			"Evaluates 'closure' in an environment where the symbol that corresponds to 'qualified-name' "
			"is bound to 'binding'. The original binding is restored afterwards.");

		AddBinary(pack, "Specialization-Monitor", GenericSpecializationCallback::New(GenericFirst::New(Arg), GenericRest::New(Arg)), "", "");

		AddBinary(pack, "Effect-Dependency", GenericDependency::New(GenericFirst::New(Arg), GenericRest::New(Arg)), "value effect", "");

		AddUnary<Any, Any>(pack, "Type-to-Key", [](Any t) -> Any {
			return Type::InternalUse(TLS::GetCurrentInstance()->Memoize(t));
		}, "value", "Make a unique key representing the type of 'value'");

		AddBinary(pack, "Key-to-Type", GenericKeyToType::New(Arg), "key", "make a zero-bit instance with the type associated with 'key'");

		//AddUnary<Any, Any>(pack, "Key-to-Type", [](Any t) -> Any {
		//	return TLS::GetCurrentInstance()->Recall(t.GetInternalTag());
		//});

	}
};


