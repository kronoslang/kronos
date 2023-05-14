#pragma once
#include "Errors.h"
#include "Graph.h"
#include "common/Ref.h"
#include "NodeBases.h"

namespace K3 {
	namespace Error{

		template <typename INTERFACE>
		class BadProgram : public INTERFACE, public std::string{
			Code code;
			const char* pos;
			std::string log;
		protected:
			BadProgram(Code c, const char* pos, std::string msg):string(msg),code(c),pos(pos) {}
		public:
			void Delete() noexcept { delete this; }
			const char* GetErrorMessage() const noexcept { return c_str(); }
			const char* GetPosition() const noexcept { return pos; }
			KRONOS_INT GetErrorCode() const noexcept { return code; }
			const char *GetErrorLog() const noexcept { return log.c_str(); }
			void AttachErrorLog(const char *msg) noexcept { log = msg; }
		};

		class Program : public BadProgram<Kronos::IProgramError> {
			virtual const char* GetSourceFilePosition() const noexcept { return GetPosition(); }
		public:
			Program(Code c, const string& msg, const char* pos):BadProgram(c,pos,msg) {}

			ErrorType GetErrorType() const noexcept { return SyntaxError; }
			void Delete()  noexcept { delete this; }
			IError* Clone() const noexcept { return new Program(*this); }
		};

		class ParserError : public BadProgram<Kronos::ISyntaxError> {
			virtual const char* GetSourceFilePosition() const noexcept { return GetPosition(); }
		public:
			ParserError(Code c, const string& msg, const char*  pos):BadProgram(c,pos,msg) {}
			ErrorType GetErrorType() const noexcept { return IError::SyntaxError; }
			void Delete() noexcept { delete this; }
			ParserError* Clone() const noexcept { return new ParserError(*this); }
		};

		class BadType : public BadProgram<Kronos::ITypeError> {
			virtual const char* GetSourceFilePosition() const noexcept { return GetPosition(); }
		public:
			BadType(Code c, const string& msg, const char*  pos, const string& log):
				BadProgram(c,pos,msg) {}

			IError* Clone() const noexcept { return new BadType(*this); }
			ErrorType GetErrorType() const noexcept { return IError::TypeError; }
		};

		class RuntimeError : public Kronos::IRuntimeError {
			const string msg;
			const Code c;
		public:
			RuntimeError(Code c, const string& msg):msg(msg),c(c) {}
			int GetErrorCode() const noexcept { return c; }
			const char* GetErrorMessage() const noexcept { return msg.c_str(); }
			ErrorType GetErrorType() const noexcept { return IError::RuntimeError; }
			virtual const char*  GetSourceFilePosition() const noexcept { return nullptr; }
			void Delete() noexcept { delete this; }
			RuntimeError* Clone() const noexcept { return new RuntimeError(*this); }
		};
	};
};


