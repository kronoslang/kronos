#include "o2xx.h"
#include "kronos.h"
#include "config/system.h"
#include "ReplEnvironment.h"
#include "ReplEntryBuffer.h"

#include <iostream>
#include <sstream>
#include <string>
#include <list>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "driver/package.h"
#include "config/corelib.h"

// o2 depends on these
#ifdef WIN32
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "winmm.lib")
#endif

using namespace Kronos;

void FormatErrors(const char* xml, std::ostream& out, Kronos::Context& cx, int startIndent = 0);

Packages::BitbucketClient bbClient(KRONOS_CORE_LIBRARY_REPOSITORY, KRONOS_CORE_LIBRARY_VERSION);

static std::string GetO2Signature(const char * kronosTypeSig) {
	std::stringstream sig;
	if (kronosTypeSig) {
		while (*kronosTypeSig) {
			if (*kronosTypeSig++ == '%') {
				switch (*kronosTypeSig++) {
				default: break;
				case 'f': sig.put(O2_FLOAT); break;
				case 'd': sig.put(O2_DOUBLE); break;
				case 'q': sig.put(O2_INT64); break;
				case 'i': sig.put(O2_INT32); break;
				}
			}
		}
	}
	return sig.str();
}

struct CompilerService {
	struct Symbol {
		Runtime::IObject *obj;
		int index;
		std::vector<char> slot;
	};

	struct Inst {
		std::int64_t instance;
		std::unordered_map<std::string, std::unique_ptr<Symbol>> methods;
	};

	std::unordered_map<std::string, Inst> instances;
	std::mutex instanceLock;

	// Instantiate IO and JiT
	Kronos::Context cx = Kronos::CreateContext();
	Kronos::REPL::JiT::Compiler compiler;
	std::unique_ptr<Kronos::IO::IHierarchy> io;
	Kronos::REPL::Console rootEnv;

	o2::service service;

	static void callback_method(const o2_msg_data_ptr msg, const char* ty, o2_arg_ptr* argv, int argc, void *user) {
		auto sym = (Symbol*)user;
		int tyIdx = 0;
		size_t byteIdx = 0;
		while (*ty) {
			size_t itemSz = 0;
			switch (*ty++) {
			case O2_FLOAT: case O2_INT32: case O2_BOOL: itemSz = 4;
				break;
			case O2_DOUBLE: case O2_INT64: itemSz = 8;
				break;
			}
			if (sym->slot.size() < byteIdx + itemSz) {
				sym->slot.resize(byteIdx + itemSz);
			}
			memcpy(sym->slot.data() + byteIdx, argv[tyIdx++], itemSz);
			byteIdx += itemSz;
		}
		sym->obj->Dispatch(sym->index, sym->slot.data(), 0, nullptr);
	}

	CompilerService(const o2::application& app, const char* serviceName)
		:cx(Kronos::CreateContext(Packages::BitbucketClient::ResolverCallback, &bbClient))
		,compiler(cx, bbClient.Resolve("", "VM.k", ""))
		,rootEnv(io.get(), compiler, "(0 0)")	
		,service(app.provide(serviceName))
	{
		std::stringstream config;
		config << "Package Control { Send(address data) { VM:Make-Op[\"kvm_send_o2!\" "
			"\"const char*\" address "
			"\"const char*\" String:Interop-Format(data) "
			"\"const void*\" data]"
			"} }";
		io = std::move(IO::CreateCompositeIO(config, 2));
		compiler.Parse(config.str().c_str());

		service.implement("run", "s", [&](o2_arg_ptr* argv, int argc) {
			try {
				std::clog << "> " << argv[0]->s << "\n";
				rootEnv.Parse(argv[0]->s);
			} catch (std::exception& e) {
				std::cerr << "'" << argv[0]->s << "': " << e.what() << "\n";
			}
		});

		service.implement("new", "ss", [&](o2_arg_ptr* argv, int argc) {
			std::string code = argv[1]->s;
			code = ":VM:Adapter('(0 0) { " + code + " })";
			try {
				auto graph = compiler.Parse(code.c_str());
				if (graph.size()) {
					auto graphUid = compiler.UID(graph[0].AsType());
					std::lock_guard<std::mutex> lg(instanceLock);
					std::string instName = argv[0]->s;
					int64_t instId = 0;
					std::clog << "Creating '" << instName << "'\n";
					try {
						instId = rootEnv.Start(graphUid, nullptr, 0);
					} catch (std::exception& e) {
						std::cerr << "* " << e.what() << "\n";
					}
					if (instId) {
						Stop(argv[0]->s);
						std::lock_guard<std::recursive_mutex> lg(o2::msg_lock());
						auto ii = instances.emplace(instName, Inst{ instId,{} }).first;
						o2_service_new(instName.c_str());
						auto obj = rootEnv.GetChild(instId);
						obj->EnumerateSymbols([&](int symIdx, const Runtime::MethodKey& key) mutable {
							auto sym = std::make_unique<Symbol>(Symbol{ obj.get(), symIdx });
							o2_method_new(("/" + instName + "/" + key.name).c_str(), GetO2Signature(key.signature).c_str(), callback_method, sym.get(), 1, 1);
							ii->second.methods.emplace(key.name, std::move(sym));
							std::clog << "Exposing '/" << instName << "/" << key.name << "'\n";
						});
					} else {
						std::cerr << "Instantiating '" << argv[1]->s << "' failed\n";
					}
				}
			} catch (std::exception &e) {
				std::clog << "* " << e.what();
			}
		});

		service.implement("delete", "s", [&](o2_arg_ptr* argv, int argc) {
			std::lock_guard<std::mutex> lg(instanceLock);
			Stop(argv[0]->s);
		});
	}

	void Stop(const char* key) {
		auto f = instances.find(key);
		if (f != instances.end()) {
			std::lock_guard<std::recursive_mutex> lg(o2::msg_lock());
			o2_service_free((char*)key);
			rootEnv.Stop(f->second.instance);
			instances.erase(f);
		}
	}
};

int main(int argn, const char* carg[]) {
	using namespace Kronos;

	const char *appName = getenv("KO2_APPLICATION");
	if (!appName) appName = "ko2";

	const char *nodeName = getenv("KO2_NODE");
	if (!nodeName) nodeName = "compile_server";

	try {
		if (argn < 2) {
			std::clog << "USAGE: " << carg[0] << " server|new|delete|send\n";
			std::clog << " " << carg[0] << " <kronos-code>\n";
			std::clog << " " << carg[0] << " send <method> <parameters...>\n";
			std::clog << " " << carg[0] << " new <symbol> <kronos-code>\n";
			std::clog << " " << carg[0] << " delete <symbol>\n\n";
			std::clog << "If the argument is 'server', a compile server is started.\nOtherwise, a command to be sent to an existing server.\n";
			std::clog << "If the argument is 'send', a sequence of numbers can be sent as a Kronos Control Event. The numbers are sent as single-precision floating point.\n";
			std::clog << "If the argument is none of the specified keywords, it is treated as Kronos code to be run on the compile server.\n";
			std::clog << carg[0] << " joins the O2 application '" << appName << "', which can be overridden with the\n";
			std::clog << "environment variable 'KO2_APPLICATION'. The targeted compile server is determined by the environment variable 'KO2_NDOE'\n\n";
			return -1;
		}

		o2::application KNode(appName);

		if (!strcmp(carg[1], "server")) {
			if (argn > 2) nodeName = carg[2];

			CompilerService service(KNode, nodeName);

			std::cout << "Application: " << appName << "\nService: " << nodeName << "\n\n";
			std::cout << "  /" << nodeName << "/run <string>\n\nRuns <string> as Kronos code in the context of the compile server\n\n";
			std::cout << "  /" << nodeName << "/new <symbol> <string>\n\nInstantiates <string> as Kronos code in a new o2 service <symbol>\n\n";
			std::cout << "  /" << nodeName << "/delete <symbol>\n\nDeletes the service <symbol>\n\n";
			std::cout << "Press any key to exit...\n";
			auto client = KNode.request(nodeName);
			client.send("run", "Actions:PrLn(\"Hello world!\")");
			std::cin.get();
			return 0;
		} 
		
		std::clog << "Waiting for " << nodeName << "...";
		auto client = KNode.request(nodeName);
		client.wait_for_discovery();
		std::clog << " Ok\n";

		if (!strcmp(carg[1], "new")) {
			if (argn < 4) {
				std::clog << "USAGE: " << carg[0] << " new <symbol> <kronos-code...>\n";
				return -1;
			}
			std::string code;
			for (int i = 3;i < argn;++i) code = code + carg[i] + "\n";
			client.send("new", carg[2], code);
		} else if (!strcmp(carg[1], "delete")) {
			if (argn != 3) {
				std::clog << "USAGE: " << carg[0] << " delete <symbol>\n";
				return -1;
			}
			client.send("delete", carg[2]);
		} else if (!strcmp(carg[1],"send")) {
			if (argn < 3) {
				std::clog << "USAGE: " << carg[0] << " send <method> [number] [number] ...\n";
				return -1;
			}

			o2_send_start();
			for (int i = 3;i < argn;++i) {
				o2_add_float(strtof(carg[i], nullptr));
			}
			o2_send_finish(0, carg[2], 0);

		} else {
			std::string code;
			for (int i = 1;i < argn;++i) code = code + carg[i] + "\n";
			client.send("run", code);
		}
	} catch (std::range_error& e) {
		std::cerr << "* " << e.what() << " *" << std::endl;
		std::cerr << "Try '" << carg[0] << " -h' for a list of parameters\n";
		return -3;
	} catch (std::exception& e) {
		std::cerr << "* Runtime error: " << e.what() << " *" << std::endl;
		return -1;
	}
	return 0;
}
