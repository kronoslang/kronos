#include <iostream>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <cstring>

#include "CmdLineOpts.h"
#include "config/system.h"
#include "driver/package.h"
#include "kronos.h"
#include "friends.h"
#include "TestInstrumentation.h"
#include "ReplEnvironment.h"
#include "runtime/inout.h"

using namespace std::string_literals;

#define EXPAND_PARAMS \
	F(deterministic_scheduling, ds, false, "", "Only execute a script once all dependencies are compiled as well")\
	F(interactive, I, false, "", "Prompt the user for additional expressions to evaluate") \
	F(format, f, "text"s, "<text|edn|xml>","Format REPL responses as...") \
	F(import, i, std::list<std::string>(), "<module>", "Import source file <module>" ) \
	F(help, h, false, "", "help; display this user guide")

Kronos::Context cx;
namespace CL {
	#define F(LONG, SHORT, DEFAULT, LABEL, DESCR) static CmdLine::Option<decltype(DEFAULT)> LONG;
	EXPAND_PARAMS
	#undef F

	static void SetRegistry(CmdLine::IRegistry& reg) {
	#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) \
	LONG.Init(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION, &reg); 
	EXPAND_PARAMS
	#undef F
	}
};

void FormatErrors(const char* xml, std::ostream& out, Kronos::Context& cx, int startIndent = 0);

int krepl_main(CmdLine::IRegistry& CLOpts, Kronos::IO::IConfiguringHierarchy* io, int argn, const char *carg[] ) {
	using namespace Kronos;
	
	int err = 0;

	CL::SetRegistry(CLOpts);

	try {
		Packages::DefaultClient bbClient;

		std::list<const char*> args;
		if (!cx) Kronos::AddBackendCmdLineOpts(CLOpts);
		for (int i(1);i < argn;++i) args.emplace_back(carg[i]);
		
		if (auto badOption = CLOpts.Parse(args)) {
			throw std::invalid_argument("Unknown command line option: "s + badOption);
		}

		std::list<std::string> repl_args;
		for (auto &a : args) repl_args.emplace_back(a);

		if (CL::help()) {
			CLOpts.ShowHelp(std::cout,
					"KREPL; Kronos " KRONOS_PACKAGE_VERSION " REPL \n"
					"(c) 2017-" KRONOS_BUILD_YEAR " Vesa Norilo, University of Arts Helsinki\n\n"
					"PARAMETERS\n\n");
            
            std::cout << "AUDIO DEVICES\n\n";
            Kronos::IO::ListAudioDevices(std::cout);
            std::cout << "\n";
            
			return 0;
		}

		if (repl_args.size() < 1) CL::interactive = true;

		cx = CreateContext(Packages::DefaultClient::ResolverCallback, &bbClient);

		for (auto import : CL::import()) {
			std::ifstream file{ import };
			std::stringstream code;
			code << file.rdbuf();
			cx.ImportBuffer(code.str(), true);
//			cx.ImportFile(import);
		}

		if (CL::interactive() && repl_args.empty()) {
			std::stringstream banner;
			std::string repo, repoVersion;
			cx.GetCoreLibrary(repo, repoVersion);
			banner << "\"Welcome to KREPL " << KRONOS_PACKAGE_VERSION << " [" << repo << " " << repoVersion << "]\"";
			repl_args.emplace_back(banner.str());
		}

		std::string coreRepo, coreVersion;
		cx.GetCoreLibrary(coreRepo, coreVersion);

		REPL::JiT::Compiler compiler{ cx, bbClient.Resolve(coreRepo, "VM.k", coreVersion) };
		REPL::CompilerConfigurer cfg{ compiler, io };

		compiler.SetLogFormatter([](Context& cx, const std::string& xml, std::ostream& fmt) {
			std::ofstream xmlDump{ "type-error.xml" };
			xmlDump << xml;
			FormatErrors(xml.c_str(), fmt, cx, -4);
		});

		REPL::Console rootEnv(io, compiler);

		if (CL::deterministic_scheduling()) rootEnv.SetDeterministic(true);


#ifndef NDEBUG
		rootEnv.Parse(
			"Compiler-Debug-Trace(flt) { "
			"   Foreign-Function(\"int64\" \"kronos_debug_compiler\" \"const char*\" flt)"
			"} "
		);
#endif

		while (true) {
			if (repl_args.size()) {
				rootEnv.Entry(repl_args.front());
				repl_args.pop_front();
			} else {
				if (CL::interactive()) {
					std::string entry = rootEnv.ReadLine();
					if (entry == "exit" || !std::cin.good()) break;
					repl_args.push_back(entry + "\n");
				} else break;
			}
		}

		if (rootEnv.HasPartialEntry()) {
			throw std::range_error("Command line syntax is incomplete (bracket or quote mismatch)");
		}

		rootEnv.Shutdown();
	} catch (Kronos::IError &e) {
		std::cerr << "* Compiler Error: " << e.GetErrorMessage( ) << " *" << std::endl;
		if (cx) {
			if (auto pos = e.GetSourceFilePosition()) {
				std::cerr << "In " << cx.GetModuleAndLineNumberText(pos) << "\n";
			}
		}
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

// the repl can name and bind anonymous instances

#ifdef _MSC_VER
#pragma comment(linker, "/include:kvm_repl_bind")
#pragma comment(linker, "/include:kvm_repl_bind_init")
#pragma comment(linker, "/include:kronos_debug_compiler")
#endif

KRONOS_ABI_EXPORT int64_t kvm_print(int64_t world, const char *pipe, const char* descr, const void* data);
KRONOS_ABI_EXPORT int64_t kvm_start(int64_t world, int64_t sz, int64_t closureType, const void* closureData);
KRONOS_ABI_EXPORT int64_t kvm_pop(int64_t world, int64_t ty, void* item);

#ifdef _MSC_VER
#include <Windows.h>
void PutOnClipboard(const std::string& s) {
	auto cpy = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
	auto txtHandle = GlobalLock(cpy);
	memcpy(txtHandle, s.data(), s.size() + 1);
	GlobalUnlock(cpy);
	if (OpenClipboard(nullptr)) {
		EmptyClipboard();
		SetClipboardData(CF_TEXT, cpy);
		CloseClipboard();
	}
}
#else 
void PutOnClipboard(const std::string& s) {
}
#endif


template <typename E, size_t SZ> size_t array_size(const E(&)[SZ]) {
	return SZ;
}

KRONOS_ABI_EXPORT int64_t kvm_repl_bind_init(int64_t w, const char* sym, int64_t ty, const void *data) {
	time_t t;
	time(&t);
	srand((unsigned)t);
	return w;
}

KRONOS_ABI_EXPORT int64_t kvm_repl_bind(int64_t w, const char* sym, int64_t ty, const void *data) {
	std::string name = sym;
	if (name.empty()) {
		name = adjective[rand() % array_size(adjective)];
		name += "-";
		name += animal_name[rand() % array_size(animal_name)];
		PutOnClipboard(name);
		kvm_print(w, "out", (name + " appears!\n").c_str(), nullptr);
	}
	cx.BindConstant(name.c_str(), cx.TypeFromUID(ty), data);
	return w;
}

KRONOS_ABI_EXPORT int64_t kronos_debug_compiler(const char *filter) {
	cx.SetCompilerDebugTraceFilter(filter);
	return 1;
}

int krepl_main(Kronos::IO::IConfiguringHierarchy* io, int argn, const char* argv[]) {
	auto reg = CmdLine::NewRegistry();
	return krepl_main(*reg, io, argn, argv);
}
int krepl_main(int argn, const char* argv[]) {
	auto io = Kronos::IO::CreateCompositeIO();
	return krepl_main(io.get(), argn, argv);
}

