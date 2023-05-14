#include <sstream>
#include <fstream>
#include <string.h>

#include "JsonRPCRepl.h"
#include "package.h"

#include "config/system.h"

#ifndef HAVE_STRNLEN
size_t strnlen(const char *str, size_t max)
{
    const char *end = (const char *)memchr (str, 0, max);
    return end ? (size_t)(end - str) : max;
}
#endif

namespace Kronos {
	MessageQueue::MessageQueue(int sizeLevel) :data(1ll << sizeLevel) {
		readPosition.store(0);
		writeCommit.store(0);
	}

	unsigned MessageQueue::GetReadHead() {
		return readPosition.load(std::memory_order_acquire);
	}

	unsigned MessageQueue::GetWriteHead() {
		return writeCommit.load(std::memory_order_acquire);
	}

	unsigned MessageQueue::ReadAvail() {
		return (GetWriteHead() - GetReadHead()) & (data.size() - 1);
	}

	unsigned MessageQueue::WriteAvailUnsafe() {
		int delta = GetReadHead() - writePointer;
		return (delta + data.size() - 1) & ((unsigned)data.size() - 1);
	}

	void MessageQueue::yield() {
		std::this_thread::yield();
	}

	std::streamsize MessageQueue::xsputn(const char_type* bytes, std::streamsize n) {
		if (n >= data.size()) return 0;

 		for (;;) {
			auto avail = WriteAvailUnsafe();
			if (avail >= n) {
				int todo = (int)n;
				int write1 = std::min((int)data.size() - writePointer, todo);
				memcpy(data.data() + writePointer, bytes, write1);
				writePointer += write1;
				todo -= write1;
				if (todo) {
					memcpy(data.data(), (char*)bytes + write1, todo);
					writePointer = todo;
				}
				return n;
			} else {
				yield();
			}
		}
	}

	std::streambuf::int_type MessageQueue::overflow(int_type c) {
		if (c != EOF) {
			while (!WriteAvailUnsafe()) yield();
			data[writePointer++] = (char)c;
			writePointer = writePointer & (data.size() - 1);
		}
		return c;
	}

	void MessageQueue::Push(const char* pipe, const char* fmt, const void* data) {
		std::ostream format{ this };
		std::lock_guard<std::mutex> lg{ writerLock };
		format << pipe << "\n"; 
		Runtime::ToStream(format, fmt, data, true);
		format.put(0);
		writeCommit.store(writePointer, std::memory_order_release);
	}

	std::string MessageQueue::Pop() {
		auto readPos = GetReadHead();
		auto available = ReadAvail();
		if (!available) return "";

		auto contiguous = (int)data.size() - readPos;
			
		auto limit = std::min(contiguous, available);
		auto msgEnd = (int)strnlen(data.data() + readPos, limit);

		std::string value{ data.data() + readPos, 
						   data.data() + readPos + msgEnd };

		readPos += msgEnd;
			
		if (msgEnd == limit && limit < available) {
			// message is wrapped
			assert(limit == contiguous);
			limit = available - contiguous;
			msgEnd = (int)strnlen(data.data(), limit);
			value += { data.data(), data.data() + msgEnd };
			readPos = (int)msgEnd;
		}

		readPosition.store((readPos + 1) & (data.size() - 1), std::memory_order_release);

		return value;
	}

	template <typename T> void append_data(std::vector<char>& data, const T& value) {
		const char *read = (const char*)&value;
		for (int i = 0;i < sizeof(value);++i) {
			data.push_back(read[i]);
		}
	}

	void JsonDataToBlob(const picojson::value& val, std::string& signature, std::vector<char>& data) {
		if (val.is<double>()) {
			signature += "%f";
			append_data(data, (float)val.get<double>());
		} else if (val.is<picojson::array>()) {
			signature += "(";
			for (const auto& e : val.get<picojson::array>()) {
				JsonDataToBlob(e, signature, data);
			}
			signature += ")";
		} else if (val.is<picojson::object>()) {

		} else if (val.is<picojson::null>()) {
			signature += "nil";
		}
	}

	RPCRepl::RPCRepl(IO::IConfiguringHierarchy* io, REPL::JiT::Compiler& c, OutputFunctionTy out) 
		:REPL::JiTEnvironment(io, c), out(out), messageQueue(20) {

		std::stringstream sndMagic;
		sndMagic << ":VM:Adapter({ :Audio:Outputs } { :snd })";
		sndClosureTy = ParseToUID(sndMagic.str());

		evaluator = c.Parse("Eval({ \
							When( Equal-Type(Class-Of(arg) :VM:Op) \
									VM:Execute(arg { :Audio:Outputs }) \
								  Otherwise arg ) } arg)")[0];

		running.test_and_set();
		messageRelay = std::thread([this]() { StartRelay(); });
	}

	RPCRepl::~RPCRepl() {
		running.clear();
		if (messageRelay.joinable()) messageRelay.join();
	}

	void RPCRepl::StartRelay() {
		while (running.test_and_set()) {
			auto msgs = PullMessages(100);
			if (msgs.empty()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			} else {
				picojson::object msgobj{
					{ "jsonrpc", "2.0" },
					{ "method", "message-bundle" },
					{ "params", msgs } };
				out(msgobj);
			}
		}
	}

	picojson::object RPCRepl::PullMessages(int max) {
		picojson::object messages;
		for (int i = 0;i < max;++i) {
			auto msg = messageQueue.Pop();
			if (msg.empty()) {
				return messages;
			}
			auto pipeEnd = msg.find_first_of("\n");
			auto pipe = msg.substr(0, pipeEnd);

			auto set = messages.find(pipe);
			if (set == messages.end()) {
				set = messages.emplace(pipe, picojson::array{}).first;
			}

			set->second.get<picojson::array>().emplace_back(msg.substr(pipeEnd + 1));			
		}
		return messages;
	}

	bool RPCRepl::RateCounter::ShouldLimit() {
		using namespace std::chrono;
		auto now = high_resolution_clock::now();
					
		// 100 messages per second steady limit
		auto delta = duration_cast<nanoseconds>(now - lastMessageTime).count() / 10000000.0;
		lastMessageTime = now;

		if (messageCounter < delta) {
			messageCounter = 0.0;
		} else {
			messageCounter -= delta;
		}

		// allow occasional burst
		if (messageCounter > overageLimit) {
			dropped++;
			return true;
		} else {
			messageCounter++;
			return false;
		}
	}

	void RPCRepl::ToOut(const char* pipe, const char* type, const void* blob, bool newline) {
		if (RateLimit.ShouldLimit()) return;
		messageQueue.Push(pipe, type, blob);
		RateLimit.dropped = 0;
		return;

		std::stringstream msg;
		msg << Runtime::Value{ type, blob };
		if (newline) msg << "\n";

		picojson::object msgobj{
			{ "jsonrpc", "2.0" },
			{ "method", "message" },
			{ "params", picojson::object{
				{ "pipe", pipe },
				{ "message", msg.str() },
				{ "dropped", (double)RateLimit.dropped }
		} } };
		// reset drop counter
		RateLimit.dropped = 0;
		out(msgobj);
	}

	picojson::object FormatErr(Kronos::IError& err, const std::string& log = "") {
		return {
			{ "error", true },
			{ "code", (double)err.GetErrorCode() },
			{ "message", err.GetErrorMessage() },
			{ "log", log }
		};
	}

	picojson::object FormatErr(std::exception& ex) {
		return {
			{ "code", 0.0 },
			{ "message", ex.what() }
		};
	}

	void RPCRepl::ToErr(Kronos::IError& err, const std::string& log) {
		picojson::object errObj{
			{ "jsonrpc", "2.0" },
			{ "method", "error" },
			{ "params",  FormatErr(err, log) }
		};
		out(errObj);
	}

	void RPCRepl::Run(int64_t timestamp, int64_t closureTy, const void* closureArg, int64_t closureSz) {
        try {
            JiTEnvironment::Run(timestamp, closureTy, closureArg, closureSz);
        } catch (Kronos::IProgramError& pe) {
            ToErr(pe, pe.GetErrorLog());
        } catch (Kronos::IError& ie) {
            ToErr(ie);
        }
	}
            
	int64_t RPCRepl::Start(int64_t closureType, const void *closureData, size_t closureSz) {
        try {
            return JiTEnvironment::Start(closureType, closureData, closureSz);
        } catch (Kronos::IProgramError& pe) {
            ToErr(pe, pe.GetErrorLog());
        } catch (Kronos::IError& ie) {
            ToErr(ie);
        }
        return 0;
    }
            
    void RPCRepl::RestartSnd() {
        if (sndInstance) {
            Stop(sndInstance);
        }
        sndInstance = Start(sndClosureTy, 0, 0);
    }

	void RPCRepl::Invalidate(std::int64_t closureTy) {
		picojson::object msgobj {
			{ "jsonrpc", "2.0" },
			{ "method", "invalidate" },
			{ "params", std::to_string(closureTy) },
		};

		out(msgobj);
	}

	std::vector<Runtime::OwnedValue> RPCRepl::Eval(const std::string& str) {
		try {
            bool recompileSnd = false;
            auto result = Parse((Runtime::IEnvironment*)this, evaluator, str, [&](const std::string& sym, std::int64_t closureTy) {
                if (sym == ":snd") recompileSnd = true;
                if (closureTy == sndClosureTy) recompileSnd = true;
				else Invalidate(closureTy);
            });
            if (recompileSnd) RestartSnd();
            return result;
		} catch (Kronos::IProgramError& pe) {
			ToErr(pe, pe.GetErrorLog());
		} catch (Kronos::IError& ie) {
			ToErr(ie);
		}
		return {};
	}

    bool RPCRepl::RunInVM(const std::string& str, bool invalidate) {
		try {
            bool recompileSnd = false;
            Parse((Runtime::IEnvironment*)this, str, [&](const std::string& sym, std::int64_t closureTy) {
                if (sym == ":snd") recompileSnd = true;
                if (closureTy == sndClosureTy) recompileSnd = true;
				else if (invalidate) Invalidate(closureTy);
            });
            if (recompileSnd) RestartSnd();
			return true;
		} catch (Kronos::IProgramError& pe) {
			ToErr(pe, pe.GetErrorLog());
		} catch (Kronos::IError& ie) {
			ToErr(ie);
		}
		return false;
	}

	picojson::object RPCRepl::Start(const std::string& code) {
		try {
			auto inst = MakeInstance(code);
			if (!inst) return {};
			auto id = Start(inst, nullptr, 0);
			auto obj = GetChild(id);
			if (!obj.empty()) {
				auto worldIdx = obj->GetSymbolIndex({ "#:VM:World:World" });
				obj->Dispatch(worldIdx, &world, sizeof(world), nullptr);
			}
			return picojson::object{
				{ "success", id != 0 },
				{ "instance" , std::to_string(id) },
				{ "build", std::to_string(inst) }
			};
		} catch (Kronos::IProgramError& pe) {
			return FormatErr(pe, pe.GetErrorLog());
		} catch (Kronos::IError& ie) {
			return FormatErr(ie);
        } catch (std::exception& ex) {
			return FormatErr(ex);
        }
		return {};
	}

	void JsonRPCEndpoint(RPCRepl& repl, InputFunctionTy in, OutputFunctionTy out, Packages::CloudClient& bb) {
		JsonRPC::Endpoint replEp;

		replEp["evaluate"] = [&](const picojson::value& r) {
			std::clog << "[ Evaluate ]\n" << r.get("code").to_str() << "\n";
			auto code = r.get("code").get<std::string>();
			auto values = repl.Eval(code);
			picojson::array resultArray;
			for (auto &v : values) {
				std::stringstream encode;
				encode << v;
				picojson::value tmp;
				picojson::parse(tmp, encode);
				resultArray.emplace_back(tmp);
			}
			return resultArray;
		};

		replEp["start"] = [&](const picojson::value& r) {
			std::clog << "\n[ Instantiate ]\n" << r.get("code").to_str() << "\n";
			auto code = r.get("code").get<std::string>();
			return repl.Start(code);
		};

		replEp["get_children"] = [&](const picojson::value& r) {
			picojson::array children;
			repl.EnumerateChildren([&children](int64_t id) {
				children.emplace_back(std::to_string(id));
				return true;
			});
			return children;
		};
        
        replEp["panic"] = [&](const picojson::value& r) {
            return (double)repl.StopAll();
        };

		replEp["stop"] = [&](const picojson::value& r) {
			auto didStop = repl.Stop(strtoll(r.get("instance").to_str().c_str(), nullptr, 10));
			if (!didStop) std::clog << "* No instance " << r.get("instance") << "\n";
			else std::clog << "* Stopped " << r.get("instance") << "\n";
			return didStop;
		};

		replEp["vm"] = [&](const picojson::value& r) {
			std::clog << "[ Run ] " << r.get("code").to_str() << "\n";
			auto code = r.get("code").to_str();
			return repl.RunInVM(code, r.get("invalidate").get<bool>());
		};

		replEp["parse"] = [&](const picojson::value& r) {
			auto code = r.get("code").to_str();
			repl.Parse(code);
			return true;
		};

		std::unordered_map<std::string, std::vector<char>> slotMemory;
		replEp["set"] = [&](const picojson::value& r) -> picojson::value {
			for (auto &kv : r.get<picojson::object>()) {
				auto method = kv.first;
				std::string signature;
				auto& data(slotMemory[method]);
				data.clear();
				JsonDataToBlob(kv.second, signature, data);
				auto idx = repl.GetSymbolIndex(Runtime::MethodKey{ method.c_str(), signature.c_str() });
				repl.Dispatch(idx, data.data(), data.size(), nullptr);
			}
			return {};
		};

		replEp["debug_trace"] = [&](const picojson::value& r) {
			repl.SetCompilerDebugTrace(r.to_str());
			return picojson::object{ };
		};

		replEp["get_module_cache"] = [&](const picojson::value& r) {
			return bb.GetCache();
		};

		replEp["get_module_cache_file"] = [&](const picojson::value& r) {
			auto fp = bb.Resolve(r.get("package").to_str(), 
								 r.get("file").to_str(), 
								 r.get("version").to_str());
			if (fp) {
				std::ifstream file{ fp };
				if (file.is_open()) {
					std::stringstream read;
					read << file.rdbuf();
					return picojson::object{
						{"data", read.str() }
					};
				}
			} 
			return picojson::object{
				{"error", "file not found" }
			};	
		};

		replEp["pull_messages"] = [&](const picojson::value& r) {
			return repl.PullMessages((int)r.get<double>());
		};

		replEp["library"] = [&](const picojson::value& r) {
			std::stringstream ss;
			repl.ExportLibraryMetadata(ss);
			picojson::value lib;
			picojson::parse(lib, ss);
			return lib;
		};

		std::unordered_set<std::string> runInThread{
			"start",
			"evaluate",
			"vm"
		};

		struct PendingThreads {
			std::atomic<int> counter;

			PendingThreads() {
				counter.store(0);
			}

			void Start() {
				++counter;
			}

			void Stop() {
				--counter;
			}

			struct Job {
				PendingThreads& pt;
				Job(PendingThreads& pt) :pt(pt) {
					pt.Start();
				}
				
				Job(const Job& j):pt(j.pt) {
					pt.Start();
				}

				~Job() {
					pt.Stop();
				}
			};

			Job Create() {
				return Job{ *this };
			}

			~PendingThreads() {
				while (counter.load() > 0) {
					std::this_thread::sleep_for(std::chrono::milliseconds(250));
				}
			}

		} Jobs;

		while (true) {
			auto recv = in();
			if (recv.contains("connection_closed")) {
				return;
			}

			if (recv.contains("method") && runInThread.count(recv.get("method").to_str())) {
				std::thread([&, recv, job = Jobs.Create()]() {
					auto val = replEp(recv);
					if (val.evaluate_as_boolean()) out(val);
				}).detach();
			} else {
				out(replEp(recv));
			}
		}
	}
}
