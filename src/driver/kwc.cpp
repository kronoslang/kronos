#define EXPAND_PARAMS \
	F(main, m, std::string("Main()") , "<expr>", "Compile the expression <expr>") \
	F(com, c, 3, "<COM>", "Communicate with Atlys board at COM-port numer <COM>") \
	F(wcc, w, std::string("./"), "<path>", "Invoke the WaveCore Compiler programs in <path>") \
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
#include <thread>
#include <chrono>
#include <map>
#include <list>

#define WIN32_LEAN_AND_MEAN
#define VC_EXTRALEAN
#include <Windows.h>

#include "AtlysInterface.h"

void FormatErrors(const std::string& xml, std::ostream& out, Kronos::Context& cx);

static const float zigzag[30] = { -12,12,-12,12,-12,12,-12,12, -12,12,-12,12, -12,12,-12,12, -12,12,-12,12, -12,12,-12,12, -12,12,-12,12, -12,12 };
static const float unity[30] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0 };
static const float oneup[30] = { 0,0,0,0, 0,0,0,0, 12,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0 };
static const float onedown[30] = { 0,0,0,0, -12,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0 };

struct {
	std::uint32_t NativeControlBaseAddress = -1;
	std::uint32_t NumControlTokens = 0;
	std::unordered_map<int, std::uint32_t> TokenAddress;
	std::map<int, float> UpdateValues;
	std::map<int, float> TokenState;
} WaveCoreObject;

extern "C" __declspec(dllexport) float WaveCoreTransmit(std::int32_t token, float value) {
	if (WaveCoreObject.TokenState[token] != value) {
		WaveCoreObject.UpdateValues[token] = value;
	}
	return value;
}

static void TransmitWaveCoreTokens(SerialPort& com) {

	if (WaveCoreObject.UpdateValues.size()) {
		std::vector<char> transmit;
		DWORD base1 = DDRBASEADDRESS + WaveCoreObject.NativeControlBaseAddress * 2;
		DWORD base2 = base1 + 2 * WaveCoreObject.NumControlTokens;

		//std::vector<float> tokenDump(WaveCoreObject.NumControlTokens * 2);
		//for (auto &upd : WaveCoreObject.UpdateValues) {
		//	WaveCoreObject.TokenState[upd.first] = upd.second;
		//}

		//for (int i = 0;i < WaveCoreObject.NumControlTokens;++i) {
		//	int addr = WaveCoreObject.TokenAddress[i];
		//	tokenDump[addr + WaveCoreObject.NumControlTokens] =
		//		tokenDump[addr] = WaveCoreObject.TokenState[i];
		//}
		//com.Encode(base1, transmit);
		//WORD bytes = tokenDump.size() * 2;
		//assert(bytes == bytes & 0x7fff);
		//transmit.push_back((bytes >> 8) & 0x7f);
		//transmit.push_back((bytes >> 0) & 0xff);
		//for (auto f : tokenDump) {
		//	com.Encode(*(DWORD*)&f, transmit);
		//}
		//com.Write(transmit);
		//return;

		for (auto &upd : WaveCoreObject.UpdateValues) {
			int tokenAddr = WaveCoreObject.TokenAddress[upd.first] * 2;
			DWORD *dw = (DWORD*)&upd.second;
			com.Encode(base1 + tokenAddr, *dw, transmit);
			com.Encode(base2 + tokenAddr, *dw, transmit);
			WaveCoreObject.TokenState[upd.first] = upd.second;
		}
		com.Write(transmit);
		WaveCoreObject.UpdateValues.clear();
		std::ofstream dump("kwcctrl.bin", std::ios_base::binary);
		dump.write(transmit.data(), transmit.size());
	}
}

int main( int argn, const char *carg[] ) {
	using namespace Kronos;
	Kronos::Context cx;
	std::stringstream log;

	try {
		std::list<std::string> args;
		for (int i(1);i < argn;++i) args.emplace_back(carg[i]);
		CmdLineParser ci(args);

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

			remove("../temp/WaveCore_obj.txt");

			float timerRate = 50.f;
			int vectorLen = 256;

			float sampleRate = timerRate * vectorLen;
			auto rateKey = GetPair(GetString("audio"), GetString("Rate"));
			if (myClass.HasVar(rateKey)) {
				myClass.SetVar(rateKey, &sampleRate);
			}

			myClass.MakeStatic(ci.prefix.c_str(), "wavecore", std::cout);
			auto myDriver = myClass.Cons();

			system((ci.wcc + "WaveCoreIDE.exe " + ci.prefix + ".script").c_str());

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
				for (auto match = std::sregex_iterator(str.begin(), str.end(), findControlTokens);
					 match != std::sregex_iterator(); ++match) {

					std::cout << "Token " << (*match)[1] << " at 0x" << std::hex << (*match)[2] << "\n";
					WaveCoreObject.NumControlTokens++;
					WaveCoreObject.TokenAddress[
						strtol((*match)[1].str().c_str(), nullptr, 10)] =
						strtol((*match)[2].str().c_str(), nullptr, 16);
				}
			}

			// generate the wavecore binary
			remove("WaveCore.bin");
			system((ci.wcc + "WaveConsole2.2.exe").c_str());
			std::ifstream wcObjectFile("WaveCore.bin", std::ios_base::binary);
			std::vector<char> wcObjectBlob;
			while (true) {
				size_t read(16384);
				auto sz = wcObjectBlob.size();
				wcObjectBlob.resize(sz + read);
				auto didread = wcObjectFile.rdbuf()->sgetn(wcObjectBlob.data() + sz, read);
				wcObjectBlob.resize(wcObjectBlob.size() - read + didread);
				if (didread == 0) break;
			}

			// push the binary to serial port
			SerialPort com(ci.com);
			while (true) {
				com.Write(wcObjectBlob);
				//system("WaveCoreLoad.bat");
				std::cout << "Did LD3 turn on on the Atlys Board? [yN]";
				char c;
				std::cin.read(&c, 1);
				if (c == 'y') break;
			}

			auto commandGains = myDriver["command-gains"];
			if (commandGains) {
				std::cout << "Enter [m]ute, [z]igzag, [u]nity, one-u[p] or one-[d]own for EQ presets...\n";
				while (true) {
					char c(0);
					std::cin.read(&c, 1);
					switch (c) {
					case 'm': for (int i(0);i < WaveCoreObject.NumControlTokens;++i) WaveCoreObject.UpdateValues[i] = 0; break;
					case 'z': commandGains = zigzag; break;
					case 'u': commandGains = unity; break;
					case 'p': commandGains = oneup; break;
					case 'd': commandGains = onedown; break;
					}
					TransmitWaveCoreTokens(com);
				}
			}

			// loop control clock
			auto timer = std::chrono::high_resolution_clock();
			auto ts = timer.now();
			auto audioClock = myDriver.GetTrigger(Kronos::GetString("audio"));
			while (true) {
				using namespace std::chrono;
				auto nts = timer.now();
				auto numFrames = duration_cast<milliseconds>(nts - ts).count() * 1000 / sampleRate;
				ts += milliseconds(long long((numFrames * 1000ll) / sampleRate));				
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
				if (audioClock) audioClock.Go(nullptr, numFrames);
				TransmitWaveCoreTokens(com);
			}
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