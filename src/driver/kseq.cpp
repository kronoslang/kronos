#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <vector>
#include <chrono>
#include <thread>

#include "IOProvider.h"
#include "kseqparser.h"
#include "CmdLineOpts.h"

#define EXPAND_PARAMS \
	F(input, i, std::string("-"), "<path>", "input source file name; '-' for stdin") \
	F(main, m, std::string("Main()"), "<expr>", "main; expression to connect to audio output") \
	F(dump_hex, DH, false, "", "write audio output to stdout as a stream of interleaved 16-bit hexadecimals") \
	F(quiet, q, false, "", "quiet mode; suppress printing to stdout") \
	F(length, len, 0, "<samples>", "compute audio output for the first <samples> frames") \
	F(profile, P, false, "", "measure CPU utilization for each signal clock") \
	F(log_sequencer, ls, false, "", "log processor output for each sequencer input") \
	F(help, h, false, "", "help; display this user guide")

namespace CmdLine {
#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) Option<decltype(DEFAULT)> LONG(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION);
	EXPAND_PARAMS
#undef F
}

void FormatErrors(const std::string& xml, std::ostream& out, Kronos::Context& cx);

namespace Kronos {
	namespace IO {
		IDriver* _CreateAudioDumper(float sr, int len, int decimate, std::ostream& dump);
		static Driver CreateAudioDumper(float sr, int len, int decimate, std::ostream& dump) {
			return _CreateAudioDumper(sr, len, decimate, dump);
		}
	}
}

int main(int n, const char *carg[]) {
	using namespace Kronos;
	std::stringstream log;
	Context cx;
	try {
		std::list<const char*> args;
		for (int i(1);i < n;++i) args.emplace_back(carg[i]);
		AddBackendCmdLineOpts(CmdLine::Registry());
		CmdLine::Registry().Parse(args);

		int64_t crc8 = 0, crc16 = 0, crc24 = 0;
		int numOutChannels = 0;

		if (CmdLine::help()) {
			CmdLine::Registry().ShowHelp(std::cout,
				"KSEQ; Kronos " KRONOS_PACKAGE_VERSION " Command Line Sequencer\n(c) 2015 Vesa Norilo, University of Arts Helsinki\n\n"
				"PARAMETERS\n\n");
			return 0;
		}

		Context myContext = CreateContext(); {
			cx = myContext;

			for (auto p : args) {
				myContext.ImportFile(p);
			}

			IO::Manager().Configure(cx);

			Class myClass = myContext.Make("llvm", CmdLine::main.Get().c_str( ), GetNil( ), &log, 0, Kronos::Default, "host", "host");

			std::istream* strm = &std::cin;
			std::ifstream file;
			if (CmdLine::input() != "-") {
				file.open(CmdLine::input());
				if (!file.is_open()) throw std::runtime_error("'" + CmdLine::input() + "' couldn't be opened");
				strm = &file;
			}

			if (!CmdLine::quiet()) std::clog << CmdLine::main() << " -> " << myClass.TypeOfResult() << "\n";

			if (CmdLine::quiet() == false) {
				std::clog << "\n[DSP Inputs]\n\n";
				for (const auto& md : myClass.GetListOfVars()) {
					std::clog << std::left << std::setw(20) << std::string("[" + md.AsString() + "]") << " : "
						<< std::left << std::setw(12) 
						<< myClass.TypeOfVar(md) << std::endl;
				}

				std::clog << "\n[DSP Output Reactivity]\n\n";
				myClass.ReactivityOfResult().ToStream(std::cout, nullptr);
				std::clog << "\n";
			}

			IO::Connector connect;

			if (CmdLine::dump_hex()) {
				if (CmdLine::length() == 0) throw std::runtime_error("Infinite file length requested");
				auto HexDumper = Kronos::IO::CreateAudioDumper(44100.f, CmdLine::length(), 8, std::cout);

				auto Connect = HexDumper->Associate(myClass);

				Instance myInst = myClass.Cons();
				
				Connect(myInst);

				IO::Sequencer EventQueue;
				HexDumper->LinkSequencer(EventQueue);
				EDNSequenceParser HexDumpSequencer(EventQueue, myInst);
				EDN::Lex(HexDumpSequencer, *strm);

				HexDumper->Start();
				HexDumper->Stop();
			} else {
				connect = IO::Manager().Associate(myClass);

				if (CmdLine::profile()) {
					std::clog << "\n[CPU Utilization]\n\n";
					static const int numTriggersPerLoop = 1024;
					std::vector<float> tmpOut(myClass.TypeOfResult().SizeOf() * numTriggersPerLoop);
					for (auto tk : myClass.GetListOfTriggers()) {
                        auto profileInst = myClass.Cons();
                        IO::Connection profileCon(connect, profileInst);
						auto trig = profileInst.GetTrigger(tk);
						if (trig) {
							auto clk = std::chrono::high_resolution_clock();
							auto beg = clk.now();
							auto end = beg;
							int numTriggers = 0;
							while (end - beg < std::chrono::milliseconds(200)) {
								trig.Go(tmpOut.data(), numTriggersPerLoop);
								numTriggers++;
								end = clk.now();
							}

							std::cout << tk << " update time (" << numTriggersPerLoop << " frames): " << (std::chrono::duration_cast<std::chrono::microseconds>(end - beg).count() / numTriggers) << "us\n";
						}
					}
				}

				Instance myInst = myClass.Cons();
                IO::Connection myCon(connect, myInst);

				IO::Sequencer EventQueue;
				IO::Manager().LinkSequencer(EventQueue);
				EDNSequenceParser SeqParser(EventQueue, myInst);

				if (EDN::Lex(SeqParser, *strm)) {
					EventQueue.Add(SeqParser.TimeStamp, Input(), nullptr, 0);
				}

				IO::Manager().Start();

				do {
					try {
						if (*strm) EDN::Lex(SeqParser, *strm);
					} catch (std::range_error& re) {
						if (CmdLine::quiet() == false) std::cerr << "* " << re.what() << " *\n";
						strm->clear();
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(50));
				} while (EventQueue.Pending());

				IO::Manager().Stop();
			}
		}
	} catch (Kronos::IProgramError& pe) {
		std::cerr << "* Program Error E" << pe.GetErrorCode() << ": " << pe.GetSourceFilePosition() << "; " << pe.GetErrorMessage() << " *\n";
		if (cx) FormatErrors(log.str(), std::cerr, cx);
		return pe.GetErrorCode();
	} catch (Kronos::IError &e) {
		std::cerr << "* Compiler Error: " << e.GetErrorMessage() << " *" << std::endl;
		return -2;
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
