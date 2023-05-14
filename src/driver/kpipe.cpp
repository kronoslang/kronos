#define EXPAND_PARAMS \
	F(input, i, std::string(""), "<path>", "input soundfile") \
	F(output, o, std::string(""), "<path>", "output soundfile") \
	F(tail, t, 0, "<samples>", "set output file length padding relative to input") \
	F(bitdepth, b, 0, "", "override bit depth for output file") \
	F(bitrate, br, 0, "", "override bitrate for output file") \
	F(samplerate, sr, 0, "", "override sample rate for output file") \
	F(expr, e, std::string("Main"), "<expr>", "function to apply to the soundfile") \
	F(quiet, q, false, "", "quiet mode; suppress logging to stdout") \
	F(import_path, ip, std::list<std::string>(),"<path>", "Add paths to look for imports in") \
	F(help, h, false, "", "display this user guide") 

#include "CmdLineMacro.h"
#include "kronos.h"
#include "PAF.h"

#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>

using namespace std;

void FormatErrors(const std::string& xml, std::ostream& out, Kronos::Context& cx);
void GetRequestedIOConfig(const Kronos::StaticClass& cls, int& numIns, int& numOuts);

int main(int n, const char *carg[]) {
	using namespace Kronos;
	stringstream log;
	Context cx;

	try {
		cx = Kronos::CreateContext( );

		std::list<std::string> args;
		for (int i(1);i < n;++i) args.emplace_back(carg[i]);
		CmdLineParser ci(args);

		if (ci.help) {
			ci.ShowHelp(
				"KPIPE; Kronos " KRONOS_PACKAGE_VERSION " Soundfile Processor\n(c) 2015 Vesa Norilo, University of Arts Helsinki\n\n"
				"PARAMETERS\n\n");
			return 0;
		}

		for (auto&& file : ci.positional) {
			cx.ImportFile( file );
		}

		PAF::AudioFileReader soundFileIn(ci.input.c_str( ));
		PAF::AudioFileWriter soundFileOut(ci.output.c_str( ));
		std::stringstream src, log;

		int numIns = 0;

		if (!soundFileOut) throw std::runtime_error("Couldn't open output file");

		if (soundFileIn) {
			if (soundFileOut->Has(PAF::BitDepth)) {
				if (soundFileIn->Has(PAF::BitDepth)) soundFileOut->Set(PAF::BitDepth, soundFileIn[PAF::BitDepth]);
				if (ci.bitdepth) soundFileOut->Set(PAF::BitDepth, ci.bitdepth);
			}

			if (soundFileOut->Has(PAF::BitRate)) {
				if (soundFileIn->Has(PAF::BitRate)) soundFileOut->Set(PAF::BitRate, soundFileIn[PAF::BitRate]);
				if (ci.bitrate) soundFileOut->Set(PAF::BitRate, ci.bitrate);
			}

			if (soundFileIn->Has(PAF::SampleRate) && soundFileOut->Has(PAF::SampleRate)) {
				soundFileOut->Set(PAF::SampleRate, soundFileIn[PAF::SampleRate]);
				if (ci.samplerate) soundFileOut->Set(PAF::SampleRate, ci.samplerate);
			}

			numIns = (int)soundFileIn[PAF::NumChannels];
			src << "Eval(" << ci.expr << " External-Stream(\"soundfile\"";
			for (int i(0);i < numIns;++i) {
				src << " 0";
			}
			src << ") )";
		} else {
			src << "Eval(" << ci.expr << " nil)";
			if (ci.bitdepth == 0) ci.bitdepth = 16;
			if (ci.bitrate == 0) ci.bitrate = 192000;
			if (ci.samplerate == 0) ci.samplerate = 44100;

			if (soundFileOut->Has(PAF::BitDepth))	soundFileOut->Set(PAF::BitDepth, ci.bitdepth);
			if (soundFileOut->Has(PAF::BitRate))	soundFileOut->Set(PAF::BitRate, ci.bitrate);
			if (soundFileOut->Has(PAF::SampleRate)) soundFileOut->Set(PAF::SampleRate, ci.samplerate);
		}

		auto soundFileProcTy = cx.Make("llvm", src.str().c_str(), Kronos::GetNil( ), &log, 0, Kronos::Default, "host", "host", "host");
		int numOuts(0);
		GetRequestedIOConfig(soundFileProcTy, numIns, numOuts);

		std::vector<float> scratchSpace(numIns);
		if (soundFileProcTy.HasVar(Kronos::GetString("soundfile"))) 
			soundFileProcTy.SetVar(Kronos::GetString("soundfile"), scratchSpace.data( ));

		if (numOuts == 0) {
			throw std::runtime_error("Type of processor output, " + soundFileProcTy.TypeOfResult( ).AsString( ) + ", is invalid");
		}

		soundFileOut->Set(PAF::NumChannels, numOuts);

		size_t numFrames = soundFileIn ? soundFileIn[PAF::NumSampleFrames] : 0;
		numFrames += ci.tail;

		float sampleRate(0.f); 
		auto rateKey = GetPair(GetString("audio"), GetString("Rate"));
		if (soundFileProcTy.HasVar(rateKey)) {
			sampleRate = (float)soundFileOut[PAF::SampleRate];
			soundFileProcTy.SetVar(rateKey, &sampleRate);
		}

		auto soundFileProc = soundFileProcTy.Cons( );
		auto soundFileSig = soundFileProc.GetVar(GetString("audio"));
		auto soundFileTrig = soundFileProc.GetTrigger(GetString("audio"));
		std::vector<float> workspace;
		int tailLeft(ci.tail);

		soundFileOut->Stream([&](float *buffer, int samples) {
			if (soundFileIn) {
				workspace.resize(samples * soundFileIn[PAF::NumChannels] / numOuts);
				int didRead = soundFileIn(workspace.data( ), (int)workspace.size( ));
				for (int i(didRead); i < workspace.size( ); ++i) workspace[i] = 0;
				if (!didRead) soundFileIn.Close( );
			}

			if (!soundFileIn) {
				samples = std::min(tailLeft, samples);
				tailLeft -= samples;
			}

			if (samples) {
				if (soundFileSig) soundFileSig.SetData( workspace.data( ) );
				if (soundFileTrig) soundFileTrig.Go(buffer, samples / numOuts);
			}

			return samples;
		});

		soundFileOut.Close( );



	} catch (Kronos::IProgramError& pe) {
		std::cerr << "* Program Error E" << pe.GetErrorCode( ) << ": " << pe.GetSourceFilePosition( ) << "; " << pe.GetErrorMessage( ) << " *\n";
		if (cx) FormatErrors(log.str( ), cerr, cx);
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