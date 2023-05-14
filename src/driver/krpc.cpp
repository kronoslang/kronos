#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <cstring>

#include "CmdLineOpts.h"
#include "config/system.h"
#include "config/corelib.h"
#include "driver/package.h"
#include "kronos.h"
#include "JsonRPCRepl.h"
#include "CmdLineOpts.h"

using namespace std::string_literals;

#define EXPAND_PARAMS \
	F(help, h, false, "", "help; display this user guide")

namespace CL {
	using namespace CmdLine;
#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) Option<decltype(DEFAULT)> LONG(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION);
	EXPAND_PARAMS
#undef F
}

void FormatErrors(const char* xml, std::ostream& out, Kronos::Context& cx, int startIndent = 0);

Kronos::Context cx;

int main( int argn, const char *carg[] ) {
	using namespace Kronos;

	std::list<const char*> args;
	Kronos::AddBackendCmdLineOpts(CmdLine::Registry());
	for (int i(1);i < argn;++i) args.emplace_back(carg[i]);

	Packages::DefaultClient bbClient;

	int err = 0;

	try {
		if (auto badOption = CmdLine::Registry().Parse(args)) {
			throw std::invalid_argument("Unknown command line option: "s + badOption);
		}

		std::clog << 
			"KRPC; Kronos " KRONOS_PACKAGE_VERSION 
			" RPC Endpoint \n(c) 2017-" KRONOS_BUILD_YEAR 
			" Vesa Norilo, University of the Arts Helsinki\n\n";

		cx = CreateContext(Packages::CloudClient::ResolverCallback, &bbClient);

		std::stringstream config;
		auto io = IO::CreateCompositeIO();
        std::string coreRepo, coreVersion;
        cx.GetCoreLibrary(coreRepo, coreVersion);
		REPL::JiT::Compiler compiler(cx, bbClient.Resolve(coreRepo, "VM.k", coreVersion));
		REPL::CompilerConfigurer cfg{ compiler, io.get() };
		io->AddDelegate(cfg);
		compiler.Parse(config.str());

		compiler.SetLogFormatter([](Context& cx, const std::string& xml, std::ostream& fmt) {
			FormatErrors(xml.c_str(), fmt, cx, -4);
		});

		std::mutex outLock;
		auto out = [&](const picojson::value& v) {
			std::lock_guard<std::mutex> lg{ outLock };
			JsonRPC::Put(std::cout, v);
		};

		RPCRepl rpcEnv(io.get(), compiler, out);
		Kronos::JsonRPCEndpoint(rpcEnv, 
			std::bind(JsonRPC::Get, std::ref(std::cin)), 
			out, bbClient);
		rpcEnv.Shutdown();

		io.reset();

	} catch (Kronos::IError &e) {
		std::cerr << "* Compiler Error: " << e.GetErrorMessage( ) << " *" << std::endl;
		err = e.GetErrorCode();
	} catch (std::range_error& e) {
		std::cerr << "* " << e.what( ) << " *" << std::endl;
		std::cerr << "Try '" << carg[0] << " -h' for a list of parameters\n";
		err = -1;
	} catch (std::exception& e) {
		std::cerr << "* Runtime error: " << e.what( ) << " *" << std::endl;
		err = -2;
	}
	return err;
}

#ifdef _MSC_VER
#pragma comment(linker, "/include:kronos_debug_compiler")
#endif


KRONOS_ABI_EXPORT int64_t kronos_debug_compiler(const char *filter) {
	cx.SetCompilerDebugTraceFilter(filter);
	return 1;
}
