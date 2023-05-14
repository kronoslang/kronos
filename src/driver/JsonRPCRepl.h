#pragma once
#include "ReplEnvironment.h"
#include "JsonRPCEndpoint.h"
#include "package.h"
#include <iosfwd>
#include <streambuf>
#include <functional>
#include <vector>
#include <mutex>
#include <atomic>

namespace Kronos {
	using OutputFunctionTy = std::function<void(const picojson::value&)>;
	using InputFunctionTy = std::function<picojson::value()>;

	class MessageQueue : public std::streambuf {
		std::vector<char> data;
		std::mutex writerLock;
		int writePointer = 0;
		std::atomic<int> readPosition, writeCommit;

		unsigned GetReadHead();
		unsigned GetWriteHead();
		unsigned ReadAvail();
		unsigned WriteAvailUnsafe();
		void yield();

		std::streamsize xsputn(const char_type* bytes, std::streamsize n) override;
		int_type overflow(int_type c) override;

	public:
		MessageQueue(int size2sExponent);
		void Push(const char* pipe, const char* fmt, const void* data);
		std::string Pop();
	};

	class RPCRepl : public REPL::JiTEnvironment {
		int64_t sndClosureTy = 0, sndInstance = 0;
		OutputFunctionTy out;
		MessageQueue messageQueue;
		std::thread messageRelay;
		std::atomic_flag running;

		struct RateCounter {
			const int overageLimit = 1000;
			double messageCounter = 0;
			int dropped = 0;
			std::chrono::high_resolution_clock::time_point lastMessageTime;
			bool ShouldLimit();
		};

		RateCounter RateLimit;
		GenericGraph evaluator;

		void StartRelay();
		void RestartSnd();
		void Invalidate(std::int64_t closureTy);
		void ToErr(Kronos::IError& err, const std::string& log = "");

	public:
		RPCRepl(IO::IConfiguringHierarchy*, REPL::JiT::Compiler&, OutputFunctionTy);
		~RPCRepl();

		void ToOut(const char* pipe, const char* type, const void* blob, bool newline = false) override;
		void Run(int64_t timestamp, int64_t closureTy, const void* closureArg, int64_t closureSz) override;
		int64_t Start(int64_t closureType, const void *closureData, size_t closureSz);
		std::vector<Runtime::OwnedValue> Eval(const std::string& str);		
		bool RunInVM(const std::string& str, bool invalidate);
		picojson::object Start(const std::string& code);
		picojson::object PullMessages(int max);
	};

	void JsonRPCEndpoint(RPCRepl&, InputFunctionTy in, OutputFunctionTy out, Packages::CloudClient&);
}