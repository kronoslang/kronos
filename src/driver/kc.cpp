#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <vector>
#include <list>
#include <unordered_map>
#include "kronos.h"
#include "CmdLineOpts.h"
#include "config/system.h"
#include "config/corelib.h"
#include "driver/package.h"

#ifdef HAVE_LLVM
#define LLVM_PARAMS F(emit_llvm, ll, false, "", "export symbolic assembly in the LLVM IR format")
#else
#define LLVM_PARAMS 
#endif

#ifdef HAVE_BINARYEN
#define BINARYEN_PARAMS \
	F(emit_wasm, wasm, false, "", "export  binary webassembly") \
	F(emit_wast, wast, false, "", "export webassembly text format") \
	F(emit_js, js, false, "", "export asm.js") 
#else 
#define BINARYEN_PARAMS 
#endif

#define EXPAND_PARAMS \
	F(input, i, std::string(""), "<path>", "input source file name; '-' for stdin") \
	F(output, o, std::string(""), "<path>", "output file name, '-' for stdout") \
	F(import_path, ip, std::list<std::string>(),"<path>", "Add paths to look for imports in") \
	F(header, H, std::string(""), "<path>", "write a C/C++ header for the object to <path>, '-' for stdout") \
	F(main, m, std::string("Main()"), "<expr>", "main; expression to compile") \
	F(arg, a, std::string("nil"), "<expr>", "Kronos expression that determines the type of the external argument to main") \
	F(assembly, S, false, "", "emit symbolic assembly") \
	F(prefix, P, std::string(""), "<sym>", "prefix; namespace for exported symbols") \
	LLVM_PARAMS BINARYEN_PARAMS \
	F(backend, G, std::string(""), "<backend>", "Select backend; default is 'llvm'.") \
	F(mcpu, C, std::string(""), "<cpu>", "engine-specific string describing the target cpu") \
	F(mtriple, T, std::string("host"), "<triple>", "target triple to compile for") \
	F(quiet, q, false, "", "quiet mode; suppress logging to stdout") \
	F(diagnostic, D, false, "", "dump specialization diagnostic trace as XML") \
	F(help, h, false, "", "display this user guide") 

namespace CL {
	using namespace CmdLine;
#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) Option<decltype(DEFAULT)> LONG(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION);
	EXPAND_PARAMS
#undef F
}

using namespace std;

void FormatErrors(const char *xml, std::ostream& out, Kronos::Context& cx, int indent = 0);

bool hasEnding(std::string const &fullString, std::string const &ending) {
	if (fullString.length( ) >= ending.length( )) {
		return (0 == fullString.compare(fullString.length( ) - ending.length( ), ending.length( ), ending));
	} else {
		return false;
	}
}

int main(int n, const char *carg[]) {
	using namespace Kronos;
	stringstream log;
	Context cx;


	Packages::DefaultClient bbClient;

	try {
		std::list<const char*> args;
		Kronos::AddBackendCmdLineOpts(CmdLine::Registry());
		for (int i(1);i < n;++i) args.emplace_back(carg[i]);
		if (auto badOption = CL::Registry().Parse(args)) {
			throw std::invalid_argument("Unknown command line option: "s + badOption);
		}

		if (CL::help()) {
			CL::Registry().ShowHelp(std::cout,
				"KC; Kronos " KRONOS_PACKAGE_VERSION " Static Compiler \n"
				"(c) 2017-" KRONOS_BUILD_YEAR " Vesa Norilo, University of Arts Helsinki\n\n"
				"PARAMETERS\n\n");
			return 0;
		}

		Context myContext = CreateContext(Packages::DefaultClient::ResolverCallback, &bbClient); {
			cx = myContext;

			for (auto p : args) {
				myContext.ImportFile(p);
			}

			if ((args.empty( ) && CL::input().empty( )) || CL::input() == "-") {
				stringstream readStdin;
				readStdin << cin.rdbuf( );
				myContext.ImportBuffer(readStdin.str( ));
			}

			if (CL::input() != "-" && CL::input().size( )) myContext.ImportFile(CL::input());

			std::string expr = CL::main();

			Kronos::Type argType = GetNil();
			if (CL::arg() != "nil") {
				argType = myContext.DeriveType(CL::arg.Get().c_str(), GetNil(), CL::diagnostic() ? &std::cout : &log, 0);
				log.str("");
			}

			auto* stream = &cout;
			ofstream file;
#ifdef WIN32
			string ext = ".obj";
#else
			string ext = ".o";
#endif
			bool symbolicAsm = false;
			if (CL::assembly()) {
				ext = ".s";
				symbolicAsm = true;
			} 
			
			if (false) { }
#ifdef HAVE_LLVM
			else if (CL::emit_llvm()) {
				ext = ".ll";
				symbolicAsm = true;
			} 
#endif
#ifdef HAVE_BINARYEN
			else if (CL::emit_wasm()) {
				ext = ".wasm";
			} 
			else if (CL::emit_wast()) {
				ext = ".wast";
				symbolicAsm = true;
			}
#endif

			if (CL::output.Get().empty( )) {
				if (symbolicAsm) CL::output = "-";
				else {
					if (CL::input.Get().empty( ) || CL::input() == "-") {
						CL::output = "a" + ext;
						if (args.size( )) CL::output = args.front( ) + ext;
					} else CL::output = CL::input() + ext;
				}
			}

			if (CL::output() != "-") {
				file.open(CL::output(), std::ios::binary);
				stream = &file;
				if (CL::output.Get().find('.') != std::string::npos) {
					ext = CL::output().substr(CL::output().find_last_of('.'));
				}
			}

			Kronos::TypedGraph specialization = nullptr;

			try {
				specialization = myContext.Specialize(myContext.Parse(expr.c_str()), argType, nullptr, 3);
			} catch (...) {
				specialization = myContext.Specialize(myContext.Parse(expr.c_str()), argType, CL::diagnostic() ? &std::cout : &log, 3);
				throw;
			}

			if (CL::quiet() == false) {
				clog << CL::main() << " -> " << specialization.TypeOfResult() << endl;
			}
				
			if (CL::header().empty( ) == false) {
				std::ostream* stream = &std::cout;
				std::ofstream headerFile;

				if (CL::header() == "-") stream = &std::cout;
				else {
					headerFile.open(CL::header());
					stream = &headerFile;
				}

				if (CL::quiet() == false) std::clog << "Writing " << CL::header() << "... ";
				//MakeCppHeader(CL::prefix.Get().empty( ) ? "KronosDSP" : CL::prefix().c_str( ), myClass, GetNil( ), *stream);
				if (CL::quiet() == false) std::clog << "OK\n";
			}

			if (CL::quiet() == false && stream != &cout) {
				clog << "Writing " << CL::output() << "... ";
			}

			
			myContext.Make(CL::prefix().c_str(), ext.c_str(), 
				*stream,
				CL::backend().c_str(), specialization,
				Kronos::Default,
				CL::mtriple().c_str(),
				CL::mcpu().c_str());

			stream->flush();

			if (CL::quiet() == false && stream != &cout) std::cout << "OK\n";
		}
	} catch (Kronos::IProgramError& pe) {
		std::cerr << "* Program Error E" << pe.GetErrorCode( ) << ": " << pe.GetSourceFilePosition( ) << "; " << pe.GetErrorMessage( ) << " *\n";
		if (cx) FormatErrors(log.str( ).c_str(), cerr, cx);
  		return pe.GetErrorCode( );
	} catch (Kronos::IError &e) {
		cerr << "* Compiler Error: " << e.GetErrorMessage( ) << " *" << endl;
		return -2;
	} catch (std::exception& e) {
		cerr << "* Runtime error: " << e.what( ) << " *" << endl;
		return -1;
	}
 	return 0;  
} 
