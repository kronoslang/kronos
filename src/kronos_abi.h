#pragma once
#ifndef __KRONOS_ABI_H
#define __KRONOS_ABI_H

/*
 *	Kronos ABI
 *
 *  This header defines the data types that may cross the shared library boundary.
 *  ABI compatibility level similar to that required by COM is required between
 *  the client and the library
 *
 */

#include <cstdint>
#include <cstddef> 
#include "kronosrt.h"

#ifdef WIN32
    #ifdef _WIN64
        #define KRONOS_ABI_FN
        #define KRONOS_ABI_MEMFN
    #else
        #define KRONOS_ABI_FN __cdecl
        #define KRONOS_ABI_MEMFN __thiscall
    #endif
	#define KRONOS_ABI_EXPORT extern "C" __declspec(dllexport)
	#define KRONOS_ABI_IMPORT extern "C" __declspec(dllimport)
#else
    #define KRONOS_ABI_FN
    #define KRONOS_ABI_MEMFN
    #define KRONOS_ABI_EXPORT extern "C" __attribute__ ((used,visibility("default"),cdecl))
    #define KRONOS_ABI_IMPORT extern "C" __attribute__ ((cdecl))
#endif

#ifdef K3_EXPORTS
	#define KRONOS_ABI KRONOS_ABI_EXPORT
#elif K3_IMPORTS
	#define KRONOS_ABI KRONOS_ABI_IMPORT
#else
	#define KRONOS_ABI extern "C"
#endif

#define KRONOS_INT std::int32_t

#define ABI KRONOS_ABI
#define MEMBER KRONOS_ABI_MEMFN
#define FUNCTION KRONOS_ABI_FN
#define INT KRONOS_INT

namespace K3 {
	class Type;
    namespace Nodes {
        class Generic;
        class Typed;
    }
}

namespace CmdLine {
	class IRegistry;
};

namespace Kronos {
	class IShared {
		virtual void MEMBER Attach() const noexcept = 0;
		virtual void MEMBER Detach() const noexcept = 0;
	protected:
		virtual MEMBER ~IShared() { }
	public:
		virtual void MEMBER Delete() const noexcept = 0;
		inline void Retain() const { Attach(); }
		inline void Release() const { Detach(); }
	};

	class IValue {
	protected:
		virtual MEMBER ~IValue() { }
	public:
		virtual void MEMBER Delete() noexcept = 0;
		virtual IValue* MEMBER Clone() const noexcept = 0;
	};

	class IStr : public IValue {
	public:
		virtual const char* MEMBER c_str() const noexcept = 0;
		virtual const char* MEMBER data() const noexcept = 0;
		virtual size_t MEMBER size() const noexcept = 0;
	};

	class IStreamBuf {
	public:
		virtual ~IStreamBuf() { }
		virtual std::uint64_t MEMBER Seek(std::int64_t, INT, INT) noexcept = 0;
		virtual size_t MEMBER Write(const char* buffer, size_t numBytes) noexcept = 0;
		virtual size_t MEMBER Read(char* buffer, size_t numBytes) noexcept = 0;
		virtual INT MEMBER Sync() noexcept = 0;
	};

	class IError : public IValue {
	public:
		enum ErrorType {
			InternalError,
			RuntimeError,
			SyntaxError,
			TypeError
		};
		virtual const char* MEMBER GetErrorMessage() const noexcept = 0;
		virtual ErrorType MEMBER GetErrorType() const noexcept = 0;
		virtual const char* MEMBER GetSourceFilePosition() const noexcept = 0;
		virtual INT MEMBER GetErrorCode() const noexcept = 0;
		virtual void MEMBER Delete() noexcept = 0;
		virtual IError* MEMBER Clone() const noexcept = 0;
	};

	class IInternalError : public IError { 
	public:
		static ErrorType GetClassErrorType() { return InternalError; }
	};

	class IRuntimeError : public IError { 
	public:
		static ErrorType GetClassErrorType() { return RuntimeError; }
	};

	class IProgramError : public IError { 
	public:
		virtual const char* MEMBER GetErrorLog() const noexcept = 0;
		virtual void MEMBER AttachErrorLog(const char*) noexcept = 0;
	};

	class ISyntaxError : public IProgramError { 
	public:
		static ErrorType GetClassErrorType() { return SyntaxError; }
	};
	class ITypeError : public IProgramError { 
	public:
		static ErrorType GetClassErrorType() { return TypeError; }
	};

	class IType : public IShared {
	public:
		virtual IType* Clone() const noexcept = 0;
		virtual IStr* MEMBER _AsString() const noexcept = 0;
		virtual void MEMBER ToStream(IStreamBuf&, const void *dataInstance, INT printMode) const noexcept = 0;
		virtual IType* MEMBER _GetUserTypeContent() const noexcept = 0;
		virtual IType* MEMBER _GetUserTypeTag() const noexcept = 0;
		virtual IType* MEMBER _GetFirst() const noexcept = 0;
		virtual IType* MEMBER _GetRest() const noexcept = 0;

		virtual const K3::Type& MEMBER GetPimpl() const noexcept = 0;

		virtual bool MEMBER IsUserType() const noexcept = 0;
		virtual bool MEMBER IsPair() const noexcept = 0;
		virtual bool MEMBER IsTrue() const noexcept = 0;
		virtual bool MEMBER IsNil() const noexcept = 0;
		virtual bool MEMBER IsConstant() const noexcept = 0;
		virtual bool MEMBER IsString() const noexcept = 0;
		virtual bool MEMBER IsFloat32() const noexcept = 0;
		virtual bool MEMBER IsFloat64() const noexcept = 0;
		virtual bool MEMBER IsInt32() const noexcept = 0;
		virtual bool MEMBER IsInt64() const noexcept = 0;

		virtual double MEMBER AsConstantF() const noexcept = 0;
		virtual std::int64_t MEMBER AsConstantI() const noexcept = 0;
		virtual bool MEMBER AsBool() const noexcept = 0;

		virtual size_t MEMBER SizeOf() const noexcept = 0;
		virtual size_t MEMBER GetHash() const noexcept = 0;
		virtual IType& MEMBER operator=(const IType&) noexcept = 0;

		virtual bool MEMBER operator<(const IType&) const noexcept = 0;
		virtual bool MEMBER operator==(const IType&) const noexcept = 0;

		enum PrintMode {
			WithParens,
			WithoutParens,
			JSON
		};
	};

	class IUserException : public IError {
	public:
		virtual const IType& MEMBER _GetUserException() const noexcept = 0;
	};

	enum BuildFlags : std::uint32_t {
		Default = 0,
		OmitEvaluate = 1,
		StrictFloatingPoint = 2,
		EmulateFloatingPoint = 4,
		OmitReactiveDrivers = 8,
		WasmStandaloneModule = 16,
		DynamicRateSupport = 32,
		CompilerFlagMask = 0xffff,
		UserFlag1 = 0x10000
	};
    
    class IGenericGraph : public IShared {
    public:
        virtual const K3::Nodes::Generic* MEMBER Get() const noexcept = 0;
		virtual bool MEMBER Equal(const IGenericGraph*) const noexcept = 0;
		virtual IGenericGraph* MEMBER _Compose(const IGenericGraph*) const noexcept = 0;
		virtual std::uint64_t MEMBER GetGraphHash() const noexcept = 0;
		virtual IType* MEMBER AsType() const noexcept = 0;
    };
    
    class ITypedGraph : public IShared {
    public:
        virtual const K3::Nodes::Typed* MEMBER Get() const noexcept = 0;
        virtual IType* MEMBER _TypeOfResult() const noexcept = 0;
        virtual IType* MEMBER _TypeOfArgument() const noexcept = 0;
        virtual const K3::Type* MEMBER _InternalTypeOfResult() const noexcept = 0;
        virtual const K3::Type* MEMBER _InternalTypeOfArgument() const noexcept = 0;
    };
    
    using ImmediateExpressionHandler = void FUNCTION (void*,const char*,const IGenericGraph*);
 	
	class ICompilerInterface : public IShared {
	public:
        virtual void MEMBER _Parse(const char *source, bool REPLMode, ImmediateExpressionHandler, void *userdata) noexcept = 0;
        virtual const ITypedGraph* MEMBER _Specialize(const IGenericGraph*, const IType& argumentType, IStreamBuf* log, INT logLevel) noexcept = 0;
		virtual krt_class* MEMBER _JiT(
			const char* engine,
            const ITypedGraph*,
      		BuildFlags flags)  noexcept = 0;
		virtual INT MEMBER _AoT(
			const char* prefix,
			const char* fileType,
			IStreamBuf* object,
			const char* engine,
			const ITypedGraph*,
			const char* triple,
			const char* mcpu,
			const char* march,
			const char* features,
			BuildFlags flags) noexcept = 0;
		virtual IStr* MEMBER _GetResolutionTrace() const noexcept = 0;
		virtual IStr* MEMBER _DrainRecentSymbolChanges() noexcept = 0;
	};

	using SpecializationCallbackHandler = void FUNCTION(void *user, INT, const IType*, std::int64_t);
	using ModulePathResolver = const char* FUNCTION(const char* package, const char* mod, const char *version, void *user);
	using AssetLinker = void* FUNCTION(const char* uri, const IType** outType, void* user);

	class IContext : public ICompilerInterface {
	public:
		virtual void MEMBER _ImportFile(const char *modulePath, INT allowRedefine)  noexcept = 0;
		virtual void MEMBER _ImportBuffer(const char* sourceCode, bool allowRedefinition)  noexcept = 0;
		virtual IStr* MEMBER _GetModuleAndLineNumberText(const char*)  noexcept = 0;
		virtual IStr* MEMBER _ShowModuleLine(const char*) noexcept = 0;
		virtual INT MEMBER _GetLibraryMetadataAsJSON(IStreamBuf* json) = 0;
		virtual IType* MEMBER _TypeFromUID(std::int64_t uid) noexcept = 0;
		virtual std::int64_t MEMBER _UIDFromType(const IType*) noexcept = 0;
		virtual void MEMBER _RegisterSpecializationCallback(const char*, SpecializationCallbackHandler, void *userData) noexcept = 0;
		virtual void MEMBER SetModulePathResolver(ModulePathResolver, void* user) noexcept = 0;
		virtual void MEMBER SetAssetLinker(AssetLinker al, void* user) noexcept = 0;
		virtual void MEMBER _BindConstant(const char*, const IType*, const void*) = 0;
		virtual void MEMBER _SetCompilerDebugTraceFilter(const char *) = 0;
		virtual void MEMBER _SetDefaultRepository(const char* package, const char* version) = 0;
		virtual const char* MEMBER _GetCoreLibPackage() = 0;
		virtual const char* MEMBER _GetCoreLibVersion() = 0;
	};

	ABI const char* FUNCTION GetVersionString( ) noexcept;
	ABI void FUNCTION AddBackendCmdLineOpts(CmdLine::IRegistry& MasterRegistry) noexcept;

	ABI IStr* FUNCTION _GetUserPath()  noexcept;
	ABI IStr* FUNCTION _GetSharedPath() noexcept;
	ABI IContext* FUNCTION _CreateContext(ModulePathResolver resolver, void *resolverData) noexcept;

	ABI IType* FUNCTION _GetString(const char*) noexcept;
	ABI IType* FUNCTION _GetConstantF(double) noexcept;
	ABI IType* FUNCTION _GetConstantI(std::int64_t) noexcept;
	ABI IType* FUNCTION _GetTuple(const IType& element, size_t count) noexcept;
	ABI IType* FUNCTION _GetList(const IType& element, size_t count) noexcept;
	ABI IType* FUNCTION _GetPair(const IType& fst, const IType& rst) noexcept;
	ABI IType* FUNCTION _GetUserType(const IType& tag, const IType &content) noexcept;
	ABI IType* FUNCTION _GetBuiltinTag(int which) noexcept;
	ABI const IType* FUNCTION _GetNil() noexcept;
	ABI const IType* FUNCTION _GetTrue() noexcept;
	ABI const IType* FUNCTION _GetFloat32Ty() noexcept;
	ABI const IType* FUNCTION _GetFloat64Ty() noexcept;
	ABI const IType* FUNCTION _GetInt32Ty() noexcept;
	ABI const IType* FUNCTION _GetInt64Ty() noexcept;
	ABI IType* FUNCTION _GetGraph(const IGenericGraph*) noexcept;
	ABI IType* FUNCTION _ConvertToABIType(const void *raw) noexcept;

	ABI IError* FUNCTION _GetAndResetThreadError() noexcept;
}
#undef ABI
#undef FUNCTION
#undef MEMBER
#undef INT
#endif
