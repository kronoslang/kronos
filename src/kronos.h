#pragma once

#include <cstdint>
#include <cassert>

#include <string>
#include <vector>
#include <ostream>
#include <functional>
#include <memory>
#include <stdexcept>
#include <iterator>

#include "kronos_abi.h"

#ifdef __APPLE__
#include <alloca.h>
#endif

namespace Kronos {
	/**
	* Utility: shared reference and value semantics
	*/

	static void _CheckLastError();
	template <typename T> T _CheckResult(T&& v) { if (!v) _CheckLastError(); return std::move(v); }

	template <class C> class Shared;
	template <class C> class Value;

	template <class C> class Pointer {
		friend class Shared<C>;
		friend class Value<C>;
	protected:
		C* i;
		Pointer(C* ptr):i(ptr) { }
	public:
		C* operator->() { return i; }
		const C* operator->() const { return i; }
		operator const C&() const { return *i; }
		operator C&() { return *i; }
		const C& operator*() const { return *i; }
		C& operator*() { return *i; }
		C* Get() noexcept { return i; }
		const C* Get() const { return i; }
		bool Empty() const { return i == nullptr; }
		bool operator==(const Pointer<C>& r) const { return i == r.Get(); }
		bool operator!=(const Pointer<C>& r) const { return i != r.Get( ); }
	};

	template <class C> class Shared : public Pointer<C> {
	protected:
		using Pointer<C>::i;
	public:
		Shared& operator=(Shared s) { std::swap(i, s.i); return *this; }
		Shared(const Shared& src):Pointer<C>(src.i) { if (i) i->Retain(); }
		Shared(Shared&& src):Pointer<C>(src.i) { src.i = nullptr; }
		Shared(C* src = nullptr):Pointer<C>(src) { if (i) i->Retain(); }
		~Shared() { if (i) i->Release(); }
		size_t GetHash() const { return std::hash<const C*>()(this->Get()); }
	};

	template <class C> class Value : public Pointer<C> {
	protected:
		using Pointer<C>::i;
	public:
		Value& operator=(Value<C> s) { std::swap(i, s.i); return *this; }
		Value(const Value<C>& src):Pointer<C>(static_cast<C*>(src?src->Clone():nullptr)) { _CheckLastError(); }
		Value(Value<C>&& src):Pointer<C>(src.i) { src.i = nullptr; }
		Value(C* src = nullptr):Pointer<C>(src) { }
		~Value() { if (i) i->Delete(); }
	};


	template<typename BASE, typename STDEX> class ErrorImpl : public STDEX, public BASE {
	public:
		ErrorImpl(const char *message):STDEX(message) { }
		const char *GetErrorMessage() const noexcept { return STDEX::what(); }
		int GetErrorCode() const noexcept { return -1; }
		const char* GetSourceFilePosition() const noexcept { return ""; }
		IError::ErrorType GetErrorType() const noexcept { return BASE::GetClassErrorType();  }
		void Delete() noexcept { delete this; }
		IError* Clone() const noexcept { return new ErrorImpl(*this); }
	};

	template <typename BASE> class ProgramErrorImpl : public std::invalid_argument, public BASE {
		std::string logMemory;
		int code; const char* sfp;
	public:
		ProgramErrorImpl(int code, const char *what, const char* sfp):invalid_argument(what),code(code),sfp(sfp) { }
		const char *GetErrorMessage() const noexcept { return what(); }
		const char *GetSourceFilePosition() const noexcept { return sfp; }
		int GetErrorCode() const noexcept { return code; }
		IError::ErrorType GetErrorType() const noexcept { return BASE::GetClassErrorType(); }
		void Delete() noexcept { delete this; }
		IError* Clone() const noexcept { return new ProgramErrorImpl(*this); }
		void AttachErrorLog(const char* log) noexcept { logMemory = log; }
		const char *GetErrorLog() const noexcept { return logMemory.c_str(); }
	};

	using InternalError = ErrorImpl<IInternalError, std::logic_error>;
	using RuntimeError = ErrorImpl<IRuntimeError, std::runtime_error>;
	using ProgramError = ProgramErrorImpl<IProgramError>;
	using SyntaxError = ProgramErrorImpl<ISyntaxError>;
	using TypeError = ProgramErrorImpl<ITypeError>;

	class StreamBuf : public IStreamBuf {
		std::streambuf *fwd;
	public:
		StreamBuf(std::streambuf* forwardTo):fwd(forwardTo) { }
		size_t Write(const char *buf, size_t bytes) noexcept { return (size_t)fwd->sputn(buf, bytes); }
		size_t Read(char *buf, size_t bytes) noexcept { return (size_t)fwd->sgetn(buf, bytes); }
		std::uint64_t Seek(std::int64_t offset, KRONOS_INT dir, KRONOS_INT mode) noexcept { return (size_t)fwd->pubseekoff((std::streamoff)offset, (std::ios_base::seekdir)dir, (std::ios_base::openmode)mode); }
		int Sync() noexcept { return fwd->pubsync(); }
	};

	using String = Value<IStr>;

	class IType;

	static inline std::string MoveString(String b) { return std::string(b->data(), b->data() + b->size()); }

	class Type : public Shared<const IType> {
	public:
		Type(const IType& it) :Shared<const IType>(&it) {}
		Type(const IType *i):Shared<const IType>(i) { }
		Type(const std::string& string) : Type(_GetString(string.c_str())) { }
		Type(const char* string) : Type(_GetString(string)) { }
		Type(double val) : Type(_GetConstantF(val)) { }
		Type(std::int64_t val) : Type(_GetConstantI(val)) { }

		inline operator const K3::Type&() const { return Get()->GetPimpl(); }
		inline std::string AsString() const { return MoveString(_CheckResult(Get()->_AsString())); }

		inline Type GetUserTypeContent() const { return _CheckResult(Get()->_GetUserTypeContent()); }
		inline Type GetUserTypeTag() const { return _CheckResult(Get()->_GetUserTypeTag()); }
		inline Type GetFirst() const { return _CheckResult(Get()->_GetFirst()); }
		inline Type GetRest() const { return _CheckResult(Get()->_GetRest()); }

		inline bool operator<(const Type& t) const { return Get()->operator<((const IType&)t); }
		inline bool operator==(const Type& t) const { return Get()->operator==((const IType&)t); }
		inline bool operator!=(const Type& t) { return !operator==(t); }

		inline void ToStream(std::streambuf* buffer, const void *data, int printMode = IType::WithParens) const {
			StreamBuf buf(buffer); Get()->ToStream(buf, data, printMode); _CheckLastError();
		}

		inline void ToStream(std::ostream& stream, const void *data, int printMode = IType::WithParens) const {
			ToStream(stream.rdbuf(), data, printMode); _CheckLastError();
		}

		inline void ToStream(IStreamBuf& sb, const void *dataInstance, int printMode) const { Get()->ToStream(sb, dataInstance, printMode); _CheckLastError(); }

		inline bool IsUserType() const { return Get()->IsUserType(); }
		inline bool IsPair() const { return Get()->IsPair(); }
		inline bool IsTrue() const { return Get()->IsTrue(); }
		inline bool IsNil() const { return Get()->IsNil(); }
		inline bool IsConstant() const { return Get()->IsConstant(); }
		inline bool IsString() const { return Get()->IsString(); }
		inline bool IsFloat32() const { return Get()->IsFloat32(); }
		inline bool IsFloat64() const { return Get()->IsFloat64(); }
		inline bool IsInt32() const { return Get()->IsInt32(); }
		inline bool IsInt64() const { return Get()->IsInt64(); }

		inline double AsConstantF() const { return Get()->AsConstantF(); }
		inline std::int64_t AsConstantI() const { return Get()->AsConstantI(); }
		inline bool AsBool() const { return Get()->AsBool(); }

		inline size_t SizeOf() const { return Get()->SizeOf(); }
		inline size_t GetHash() const { return Get()->GetHash(); }
	};

	static Type GetString(const char* s) { return _CheckResult(_GetString(s)); }
	static Type GetConstant(double c) { return _CheckResult(_GetConstantF(c)); }
	static Type GetConstant(std::int64_t c) { return _CheckResult(_GetConstantI(c)); }
	static Type GetConstant(int c) { return GetConstant(std::int64_t(c)); }
	static Type GetConstant(float c) { return GetConstant(double(c)); }
	static Type GetTuple(const IType& element, size_t count) { return _CheckResult(_GetTuple(element, count)); }
	static Type GetList(const IType& element, size_t count) { return _CheckResult(_GetList(element, count)); }
	static Type GetPair(const IType& fst, const IType& rst) { return _CheckResult(_GetPair(fst, rst)); }
	static Type Tuple(const Type& fst) { return fst; }
	template <typename... ARGS> static Type Tuple(const Type& fst, const Type& snd, const ARGS&... rst) { return _CheckResult(_GetPair(fst, Tuple(snd, rst...))); }
	static Type GetString(const std::string& s) { return _CheckResult(_GetString(s.c_str())); }
	static Type GetNil() { return _CheckResult(_GetNil()->Clone()); }
	static Type GetTrue() { return _CheckResult(_GetTrue()->Clone()); }
	static Type GetFloat32Ty() { return _CheckResult(_GetFloat32Ty()->Clone()); }
	static Type GetFloat64Ty() { return _CheckResult(_GetFloat64Ty()->Clone()); }
	static Type GetInt32Ty() { return _CheckResult(_GetInt32Ty()->Clone()); }
	static Type GetInt64Ty() { return _CheckResult(_GetInt64Ty()->Clone()); }

	enum class TypeTag {
		Float32,
		Float64,
		Int32,
		Int64,
		Pair,
		Vector,
		TypeTag,
		True,
		Nil,
		Graph,
		Invariant,
		InvariantString,
		UserType,
		Union,
		Function,
		AudioFile,
	};

	static Type GetBuiltinTag(TypeTag which) { return _CheckResult(_GetBuiltinTag((int)which)); }
	static Type GetUserType(const IType& tag, const IType& content) { return _CheckResult(_GetUserType(tag, content)); }

	template <typename ITER> class seq_view {
		ITER b, e;
	public:
		seq_view(ITER b, ITER e):b(b), e(e) { }
		ITER begin() const { return b; }
		ITER end() const { return e; }
		size_t size() const { return e - b; }
	};
    
	class GenericGraph : protected Shared<const IGenericGraph> {
	public:
		GenericGraph(const IGenericGraph* ptr =  nullptr) :Shared(ptr) {}
		using Shared::Get;
		using Shared::Empty;
		inline size_t GetGraphHash() const { return (size_t)(Get() ? Get()->GetGraphHash() : 0); }
		inline bool Equal(const GenericGraph& cg) const { return Get()->Equal(cg.Get()); }
		inline GenericGraph Compose(const GenericGraph& cg) const { return Get()->_Compose(cg.Get()); }
		inline bool operator==(const GenericGraph& b) const { return Equal(b); }
		inline Type AsType() const { return Get()->AsType(); }
	};
	
	static Type GetGraph(GenericGraph g) { return _CheckResult(_GetGraph(g.Get())); }

	class TypedGraph : protected Shared<const ITypedGraph> {
    public:
        TypedGraph(const ITypedGraph* ptr):Shared(ptr) {}
        Type TypeOfArgument() const { return Type(Get()->_TypeOfArgument()); }
        Type TypeOfResult() const { return Type(Get()->_TypeOfResult()); }
        const ITypedGraph* Get() const { return Shared::Get(); }
    };

	using Class = std::unique_ptr<krt_class, void(*)(krt_class*)>;
    
    class Context : Shared<IContext> {
        using ImmediateHandler = std::function<void(const char*, GenericGraph)>;
        static void KRONOS_ABI_FN ImmediateHandlerForwarder(void* userdata, const char* sym, const IGenericGraph* ig) {
            auto imptr = (ImmediateHandler*)userdata; (*imptr)(sym,ig);
        }
	public:
		inline String DrainRecentSymbolChanges() { return _CheckResult(Get()->_DrainRecentSymbolChanges()); }
		inline String GetResolutionTrace() const { return _CheckResult(Get()->_GetResolutionTrace()); }

		inline void RegisterSpecializationCallback(const char* signature, SpecializationCallbackHandler callback, void* userData) {
			Get()->_RegisterSpecializationCallback(signature, callback, userData);
		}

		inline void SetModuleHandler(ModulePathResolver callback, void *userData) {
			Get()->SetModulePathResolver(callback, userData);
		}

		inline void SetAssetLinker(AssetLinker al, void *userData) {
			Get()->SetAssetLinker(al, userData);
		}

        inline void Parse(const char *source, bool REPLMode, ImmediateHandler h) {
            Get()->_Parse(source, REPLMode, ImmediateHandlerForwarder, &h);
			_CheckLastError();
        }
        
        inline GenericGraph Parse(const char *source, bool REPLMode = false) {
            GenericGraph gg;
            Parse(source,REPLMode,[&gg](const char*, GenericGraph g) mutable { gg = g; });
            return gg;
        }
        
        inline TypedGraph Specialize(GenericGraph g, const Type& argumentType, std::ostream* log, int logLevel) {
            if (log) {
                StreamBuf tmp(log->rdbuf());
                return _CheckResult(Get()->_Specialize(g.Get(), argumentType, &tmp, logLevel));
            } else {
                return _CheckResult(Get()->_Specialize(g.Get(), argumentType, nullptr, logLevel));
            }
        }

		inline void Make(const char* prefix, const char* fileType, std::ostream& object, const char* engine, TypedGraph g, BuildFlags flags = Default, const char *mtriple = "host",
			const char* mcpu = "",
			const char* march = "",
			const char* features = "") {
			StreamBuf objstream(object.rdbuf());
			_CheckResult(Get()->_AoT(prefix, fileType, &objstream, engine, g.Get(), mtriple, mcpu, march, features, flags));
		}
        
        inline Class Make(const char *engine, TypedGraph g, BuildFlags flags = Default) {
			auto rawClass = Get()->_JiT(engine, g.Get(), flags);
			return _CheckResult(Class(rawClass, rawClass->dispose_class));
        }
        
        inline Class Make(const char* engine, const char* source, const Type& argumentType, std::ostream* log, int logLevel, BuildFlags flags = Default) {
            return Make(engine, Specialize(Parse(source), argumentType, log, logLevel), flags);
        }

		inline void Make(const char* prefix, const char *fileExt, std::ostream& object, const char* engine, const char* source, const Type& argumentType, std::ostream* log, int logLevel, BuildFlags flags = Default, const char* triple = "host",
			const char* mcpu = "",
			const char* features = "") {
			Make(prefix, fileExt, object, engine, Specialize(Parse(source), argumentType, log, logLevel), flags, triple, mcpu, features);
		}

		inline Type DeriveType(const char *source, const Type& argumentType, std::ostream* log, int logLevel) {
            return Specialize(Parse(source), argumentType, log, logLevel).TypeOfResult();
		}
        
		Context(IContext *ic = nullptr) : Shared<IContext>(ic) { }

		inline void SetCoreLibrary(const char* repository, const char* version) { Get()->_SetDefaultRepository(repository, version); }
		inline void GetCoreLibrary(std::string& repo, std::string& version) {
			repo = Get()->_GetCoreLibPackage();
			version = Get()->_GetCoreLibVersion();
		}

		inline std::string GetModuleAndLineNumberText(const char* offset) { return MoveString(_CheckResult(Get()->_GetModuleAndLineNumberText(offset))); }
		inline std::string ShowModuleLine(const char* offset) { return MoveString(_CheckResult(Get()->_ShowModuleLine(offset))); }

		inline void ImportFile(const std::string& modulePath, bool allowRedefinition = false) { Get()->_ImportFile(modulePath.c_str(), allowRedefinition ? 1 : 0); _CheckLastError(); }
		inline void ImportBuffer(const std::string& sourceCode, bool allowRedefinition = false) { Get( )->_ImportBuffer(sourceCode.c_str( ), allowRedefinition); _CheckLastError(); }

		inline Type TypeFromUID(std::int64_t uid) {
			return Get()->_TypeFromUID(uid);
		}

		inline std::int64_t UIDFromType(const Type& t) {
			return Get()->_UIDFromType(t.Get());
		}

		inline operator bool() const {
			return Get() != nullptr;
		}

		inline void BindConstant(const std::string& qualifiedName, const Type& ty, const void* data) {
			Get()->_BindConstant(qualifiedName.c_str(), ty.Get(), data);
			_CheckLastError();
		}

		inline void GetLibraryMetadataAsJSON(std::ostream& stream) {
			StreamBuf buf(stream.rdbuf());
			Get()->_GetLibraryMetadataAsJSON(&buf);
		}

		inline void SetCompilerDebugTraceFilter(const char *flt) {
			Get()->_SetCompilerDebugTraceFilter(flt);
		}
	};

	static std::string GetUserPath() { return MoveString(_GetUserPath()); }
	static std::string GetSharedPath() { return MoveString(_GetSharedPath()); }

	// propagate exceptions to the client ABI
	static inline void _CheckLastError() {
#ifndef __EMSCRIPTEN__
		IError *err = _GetAndResetThreadError();
		if (err == nullptr) return;
		std::string msg = err->GetErrorMessage(); auto code = err->GetErrorCode();
		auto pos = err->GetSourceFilePosition(); auto ty = err->GetErrorType();
		err->Delete();
		switch (ty) {
			case IError::InternalError: throw InternalError(msg.c_str());
			case IError::RuntimeError: throw RuntimeError(msg.c_str());
			case IError::SyntaxError: throw SyntaxError(code, msg.c_str(), pos);
			case IError::TypeError: throw TypeError(code, msg.c_str(), pos);
			default: assert(0 && "Unexpected error of unknown type"); abort();
		}
#endif
	}

	template <typename THandler, typename TNormalFlow>
	static inline bool CatchLastError(THandler catchBlock, TNormalFlow normalBlock) {
		IError *err = _GetAndResetThreadError();
		if (err) {
			std::string msg = err->GetErrorMessage(); auto code = err->GetErrorCode();
			auto pos = err->GetSourceFilePosition(); auto ty = err->GetErrorType();
			err->Delete();
			catchBlock(ty, code, msg, pos);
			return true;
		} else {
			normalBlock();
			return false;
		}
	}

	template <typename THandler>
	static inline bool CatchLastError(THandler catchBlock) {
		return CatchLastError(catchBlock, []() {});
	}


	static Context CreateContext(ModulePathResolver resolver = nullptr, void *user = nullptr) {
		return _CheckResult(_CreateContext(resolver, user));
	}

	class TypeIter {
		Type t;
	public:
		TypeIter(Type t):t(std::move(t)) { }
		TypeIter operator++() { t = t.GetRest(); return *this; }
		TypeIter operator++(int) { auto tmp(*this); operator++(); return tmp; }
		Type operator*() const { return t.GetFirst(); }
		bool operator==(const TypeIter& r) { return *t == *r.t; }
		bool operator!=(const TypeIter& r) { return !operator==(r); }
	};

	static Kronos::TypeIter end(const Kronos::Type) { return Kronos::GetNil(); }
	static Kronos::TypeIter begin(const Kronos::Type &t) { return t.IsPair() ? t : end(t); }
}

namespace std {
	template <> struct hash<Kronos::Type> { 
		size_t operator()(const Kronos::Type& t) const { 
			return t->GetHash(); 
		} 
	};

	template<> struct hash<Kronos::GenericGraph> {
		size_t operator()(const Kronos::GenericGraph& g) const {
			return g.GetGraphHash();
		}
	};

	static std::ostream& operator<<(std::ostream& s, const Kronos::Type& t) { t.ToStream(s,nullptr,false); return s; }
	template <> struct iterator_traits<Kronos::TypeIter> {
		using value_type = Kronos::Type;
		using reference = Kronos::Type&;
		using pointer = Kronos::Type*;
		using iterator_category = forward_iterator_tag;
	};
}


