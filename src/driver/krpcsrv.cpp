#include "network/router.h"
#include "kronos.h"
#include "common/PlatformUtils.h"
#include "driver/CmdLineOpts.h"
#include "driver/package.h"
#include "config/corelib.h"
#include "JsonRPCRepl.h"
#include "paf/PAF.h"

#include <iostream>
#include <fstream>

using namespace std::string_literals;

#define EXPAND_PARAMS                                                        \
  F(import, i, std::list<std::string>(), "<module>",                         \
    "Import source file <module>")                                           \
  F(import_path, ip, std::list<std::string>(),"<path>",						 \
    "Add paths to look for imports in")										 \
  F(port, p, "15051"s, "<protocol>",                             \
    "listen to TCP port <protocol> for http requests")                       \
  F(wideopen, wideopen, false, "", "Accept connections from network (default localhost only)") \
  F(root, r, ""s, "<directory>", "serve files from <directory>") \
  F(help, h, false, "", "help; display this user guide")

namespace CL {
	using namespace CmdLine;
#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) Option<decltype(DEFAULT)> LONG(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION);
	EXPAND_PARAMS
#undef F
}

void FormatErrors(const char* xml, std::ostream& out, Kronos::Context& cx, int startIndent = 0);

Packages::CloudClient* AssetManager = nullptr;

void PostAsset(Sxx::Socket& sock, const std::string& uri, const Sxx::http::Request& req, Sxx::http::Response& resp) {
	auto filePath = GetCachePath() + "/assets/" + uri;
	resp.Headers["Access-Control-Allow-Origin"] = "*";
	if (Packages::CloudClient::DoesFileExist(filePath)) {
		resp.ResultCode = resp.OK;
		return;
	}
	Packages::MakeMultiLevelPath(filePath);
	std::ofstream writeFile(filePath, std::ios_base::binary);
	if (writeFile.is_open()) {
		std::clog << "* Caching " << filePath << " (" << req.Body.size() << " bytes)\n";
		writeFile.write(req.Body.data(), req.Body.size());
		resp.ResultCode = resp.Created;
		return;
	} else {
		resp.ResultCode = resp.NotAcceptable;
		return;
	}
}

void* CachedAssetProvider(const char *uri, const Kronos::IType** ty, void*) {

	std::string filePath = GetCachePath() + "/assets/" + uri;
	if (Packages::CloudClient::DoesFileExist(filePath)) {
		auto readFile = PAF::AudioFileReader(filePath.c_str());
		if (readFile) {
			auto numCh = readFile->Get(PAF::NumChannels);
			auto smpRate = readFile->Get(PAF::SampleRate);

			std::vector<float> memory;
			readFile->Stream([&](const float* data, int numSamples) {
				auto at = memory.size();
				memory.resize(at + numSamples);
				std::copy(data, data + numSamples, memory.data() + at);
				return numSamples;
			});

			auto assetType = Kronos::GetList(Kronos::GetTuple(Kronos::GetFloat32Ty(), numCh), memory.size() / numCh);

			assetType = Kronos::GetUserType(Kronos::GetBuiltinTag(Kronos::TypeTag::AudioFile),
											Kronos::GetPair(Kronos::GetConstant(smpRate),
															assetType));

			assetType.Get()->Retain();
			*ty = assetType.Get();

			auto mem = malloc(memory.size() * sizeof(float));
			memcpy(mem, memory.data(), memory.size() * sizeof(float));

			return mem;
		}
	}
	return nullptr;
}

int main(int argn, const char* carg[]) {
	using namespace Kronos;
	using namespace Sxx;


	Packages::DefaultClient bbClient;
	AssetManager = &bbClient;

	int resultCode = 0;

	try {
		std::list<const char*> args;
		Kronos::AddBackendCmdLineOpts(CmdLine::Registry());
		for (int i(1);i < argn;++i) args.emplace_back(carg[i]);
		if (auto badOpt = CmdLine::Registry().Parse(args)) {
			throw std::invalid_argument("Unknown command line option: "s + badOpt);
		}

		if (CL::help()) {
			CmdLine::Registry().ShowHelp(std::cout,
							"KRPCSRV; Kronos " KRONOS_PACKAGE_VERSION 
							" REPL \n(c) 2017-" KRONOS_BUILD_YEAR " Vesa Norilo, University of Arts Helsinki\n\n"
							"PARAMETERS\n\n");
			return 0;
		}

		std::stringstream config;
		auto io = IO::CreateCompositeIO();	

		std::cout << 
			"KRPCSRV; Kronos " KRONOS_PACKAGE_VERSION " Websocket REPL\n"
			"(c)2017 - " KRONOS_BUILD_YEAR " Vesa Norilo, University of Arts Helsinki\n\n";

		std::cout
			<< "Listening on port " << CL::port() << " for "
			<< (CL::wideopen() ? "!!!REMOTE!!!" : "local") << " connections\n";

		Serve(CL::port(), CL::wideopen(), Route({
			{ "echo", Responders::Websocket([](Responders::IWebsocketStream& wss) {
				std::vector<char> buf;
				while (wss.IsGood()) {
					const char *end = wss.Read(buf);
					wss.Write(buf.data(), end - buf.data());
				} 
			}) }, 
			{ "asset", PostAsset },
			{ "repl",
			Responders::Websocket([&](Responders::IWebsocketStream& wss) {
				std::vector<char> buf;

				std::cout << "[" << wss.GetHttpRequest().Peer << "] <- " << wss.GetHttpRequest().Uri << "\n";

				auto cx = Kronos::CreateContext(Packages::CloudClient::ResolverCallback, &bbClient);
				cx.SetAssetLinker(CachedAssetProvider, nullptr);
				std::string coreRepo, coreVersion;
				cx.GetCoreLibrary(coreRepo, coreVersion);

				REPL::JiT::Compiler compiler(cx, bbClient.Resolve(coreRepo, "VM.k", coreVersion));
				REPL::CompilerConfigurer cfg{ compiler, io.get() };
				io->AddDelegate(cfg);

				compiler.SetLogFormatter([](Context& cx, const std::string& xml, std::ostream& fmt) {
					FormatErrors(xml.c_str(), fmt, cx, -4);
				});

				std::mutex wssLock;
				auto outputFunction = [&](const picojson::value& v) {
					auto str = v.serialize();
					std::lock_guard<std::mutex> lg{ wssLock };
					wss.Write(str.data(), str.size());
				};

				RPCRepl rootEnv(io.get(), compiler, outputFunction);

				try {
					cx.RegisterSpecializationCallback("rpc-monitor", [](void *user, int diags, const IType* ty, int64_t tyUid) {
						if (!diags) return;
						std::stringstream tyStr;
						StreamBuf sbuf(tyStr.rdbuf());
						ty->ToStream(sbuf, nullptr, IType::JSON);
						picojson::value payload;

						std::stringstream valStr;

						auto tyS = tyStr.str();
						valStr << Runtime::Value{ tyS.c_str(), nullptr };

						picojson::parse(payload, valStr);

						picojson::value monitor = picojson::object {
							{"jsonrpc", "2.0"},
							{"method", "rpc-monitor"},
							{"params", picojson::object {
								{ "success", tyUid != 0 },
								{ "result", payload }
							}}
						};

						auto monitorStr = monitor.serialize();
						((Responders::IWebsocketStream*)user)->Write(monitorStr.data(), monitorStr.size());
					}, &wss);

					JsonRPCEndpoint(rootEnv, [&]() -> picojson::value {
						auto end = wss.Read(buf);
						const char *beg = buf.data();

						if (end == beg) {
							return picojson::object{
								{ "connection_closed", true }
							};
						}

						picojson::value val;
						if (picojson::parse(val, beg, end).size()) {
							val = picojson::value();
						}
						return val;
					}, outputFunction, bbClient);
				} catch (std::exception& e) {
					picojson::object error{
						{"jsonrpc", "2.0" },
						{"error", picojson::object{
							{ "code", -32601.0 },
							{ "message", e.what() }
						} }
					};
					auto str = picojson::value(error).serialize();
					wss.Write(str.data(), str.size());
					std::cerr << str;
				}
				rootEnv.Shutdown();

			})}
		}));
	} catch (std::exception& e) {
		std::cerr << "** " << e.what() << " **\n";
		resultCode = -1;
	}
	return resultCode;
}
