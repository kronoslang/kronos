#pragma once

#include <string>
#include "kronos_abi.h"
#include "Graph.h"

using namespace std;

namespace K3 {

	class Type;

	namespace Error{

		enum Code : int{
			Info = 0,
			Success = 1,

			FirstOfNonPair = -9999,
			RestOfNonPair,
			InvalidNodeConnectionPath,
			TypeMismatchInSpecialization,
			SpecializationFailed,
			PackageAlreadyExists,
			SymbolAlreadyExists,
			PackageNotFound,
			SymbolNotFound,
			EmptyGraph,
			NotAFunction,
			SpecializingAlias,
			DivisionByZero,
			InvalidType,
			ReservedWord,
			ArgumentInStateInitializer,
			BadStateDataSource,
			PriorityRecursion,
			ReactiveError,
			NoForeignBufferCallback,
			LibraryLocked,
			ReactivityError,
			InfiniteRecursion,
			CallDepthExceeded,
			InternalError,
			UserExceptionCode,

			// Lexical errors
			BracketInsideParens = -19999,
			BracketNotMatched,
			ParensNotMatched,
			MismatchedBracketsParens,
			EndOfFileInsideParens,
			EndOfFileInsideBrackets,
			UnrecognizedCharacter,
			RedefinitionNotAllowed,
			BadDefinition,
			RedundantTokens,
			UndefinedSymbol,
			BadTokenInArgumentTuple,
			BadNumericConstant,
			ReadFailure,
			BadInput,
			IncompleteInfixFunction,
			TooManyTokens,
			RunawayComment,
			FileNotFound,
			InvalidStringEncoding,

			// API errors
			InvalidAPIParameter = -29999

		};


		enum Level{
			Warning,
			Recoverable,
			Lexical,
			Fatal
		};

		class Internal : public Kronos::IInternalError, public std::string {
		public:
			Internal(const std::string& msg):string(msg) { }
			const char* GetErrorMessage() const noexcept { return c_str(); }
			ErrorType GetErrorType() const noexcept { return IError::InternalError; }
			virtual const char* GetSourceFilePosition() const noexcept { return nullptr; }
			void Delete() noexcept { delete this; }
			IError* Clone() const noexcept { return new Internal(*this); }
			int GetErrorCode() const noexcept { return -1; }
		};

		ostream& operator<<(ostream& stream, const Internal &r);

#ifndef DEBUG_NOVERIFY
#define DEBUG_CHECK(condition, error) {if (condition) throw K3::Error::Internal(error);}
#define DVERIFY(context, condition, error) DEBUG_CHECK(context && (condition), error)
#else
#define DEBUG_CHECK(condition, error) error;
#define DVERIFY(context, condition, error) error;
#endif

	};
};

#if (defined(__cpp_exceptions) && __cpp_exceptions == 199711) || (defined(__EXCEPTIONS) && __EXCEPTIONS != 0)
#define INTERNAL_ERROR(msg) throw K3::Error::Internal(msg)
#else 
#define INTERNAL_ERROR(msg) perror(msg); abort();
#endif
#define KRONOS_UNREACHABLE { assert("unreachable"); abort(); }
