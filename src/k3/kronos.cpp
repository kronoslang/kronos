#include "driver/CmdLineOpts.h"
#include "common/PlatformUtils.h"
#include "TLS.h"
#include "Type.h"
#include "Parser.h"
#include "kronos.h"
#include "Invariant.h"
#include "config/system.h"
#include "LibraryRef.h"
#include "Evaluate.h"

#include <sstream>
#include <unordered_set>
#include <iomanip>

#ifdef HAVE_LLVM
#include "backends/LLVM.h"
#endif

#ifdef HAVE_BINARYEN
#include "backends/Binaryen.h"
#endif

namespace {
	using namespace Kronos;
	using namespace K3;
	using namespace K3::Nodes;
	using namespace K3::Parser;

	struct RuntimeError : public Kronos::IError {
		std::string msg;
		Error::Code code;
		RuntimeError(Error::Code code, const std::string msg) :code(code), msg(msg) {}

		const char* GetErrorMessage() const noexcept override {
			return msg.c_str();
		};

		ErrorType GetErrorType() const noexcept override {
			return ErrorType::RuntimeError;
		}
		const char* GetSourceFilePosition() const noexcept override {
			return nullptr;
		}

		int GetErrorCode() const noexcept override {
			return (int)code;
		}

		void Delete() noexcept override {
			delete this;
		}

		IError* Clone() const noexcept override {
			return new RuntimeError(*this);
		}
	};


	struct _String : public Kronos::IStr {
		std::string str;
		_String(std::string s):str(std::move(s)) { }
		void Delete() noexcept override { delete this; }
		_String* Clone() noexcept { return new _String(str); }
		const char *c_str() const noexcept override { return str.c_str(); }
		const char *data() const noexcept override { return str.data(); }
		size_t size() const noexcept override { return str.size(); }
		virtual _String* Clone() const noexcept override { return new _String(*this); }
	};

	struct _Streambuf : public std::streambuf {
		IStreamBuf *sb;
		_Streambuf(IStreamBuf* sb):sb(sb) { }
		int overflow(int c) override {
			if (c == EOF) return !EOF;
			char ch = traits_type::to_char_type(c);
			if (sb->Write(&ch, 1) < 1) return EOF;
			else return c;
		}

		std::streamsize xsputn(const char *data, std::streamsize n) override {
			return sb->Write(data, n);
		}

		int sync() override {
			return sb->Sync();
		}

		std::streampos seekoff(streamoff off, ios_base::seekdir dir, ios_base::openmode mode) override {
			return sb->Seek(off,dir,mode);
		}
	};

	static thread_local IError* lastError = nullptr;

	template <typename T>
	struct ErrorHandler {
		T operator()(T v) const { return v; }
	};

	template <typename T> 
	struct ErrorHandler<Err<T>> {
		T operator()(Err<T> e) const {
			if (e.err) {
				if (lastError) {
					std::cerr << "Error status wasn't cleared: " << lastError->GetErrorMessage() << "\n";
					INTERNAL_ERROR("Unhandled error in the API boundary");
				}
				lastError = e.err->Clone();
				return T(0);
			} else {
				return *e;
			}
		}
	};

	template <typename FUNCTOR> auto XX(FUNCTOR f) {
		using namespace Kronos;
		using TValue = decltype(f());
		ErrorHandler<TValue> handler;
		auto result = f();
		return handler(std::move(result));
	}

	struct TypeImpl : public IType, public AtomicRefCounting {
		K3::Type t;
		TypeImpl(K3::Type tp):t(std::move(tp)) { }

		void Attach() const noexcept override { AtomicRefCounting::Attach(); }
		void Detach() const noexcept override { AtomicRefCounting::Detach(); }

		virtual IStr* _AsString() const noexcept override { return XX([&](){
			stringstream str; 
			t.OutputText(str); 
			return new _String(str.str()); }); 
		}

		virtual void ToStream(IStreamBuf& sb, const void *dataInstance, int printMode) const noexcept override {
			XX([&](){
				_Streambuf sbuf(&sb);
				std::ostream strm(&sbuf);
				switch (printMode) {
				case WithParens:
					t.OutputText(strm, dataInstance, true); break;
				case WithoutParens:
					t.OutputText(strm, dataInstance, false); break;
				case JSON:
					t.OutputJSONTemplate(strm); break;
				}
				return 0;
			});
		}

		virtual IType* _GetUserTypeContent() const noexcept override { return XX([&](){ return new TypeImpl(t.UnwrapUserType()); }); }
		virtual IType* _GetUserTypeTag() const noexcept override { return XX([&](){ return new TypeImpl(K3::Type(t.GetDescriptor())); }); }
		virtual IType* _GetFirst() const noexcept override { return XX([&](){ return new TypeImpl(t.First()); }); }
		virtual IType* _GetRest() const noexcept override { return XX([&](){ return new TypeImpl(t.Rest()); }); }
		virtual const K3::Type& GetPimpl() const noexcept override { return t; }
		virtual bool IsUserType() const noexcept override { return t.IsUserType(); }
		virtual bool IsPair() const noexcept override { return t.IsPair(); }
		virtual bool IsTrue() const noexcept override { return t.IsTrue(); }
		virtual bool IsNil() const noexcept override { return t.IsNil(); }
		virtual bool IsFloat32() const noexcept override { return t.IsFloat32(); }
		virtual bool IsFloat64() const noexcept override { return t.IsFloat64(); }
		virtual bool IsInt32() const noexcept override { return t.IsInt32(); }
		virtual bool IsInt64() const noexcept override { return t.IsInt64(); }
		virtual bool IsConstant() const noexcept override { return t.IsInvariant(); }
		virtual bool IsString() const noexcept override { return t.IsInvariantString(); }
		virtual double AsConstantF() const noexcept override { return XX([&](){return t.GetInvariant(); }); }
		virtual std::int64_t AsConstantI() const noexcept override { return XX([&]() {return t.GetInvariantI64(); }); }
		virtual bool AsBool() const noexcept override { return XX([&](){return t.GetTrueOrNil(); }); }
		virtual size_t SizeOf() const noexcept override { return t.GetSize(); }
		virtual size_t GetHash() const noexcept override { return t.GetHash(); }
		virtual IType& operator=(const IType& r) noexcept override { t = r.GetPimpl(); return *this; }
		virtual bool operator<(const Kronos::IType& r) const noexcept override { return t < r.GetPimpl(); }
		virtual bool operator==(const Kronos::IType& r) const noexcept override { return t == r.GetPimpl(); }
		virtual TypeImpl* Clone() const noexcept override { return new TypeImpl(t); }
		virtual void Delete() const noexcept override { delete this; }
	};
            
    struct GenericGraphImpl : public Kronos::IGenericGraph, public RefCounting, public Graph<Generic> {
        virtual void Delete() const noexcept override  { delete this; }
        virtual void Attach() const noexcept override  { RefCounting::Attach(); }
        virtual void Detach() const noexcept override  { RefCounting::Detach(); }
        virtual CGRef Get() const noexcept override { return *this; }
		virtual IType* AsType() const noexcept override {
			return new TypeImpl(K3::Type((CGRef)*this));
		}
		virtual bool Equal(const IGenericGraph* g) const noexcept override {
			return XX([&]() { return *this == *(GenericGraphImpl*)g;});
		}

		virtual std::uint64_t GetGraphHash() const noexcept override {
			return std::hash<Graph>()(*this);
		}

		virtual IGenericGraph* _Compose(const IGenericGraph *inner) const noexcept override {
			RegionAllocator composition;
			Transform::Identity<const Generic> copy(*((GenericGraphImpl*)inner));

			struct compose : public Transform::Identity<const Generic> {
				CGRef inner;
				compose(CGRef outer, CGRef inner) :Identity(outer), inner(inner) {}
				CGRef operator()(CGRef node) {
					Lib::Reference* ref;
					if (node->Cast(ref)) {
						for (auto &lu : ref->GetLookupSequence()) {
							if (lu == "arg") return inner;
						}
					} else if (IsOfExactType<GenericArgument>(node)) {
						return inner;
					}
					return node->IdentityTransform(*this);
				}
			} comp(*this, copy.Go());
			return new GenericGraphImpl(comp.Go());
		}

        GenericGraphImpl(CGRef g):Graph(g) {}
    };

    struct TypedGraphImpl : public Kronos::ITypedGraph, public RefCounting, public Graph<Typed> {
        void Delete() const noexcept override { delete this; }
        void Attach() const noexcept override { RefCounting::Attach(); }
        void Detach() const noexcept override { RefCounting::Detach(); }
        CTRef Get() const noexcept override { return *this; }
        K3::Type a, r;
        TypedGraphImpl(CTRef g, const K3::Type& arg, const K3::Type& res):Graph(g),a(arg),r(res) {}
        IType* _TypeOfResult() const noexcept override { return new TypeImpl(r); }
        IType* _TypeOfArgument() const noexcept override { return new TypeImpl(a); }
        const K3::Type* _InternalTypeOfResult() const noexcept override { return &r; }
        const K3::Type* _InternalTypeOfArgument() const noexcept override { return &a; }
    };

           
	struct ContextImpl : public Kronos::IContext, public K3::TLS, public RefCounting {
		virtual void Delete() const noexcept override  { delete this; }
		virtual void Attach() const noexcept override  { RefCounting::Attach(); }
		virtual void Detach() const noexcept override  { RefCounting::Detach(); }
		std::vector<const char*> ipaths;

		ContextImpl(ModulePathResolver res, void *user):TLS(res, user) {
		}

		void _SetDefaultRepository(const char* package, const char* version) override {
			codebase.SetCoreLib(package, version);
		}

		const char* _GetCoreLibPackage() override {
			return codebase.GetCoreLibPackage().c_str();
		}

		const char* _GetCoreLibVersion() override {
			return codebase.GetCoreLibVersion().c_str();
		}

		void _SetCompilerDebugTraceFilter(const char *flt) noexcept override {
			SetCompilerDebugTraceFilter(flt);
		}
        
        virtual void _Parse(const char *source, bool REPLMode, ImmediateExpressionHandler handler, void* userdata) noexcept override {
            XX([&](){
                ScopedContext scope(*this);
                RegionAllocator parsedNodes;
				return codebase.ImportBuffer(source, true, [handler, userdata](const char* sym, CGRef imm) mutable {
                    handler(userdata, sym, Ref<GenericGraphImpl>::Cons(imm));
                });
            });
        }

		IStr* _GetResolutionTrace(void) const noexcept override {
			return XX([&]() {
				std::stringstream syms;
				for (auto &s : GetResolutionTrace()) {
					syms << s << " ";
				};
				return new _String(syms.str());
			});
		}

		IStr* _DrainRecentSymbolChanges(void) noexcept override {
			return XX([&]() {
				std::stringstream changes;
				for (auto& key : DrainRecentChanges()) {
					changes << key << " ";
				};
				return new _String(changes.str());
			});
		}

		void _RegisterSpecializationCallback(const char* str, SpecializationCallbackHandler callback, void *user) noexcept override {
			RegisterSpecilizationCallback(str, [callback, user](bool diags, const K3::Type& t, std::int64_t uid) {
				TypeImpl ti(t);
				callback(user, diags ? 1 : 0, &ti, uid);
			});
		}

		void SetAssetLinker(AssetLinker al, void* user) noexcept override {
			SetAssetLoader([al, user](const char* url, K3::Type& t) -> void* {
				const IType* assetType = nullptr;
				void* ptr = al(url, &assetType, user);
				t = assetType->GetPimpl();
				assetType->Release();
				return ptr;
			});
		}
        
        virtual const ITypedGraph* _Specialize(const IGenericGraph* GAST, const IType& argument, IStreamBuf* _log, int logLevel) noexcept override {
            return XX([&]() -> Err<ITypedGraph*> {
                ScopedContext scope(*this);
                RegionAllocator buildAllocator;
            
                _Streambuf logbuf(_log);
                std::ostream log(&logbuf);
				SpecializationDiagnostic diags(_log ? &log : nullptr, Verbosity( (int)Verbosity::LogErrors - logLevel ));
                SetSpecializationCache(new SpecializationCache);
                
                auto RootBlock(diags.Block(LogTrace,"Specialization"));
                
				ClearResolutionTrace();
				Specialization spec(SpecializationTransform(GAST->Get(), argument.GetPimpl(), diags, SpecializationState::Normal).Go());
                if (spec.node == nullptr) {
					std::stringstream ss;
					ss << spec.result;
                    return TypeError(Error::SpecializationFailed, ss.str().c_str(), GAST->Get()->GetRepositoryAddress());
                }
                return new TypedGraphImpl(spec.node, argument.GetPimpl(), spec.result);
            });
        }

		void SetModulePathResolver(Kronos::ModulePathResolver re, void *user) noexcept override {
			TLS::SetModuleResolver(re, user);
		}

		KRONOS_INT _GetLibraryMetadataAsJSON(IStreamBuf* buf) override {
			return XX([&]() {
				_Streambuf jsonBuf(buf);
				std::ostream json(&jsonBuf);
				codebase.ExportMetadata(json);
				return 1;
			});
		}


		std::unordered_set<std::string> llvmExtensions{
			".s", ".asm", ".obj", ".o", ".ll", ".bc"
		};

		std::unordered_set<std::string> binaryenExtensions{
			".wasm", ".wast", ".js"
		};

		virtual KRONOS_INT _AoT(
			const char* prefix,
			const char* fileType,
			IStreamBuf* object,
			const char* engine,
			const ITypedGraph* itg,
			const char* triple,
			const char* mcpu,
			const char* march,
			const char* targetFeatures,
			BuildFlags flags)  noexcept  override {
			return XX([&]() -> Err<int> {
				this->flags = flags;
				K3::ScopedContext scope(*this);
				std::string eng(engine);
				_Streambuf objbuf(object);
				std::ostream obj(&objbuf);

				if (eng == "") {
					if (false);
#ifdef HAVE_LLVM
					else if (llvmExtensions.count(fileType)) eng = "llvm";
#endif
#ifdef HAVE_BINARYEN
					else if (binaryenExtensions.count(fileType)) eng = "binaryen";
#endif
					else {
						using namespace std::string_literals;
						return RuntimeError(Error::BadInput, "Could not determine backend code generator for "s + fileType);
					}
				}

				if (eng == "llvm") {
#ifdef HAVE_LLVM
					return K3::Backends::LLVMAoT(prefix, fileType, obj, engine, itg, triple, mcpu, march, targetFeatures, flags);
#else 
					return Error::RuntimeError(Error::BadInput, "Kronos is built without the LLVM backend");
#endif
				} else if (eng == "binaryen") {
#ifdef HAVE_BINARYEN
					return K3::Backends::BinaryenAoT(prefix, fileType, obj, engine, itg, triple, mcpu, march, targetFeatures, flags);
#else
					return Error::RuntimeError(Error::BadInput, "Kronos is built without the Binaryen backend");
#endif
				} 
				return Error::RuntimeError(Error::BadInput, "AoT Compilation engine not recognized");
			});
		}

        virtual krt_class*  _JiT(const char* engine,
                                 const ITypedGraph* itg,
                               BuildFlags flags)  noexcept override {
			this->flags = flags;
			return XX([&]() mutable -> Err<krt_class*> {
                K3::ScopedContext scope(*this);
                std::string eng(engine);
                
                if (eng == "llvm") {
#ifdef HAVE_LLVM
					return K3::Backends::LLVMJiT(engine, itg, flags);
#else 
					return Error::RuntimeError(Error::BadInput, "Kronos is built without the LLVM backend");
#endif
				} 
				/* else if (eng == "WaveCore") {
                    K3::Backends::WaveCore compiler(itg->Get(), *itg->_InternalTypeOfArgument(), *itg->_InternalTypeOfResult());
                    return compiler.Build(flags);
                } */ 
				else return Error::RuntimeError(Error::BadInput, "JiT Compilation engine not recognized");
            });
        }
        
		virtual void _ImportFile(const char *modulePath, KRONOS_INT allowRedefine) noexcept override {
			XX([&]() {
				SetForThisThread();
                return codebase.ImportFile(modulePath, allowRedefine != 0);
			});
		}

		virtual void _ImportBuffer(const char* sourceCode, bool allowRedefinition) noexcept override {
			XX([&]() {
				SetForThisThread();
                return codebase.ImportBuffer(sourceCode, allowRedefinition);
			});
		}

		virtual IStr* _GetModuleAndLineNumberText(const char* codePosition) noexcept override {
			return XX([&]() {
				SetForThisThread();
				return new _String(GetModuleAndLineNumberText(codePosition,nullptr));
			});
		}

		virtual IStr* _ShowModuleLine(const char* codePosition) noexcept override {
			return XX([&]() {
				SetForThisThread();
				std::string line;
				GetModuleAndLineNumberText(codePosition,&line);
				return new _String(std::move(line));
			});
		}

		virtual IType* _TypeFromUID(std::int64_t uid) noexcept override {
			return XX([&]() {
				SetForThisThread();
				return new TypeImpl(Recall((const void*)uid));
			});
		}

		virtual int64_t _UIDFromType(const IType* t) noexcept override {
			return XX([&]() {
				SetForThisThread();
				return (int64_t)Memoize(t->GetPimpl());
			});
		}

		virtual void _BindConstant(const char* name, const IType* ty, const void* data) override {
/*			RegionAllocator alloc;
			SetForThisThread();
			auto val = Invariant::Constant::New(ty->GetPimpl(), data);
			auto cs = codebase.new_changeset(Parser::package_uri{
				std::string("#binding"),
				"",
				name
			}, name, true);
			if (!cs.err) (*cs)->second.def(name, val);*/
		}
	};
}

namespace Kronos {

	KRONOS_ABI IStr* KRONOS_ABI_FN _GetUserPath() noexcept {
		return XX([](){
			return new _String(::GetUserPath());
		});
	}

	KRONOS_ABI IStr* KRONOS_ABI_FN _GetSharedPath() noexcept {
		return XX([&](){
			return new _String(::GetSharedPath());
		});
	}

	KRONOS_ABI IError* KRONOS_ABI_FN _GetAndResetThreadError() noexcept {
		if (lastError) {
			IError *e = lastError;
			lastError = nullptr;
			return e;
		}
		else return nullptr;
	}

	KRONOS_ABI IContext* KRONOS_ABI_FN _CreateContext(ModulePathResolver res, void *user) noexcept {
		return XX([&](){
			return new ContextImpl(res, user);
		});
	}

	KRONOS_ABI const IType* KRONOS_ABI_FN _GetNil( ) noexcept { static TypeImpl nil(K3::Type::Nil); return &nil; }
	KRONOS_ABI const IType* KRONOS_ABI_FN _GetTrue( ) noexcept { static TypeImpl tru(K3::Type::True); return &tru; }
	KRONOS_ABI const IType* KRONOS_ABI_FN _GetFloat32Ty( ) noexcept { static TypeImpl f(K3::Type::Float32); return &f; }
	KRONOS_ABI const IType* KRONOS_ABI_FN _GetFloat64Ty( ) noexcept { static TypeImpl f(K3::Type::Float64); return &f; }
	KRONOS_ABI const IType* KRONOS_ABI_FN _GetInt32Ty( ) noexcept { static TypeImpl i(K3::Type::Int32); return &i; }
	KRONOS_ABI const IType* KRONOS_ABI_FN _GetInt64Ty( ) noexcept { static TypeImpl i(K3::Type::Int64); return &i; }
	KRONOS_ABI IType* KRONOS_ABI_FN _ConvertToABIType(const void *raw) noexcept {
		auto tptr = (K3::Type*)raw;
		return new TypeImpl(*tptr);
	};


	KRONOS_ABI IType* KRONOS_ABI_FN _GetString(const char* str) noexcept {
		return XX([&](){ 
			return new TypeImpl(K3::Type(str));
		});
	}

	KRONOS_ABI IType* KRONOS_ABI_FN _GetConstantF(double v) noexcept {
		return XX([&](){
			return new TypeImpl(K3::Type(v));
		});
	}

	KRONOS_ABI IType* KRONOS_ABI_FN _GetConstantI(std::int64_t v) noexcept {
		return XX([&]() {
			return new TypeImpl(K3::Type(v));
		});
	}

	KRONOS_ABI IType* KRONOS_ABI_FN _GetGraph(const IGenericGraph* igg) noexcept {
		return XX([&]() {
			return new TypeImpl(K3::Type(igg->Get()));
		});
	}

	KRONOS_ABI IType* KRONOS_ABI_FN _GetTuple(const IType& element, size_t count) noexcept {
		return XX([&]() -> Err<IType*> {
			if (count == 0) return RuntimeError("Tuple must have at least 2 elements");
			if (count == 1) return new TypeImpl(element.GetPimpl( ));
			return new TypeImpl(K3::Type::Chain(element.GetPimpl(), count - 1, element.GetPimpl()));
		});
	}

	KRONOS_ABI IType* KRONOS_ABI_FN _GetList(const IType& element, size_t count) noexcept {
		return XX([&]() {
			if (count == 0) return new TypeImpl(K3::Type::Nil);
			return new TypeImpl(K3::Type::Chain(element.GetPimpl(), count, K3::Type::Nil));
		});
	}

	KRONOS_ABI IType* KRONOS_ABI_FN _GetUserType(const IType& tag, const IType &content) noexcept {
		return XX([&]() -> IType* {
			if (!tag.GetPimpl().IsTypeTag()) return nullptr;
			return new TypeImpl(K3::Type::User(tag.GetPimpl().GetDescriptor(), content.GetPimpl()));
		});
	}

	KRONOS_ABI IType* KRONOS_ABI_FN _GetBuiltinTag(int which) noexcept {
#define F(TAG) case TypeTag::TAG: return new TypeImpl(K3::Type(&K3:: TAG ## Tag));
		switch ((TypeTag)which) {
			F(Float32)
			F(Float64)
			F(Int32)
			F(Int64)
			F(Pair)
			F(Vector)
			F(TypeTag)
			F(True)
			F(Nil)
			F(Graph)
			F(Invariant)
			F(InvariantString)
			F(UserType)
			F(Union)
			F(Function)
			F(AudioFile)
		default: return nullptr;
		}
	}

	KRONOS_ABI IType* KRONOS_ABI_FN _GetPair(const IType& fst, const IType& rst) noexcept {
		return XX([&](){
			return new TypeImpl(K3::Type::Pair(fst.GetPimpl(), rst.GetPimpl()));
		});
	}

	KRONOS_ABI const char* KRONOS_ABI_FN GetVersionString( ) noexcept {
		return KRONOS_PACKAGE_VERSION;
	}

	KRONOS_ABI void KRONOS_ABI_FN AddBackendCmdLineOpts(CmdLine::IRegistry& Master) noexcept {
		CmdLine::Registry().AddParsersTo(Master);
	}

	IType* ConvertToABI(const K3::Type &t) {
		return new TypeImpl(t);
	}
}
