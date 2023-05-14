#define EXPAND_PARAMS \
	F(main, m, std::string("Main()") , "<expr>", "Compile the expression <expr>") \
	F(com, c, 3, "<COM>", "Communicate with Atlys board at COM-port numer <COM>") \
	F(wcc, w, std::string("WaveCoreIDE.exe"), "<command>", "Command to invoke the WaveCore compiler") \
	F(prefix, p, std::string("kronos"), "<prefix>", "Prefix generated files with <prefix>") \
	F(help, h, false, "", "help; display this user guide")

#include "CmdLineMacro.h"
#include "kronos.h"

#include <sstream>
#include <algorithm>
#include <chrono>
#include <regex>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <stdlib.h>
#include <stdio.h>

void FormatErrors(const std::string& xml, std::ostream& out, Kronos::Context& cx);

struct {
	std::uint32_t NativeControlBaseAddress = -1;
	std::uint32_t NumControlTokens = 0;
	std::unordered_map<int, std::uint32_t> TokenAddress;
} WaveCoreObject;

int main( int argn, const char *carg[] ) {
	using namespace Kronos;
	Kronos::Context cx;
	std::stringstream log;

	try {
		CompilerParser ci(carg + 1, carg + argn);
		if (ci.help) {
			ci.ShowHelp(
				"KREPL; Kronos " KRONOS_PACKAGE_VERSION " REPL \n(c) 2015 Vesa Norilo, University of Arts Helsinki\n\n"
				"PARAMETERS\n\n");
			return 0;
		}

		Context myContext = CreateContext(); {
			cx = myContext;

			for (auto p : ci.positional) {
				myContext.ImportFile(p);
			}

			Class myClass = myContext.Make("WaveCore", ci.main.c_str(), GetNil(), &log, 0, Kronos::Default, "host", "host");

			float sampleRate = 48000.f;
			auto rateKey = GetPair(GetString("audio"), GetString("Rate"));
			if (myClass.HasVar(rateKey)) {
				myClass.SetVar(rateKey, &sampleRate);
			}

			remove("../temp/WaveCore_obj.txt");
			myClass.MakeStatic(ci.prefix.c_str(), "wavecore", std::cout);

			system((ci.wcc + " " + ci.prefix + ".script").c_str());

			std::stringstream objectFileString;
			std::ifstream objectFileStream("../temp/WaveCore_obj.txt");

			if (!objectFileStream.is_open()) {
				throw Kronos::RuntimeError("Couldn't open '../temp/WaveCore_obj.txt' for parsing.");
			}

			objectFileString << objectFileStream.rdbuf();

			auto objectString = objectFileString.str();

			std::regex findEdgeBaseAddress("Process NativeControl[\\s\\S]*EdgeBaseAddress\\s+0x([0-9]+)[\\s\\S]*EndProcess NativeControl");
			std::regex findControlTokens("Signal .ControlToken([0-9]+)\\s0x([0-9]+)");

			std::smatch edgeBaseAddress;
			if (std::regex_search(objectString, edgeBaseAddress, findEdgeBaseAddress)) {
				std::smatch controlTokenAddresses;
				auto str = edgeBaseAddress.str();
				WaveCoreObject.NativeControlBaseAddress = strtol(edgeBaseAddress[1].str().c_str(), nullptr, 16);
				if (std::regex_search(str, controlTokenAddresses, findControlTokens)) {
					for (int i = 0;i < controlTokenAddresses.size();i += 3) {
						std::cout << "Token " << controlTokenAddresses[i+1] << " at " << controlTokenAddresses[i + 2] << "\n";
						WaveCoreObject.TokenAddress[
							strtol(controlTokenAddresses[i + 1].str().c_str(),nullptr,10)] =
							strtol(controlTokenAddresses[i + 2].str().c_str(),nullptr,16);
					}
				}
			} 

			//Instance myInst = myClass.Cons();

			//std::vector<uint8_t> outbuf(myClass.TypeOfResult().SizeOf());
			//myInst.Evaluate(nullptr, outbuf.data());
			//myClass.TypeOfResult().ToStream(std::cout, outbuf.data());
		}        
	} catch (Kronos::IProgramError& pe) {
		std::cerr << "* Program Error E" << pe.GetErrorCode() << ": " << pe.GetSourceFilePosition() << "; " << pe.GetErrorMessage() << " *\n";
		if (cx) FormatErrors(log.str(), std::cerr, cx);
		return pe.GetErrorCode();
	} catch (Kronos::IError &e) {
		std::cerr << "* Compiler Error: " << e.GetErrorMessage( ) << " *" << std::endl;
		return -2;
	} catch (std::range_error& e) {
		std::cerr << "* " << e.what( ) << " *" << std::endl;
		std::cerr << "Try '" << carg[0] << " -h' for a list of parameters\n";
		return -3;
	} catch (std::exception& e) {
		std::cerr << "* Runtime error: " << e.what( ) << " *" << std::endl;
		return -1;
	}
	return 0;
}