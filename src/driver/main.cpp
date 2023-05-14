#include <Windows.h>
#include <string>
#include <mutex>
#include <thread>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <sstream>
#include <regex>
#include <memory>

#include <conio.h>

#include "PAD.h"
#include "kronos.h"

#include "StackExtender.h"

#undef max
#undef min

using namespace std;

namespace KronosPlugin {
	using namespace Kronos;
	class CompilerDriver {
		string source;
		Shared<Class> builtClass; 
		recursive_mutex driverMutex;
		Kronos::Context& context;
		float sampleRate;

	public:
#ifdef WIN32
		SHELLEXECUTEINFO execInfo;
		static vector<TCHAR> NewTempPath() {
			vector<TCHAR> tmp_path(MAX_PATH);
			vector<TCHAR> tmp_file(MAX_PATH);
			if (GetTempPathW(MAX_PATH,tmp_path.data()) >= 0) {
				UUID newId;
				UuidCreate(&newId);
				WCHAR* uuidstr;
				UuidToStringW(&newId,(RPC_WSTR*)&uuidstr);
				wsprintf(tmp_file.data(),TEXT("%s/kvst_%s.k"),tmp_path.data(),uuidstr);
				RpcStringFreeW((RPC_WSTR*)&uuidstr);
				return tmp_file;
			}
			else throw runtime_error("Failed to get a path for a temporary file");			
		}

		static void WriteFile(const vector<TCHAR>& path, string expr) {
			HANDLE file = CreateFile(path.data(),GENERIC_WRITE,NULL,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
			if (file) {
				DWORD written;
				::WriteFile(file,expr.c_str(),expr.size(),&written,NULL);
				CloseHandle(file);
			}
		}

		static string ReadSourceFile(const vector<TCHAR>& path) {
			HANDLE file = CreateFile(path.data(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
			if (file) {
				try {
					static const DWORD readSize = 4096;
					DWORD didRead = readSize;
					vector<char> buffer;
					while(didRead >= readSize) {
						buffer.resize(buffer.size() + readSize);
						ReadFile(file,buffer.data()+buffer.size()-readSize,readSize,&didRead,NULL);
						if (didRead < readSize) {
							buffer.resize(buffer.size() + didRead - readSize);
						}
					}
					CloseHandle(file);
					return string(buffer.data(),buffer.data()+buffer.size());
				} catch(...) {
					CloseHandle(file);
					throw;
				}
			}
			else throw runtime_error("Couldn't read source from temporary file");
		}

		void LaunchEditor() {
			if (execInfo.hProcess) {
				cerr << "Editor already running\n";
				return;
			}

			auto path = NewTempPath();
			WriteFile(path, source);

			memset(&execInfo,0,sizeof(SHELLEXECUTEINFO));
			execInfo.cbSize = sizeof(SHELLEXECUTEINFO);

			execInfo.hwnd = NULL;
			execInfo.lpVerb = TEXT("open");
			execInfo.lpFile = path.data();
			execInfo.nShow = SW_SHOW;
			execInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
			if (ShellExecuteEx(&execInfo)) {
				if (execInfo.hProcess) {
					thread processMonitor([this,path](){
						vector<TCHAR> tmppath(MAX_PATH);
						GetTempPath(MAX_PATH,tmppath.data());

						HANDLE waitHandle[2] = {
							execInfo.hProcess,
							FindFirstChangeNotification(tmppath.data(),false,FILE_NOTIFY_CHANGE_LAST_WRITE)
						};

						WIN32_FILE_ATTRIBUTE_DATA fileData;
						if (GetFileAttributesEx(path.data(),GetFileExInfoStandard,&fileData) == false) {
							throw runtime_error("Couldn't read temporary file attributes");
						}

						FILETIME lastWrite = fileData.ftLastWriteTime;
						HRESULT hr;
						while((hr = ::WaitForMultipleObjects(2,waitHandle,false,5000)) != WAIT_OBJECT_0) {							
							if (GetFileAttributesEx(path.data(),GetFileExInfoStandard,&fileData)) {
								if (memcmp(&lastWrite,&fileData.ftLastWriteTime,sizeof(FILETIME))) {
									lastWrite = fileData.ftLastWriteTime;
									try {
										source = ReadSourceFile(path);
										CompileLibrary(source);
									} catch(Kronos::ProgramError& se) {
										cerr << "** Program Error E" << se.GetErrorCode() << ": '"<<se.GetErrorMessage()<<"'\n";
									} catch(Kronos::Error& e) {
										cerr << "** General Error '" << e.GetErrorMessage() << "'\n";
									} catch(exception& re) {
										cerr << "Unknown Error: " << re.what() << "\n";
									} catch(...) {
										cerr << "FFFFUUUUUUUUUUUUU\n";
									}

								}
								FindNextChangeNotification(waitHandle[1]);
							}
						}

						cerr << "Editor was closed\n";
						CloseHandle(waitHandle[0]);
						FindCloseChangeNotification(waitHandle[1]);
						DeleteFile(path.data());
						execInfo.hProcess = 0;
					});
					processMonitor.detach();
				}
			}
		}

		void Reset() {
			source = 
			"/* Enter your code here */ \n\n";
			"Main() { \n"
			"	(l r) = IO:Audio-In() "
			"	level = IO:Parameter(\"level\" #0 #0.25 #1) "
			"	Main = (l * level r * level) "
			"}\n";

			execInfo.hProcess = 0;
		}

		bool IsEditing() {
			return execInfo.hProcess != NULL;
		}
#endif

		CompilerDriver(Kronos::Context& c):builtClass(0),context(c) {
			Reset();
		}

		void Compile(const string& expression) {
			lock_guard<recursive_mutex> guard(GetMutex());
		//	this->source = expression;
			stringstream log;
			try {
				builtClass = context.Make("llvm",expression.c_str(),Kronos::Type::GetNil(),&log,0);
			} catch(...) {
				cerr << log.str() << endl;;
				throw;
			}
		}

		void CompileLibrary(const string& userLibrary) {

			auto startTime = chrono::high_resolution_clock::now();

			lock_guard<recursive_mutex> guard(GetMutex());

			context.ImportBuffer("#vst", userLibrary,"vst:");

			auto duration = chrono::high_resolution_clock::now() - startTime;
			cout << "Compiled in " << chrono::duration_cast<chrono::milliseconds>(duration).count() << "ms\n";
		}


		recursive_mutex& GetMutex() { return driverMutex; }
		Class* GetClass() const { return builtClass; }
		Shared<Instance> NewInstance() {
			return builtClass ? builtClass->Cons() : Shared<Instance>(nullptr);
		}
	};
}

bool OutputCType(std::ostream& stream, Kronos::Type t, int memberIndex = 1) {
	if (t.SizeOf() < 1) stream << "void";
	else if (t == t.GetFloat32()) stream << "float";
	else if (t == t.GetFloat64()) stream << "double";
	else if (t == t.GetInt32()) stream << "int32_t";
	else if (t == t.GetInt64()) stream << "int64_t";
	else if (t.IsPair()) {
		stream << "struct { \n";
		
		while(t.IsPair()) {
			auto fst = t.GetFirst();
			int count = 1;

			Kronos::Type rst = t.GetRest();
			while(rst.IsPair() && rst.GetFirst() == fst) {
				count++;
				rst = rst.GetRest();
			}

			if (rst == t.GetFirst()) count++;

			if (fst.SizeOf()) {
				OutputCType(stream,fst);
				stream << " V" << memberIndex++;
				if (count > 1) stream << "["<<count<<"]";
				stream << ";\n";
			}

			if (rst == t.GetFirst()) {
				stream << "}";
				return true;
			} else t = rst;
		}

		if (t.SizeOf()) {
			OutputCType(stream,t,1);
			stream << " V" << memberIndex << ";\n";
		}
		stream << "}";
		return true;

	} else stream << "/*unknown*/";
	return false;
};

std::string CSanitize(std::string symbol) {
	for(auto &c : symbol) {
		if (!isalnum(c)) c = '_';
	}

	for(int i(1);i<symbol.size();++i) if (symbol[i-1] == '_') symbol[i] = toupper(symbol[i]);

	regex removeUnderScores("([a-zA-Z])_+([a-zA-Z])");

	symbol = regex_replace(symbol,removeUnderScores,"$1$2");
	symbol = regex_replace(symbol,regex("_+"),"_");

	if (!isalpha(symbol[0])) return "_"+symbol;
	else return symbol;
}

class CppSymbolDict : public set<string> {
	std::string prefix;

	void MakeSym(ostream& str) {}

	template <typename ARG, typename... ARGS> void MakeSym(ostream& str, ARG a, ARGS... as) {
		str << a;
		MakeSym(str,as...);
	}

public:
	CppSymbolDict(const std::string& pf):prefix(pf) {}

	template <typename... ARGS> std::string operator()(ARGS... as) {
		stringstream tmp;
		MakeSym(tmp,as...);
		std::string s = CSanitize(prefix + tmp.str());
		if (find(s) == end()) {
			insert(s);
			return s;
		} else {
			int idx(2);
			while(true) {
				stringstream tmp;
				tmp << s << idx++;
				if (find(tmp.str()) == end()) {
					insert(tmp.str());
					return tmp.str();
				}
			}
		}
	}
};

struct CppTypeDefs : public unordered_map<Kronos::Type, string> {
	ostream& writeDefs;
	CppSymbolDict& dict;
	int numCompositeTypes;
	CppTypeDefs(ostream& writeDefsToStream, CppSymbolDict& dict):writeDefs(writeDefsToStream),dict(dict),numCompositeTypes(0) {}
	string operator()(Kronos::Type& t) {
		auto f(find(t));
		if (f==end()) {
			string typeName;
			stringstream def;

			if (OutputCType(def,t)) {
				typeName = dict("Ty",++numCompositeTypes);
				writeDefs << "\ntypedef " << def.str() << " " << typeName << ";\n\n";				
			} else {
				typeName = def.str();
			}

			f = insert(make_pair(t,typeName)).first;
		}
		return f->second;
	}
};


void MakeCppHeader(const char *prefix, Shared<Kronos::Class> cls, Kronos::Type arg, std::ostream& stream) {

	string prefixUpper(prefix);
	for(auto &c: prefixUpper) c = toupper(c);

	CppSymbolDict syms(prefix);
	CppTypeDefs typeDefs(stream,syms);

	stream << "#ifndef __" << prefixUpper << "_H\n#define __"<<prefixUpper<<"_H\n\n";
	stream << "#ifdef __cplusplus \n#include <cstdint>\n#include <cstring>\n#include <cstdlib>\n"
		"extern \"C\" { \n#else\n#include <stdint.h>\n#include <string.h>\n#include <stdlib.h>\n#endif\n\n";

	stream << "#pragma pack(push)\n#pragma pack(1)\n";

	auto instTy = syms("Instance");
	auto argTy = syms("ArgumentTy");
	auto resTy = syms("ResultTy");

	stream << "typedef void* "<<instTy<<";\n";
	stream << "typedef "; OutputCType(stream, arg); stream << " " << argTy << ";\n";
	stream << "typedef "; OutputCType(stream, cls->TypeOfResult()); stream << " " << resTy << ";\n\n";

	string GetSize = syms("GetSize");
	string GetSymOfs = syms("GetSymbolTableOffset");
	string GetSyms = syms("GetSymbolTable");
	string Init = syms("Initialize");
	string Eval = syms("Evaluate");

	stream << "int64_t "<<GetSize<<"();\n";
	stream << "int64_t "<<GetSymOfs<<"();\n\n";
	stream << "void " << Init << "(" << instTy << ");\n";
	stream << "void " << Eval << "(" << instTy << ", const "<<argTy<<"* arg, " << resTy << "* result);\n\n";
	stream << "static void const** " << GetSyms << "("<<instTy<< " instance) { return (void const**)((char *)instance + " << GetSymOfs << "()); } \n\n";

	stringstream updaters;

	stringstream inputTable;
	stringstream inputEnum;

	bool First = true;
	int idx = 1;

	idx = 1;
	unordered_map<Kronos::Type,string> setters;
	stream << "\n/* Property setters */\n";
	for(auto& input : cls->GetListOfVars()) {
		string setter = syms("Set_",input.AsString());
		setters[input] = setter;

		string typeName = typeDefs(cls->TypeOfVar(input));

		stream << "static void " << setter << "("<<instTy<<" self, const " << typeName <<
				  "* value) { " << GetSyms << "(self)["<<cls->GetVarIndex(input)<<"] = (const void*)value; }\n";
	}

	stream << "\n/* Update triggers */\n";
	for(auto trigger : cls->GetListOfTriggers()) {
		string trigName = cls->GetTriggerName(trigger);

		stream << "void " << prefix << cls->GetTriggerName(trigger) <<"("<<instTy<<", "<<resTy<<"* output, int32_t numFrames);\n";

		string trigSym = syms(trigName.substr(4));
		inputEnum << "\t" << trigSym << ",\n";


		if (cls->HasVar(trigger)) {
			/* dual input / trigger key */
			string setter = setters[trigger];
			string typeName = typeDefs(cls->TypeOfVar(trigger));

			updaters << "\tvoid Update"<<cls->GetTriggerName(trigger).substr(4)<<"(const " << typeName;
			updaters << "* inputVector, ResTy* outputVector, int32_t vectorLength = 1) { \n\t\t" << setter << "(self, inputVector);\n\t\t" << prefix << cls->GetTriggerName(trigger) << "(self, outputVector, vectorLength);\n\t}\n\n";

			inputTable << "\t{\""<<trigName.substr(4)<<"\", "<<prefix<<trigName<<", "<<cls->TypeOfVar(trigger).SizeOf()<<", "<<cls->GetVarIndex(trigger)<<"}, /* input: " << cls->TypeOfVar(trigger).AsString() << "*/\n";

		} else {
			updaters << "\tvoid Update" << cls->GetTriggerName(trigger).substr(4)<<"(ResTy* outputVector, int32_t vectorLength = 1) { "<<prefix <<cls->GetTriggerName(trigger) << "(self, outputVector, vectorLength); }\n\n";

			inputTable << "\t{\""<<trigName.substr(4)<<"\", "<<prefix<<trigName<<", 0, -1}, /* no input */\n";
		}
	}

	string symEnum = syms("Symbols");

	stream << "\n/* Symbol Table */\n";
	stream << "typedef enum {\n" << inputEnum.str() << "\t" << prefix << "NumSymbols\n} " << symEnum << ";\n\n";
	stream << "typedef void (*KronosUpdateFunction)("<<instTy<<", " << resTy <<" *out, int32_t frames);\n";
	stream << "\ntypedef struct {\nconst char *label;\nKronosUpdateFunction update;\nsize_t inputFrameSize;\nint32_t sym;\n} KronosSymbolDescriptor;\n";

	stream << "\nstatic KronosSymbolDescriptor " << prefix << "SymbolTable[] = {\n" << inputTable.str() <<
		"\t{NULL, NULL, 0, 0}\n};\n\n";

	string Update = syms("Update");
	stream << "static void " << Update << "("<<instTy<<" self, "<<symEnum<<" symbol, const void* input, "<<resTy<<"* output, int32_t frames) {\n\t KronosSymbolDescriptor *sym = &"<<prefix<<"SymbolTable[symbol];\n";
	stream << "\tif (sym->inputFrameSize) "<<GetSyms<<"(self)[sym->sym] = input; \n"
		"\tif (sym->update) sym->update(self, output, frames); \n}\n\n";

	vector<tuple<Kronos::Type,Kronos::Type,unsigned,string>> UndefinedInputs;
	for(auto &id : cls->GetUndefinedVars()) {
		UndefinedInputs.push_back(
			make_tuple(id, cls->TypeOfVar(id), cls->GetVarIndex(id), typeDefs(cls->TypeOfVar(id))));
	}

	string Cons = syms("Cons");
	stringstream ctorParams, ctorRelay;

	
	for(auto& ui : UndefinedInputs) {
		if (First) First = false;
		else {ctorParams << ", "; ctorRelay << ", "; }
		stringstream name; name << CSanitize(std::get<0>(ui).AsString()) << idx++;
		ctorParams << "const " << std::get<3>(ui) << " *" << name.str();
		ctorRelay << name.str();
	}

	stream << "\n/* Convenience Constructor */\n";
	stream << "static "<<instTy<<" "<<Cons<<"(" << ctorParams.str() << ") {\n"
	"	void *mem = calloc(1,"<<prefix<<"GetSize());\n";
	if (UndefinedInputs.size()) { stream << 
	"	void const **syms = " << GetSyms << "(mem);\n";
	idx = 1;
	for(auto& ui : UndefinedInputs) {
		stream <<
	"	syms["<<cls->GetVarIndex(std::get<0>(ui))<<"] = (const void*)" << CSanitize(std::get<0>(ui).AsString()) << idx++ << ";\n";
	}
	}
	stream << 
	"	"<<prefix<<"Initialize(mem);\n"
	"	return mem;\n"
	"}\n\n";

	stream << "#pragma pack(pop)\n";

	stream << "#ifdef __cplusplus\n}\n\n";
	stream << "/* Class wrapper */\n"
	"class " << prefix << " {\n"
	"	void *self;\n" 
	"	int64_t GetSize() { return " << GetSize << "(); } \n"

	"public:\n"
	"	typedef " << argTy << " ArgTy;\n"
	"	typedef " << resTy << " ResTy;\n"
	"	" << prefix << "("<<ctorParams.str()<<") {\n"
	"		self = " << Cons << "("<<ctorRelay.str()<<");\n"
	"	}\n\n"
	"	" << prefix << "(const "<<prefix<<"& copyFrom) {\n"
	"		self = malloc(GetSize()); *this = copyFrom; \n"
	"	}\n\n"
	"	~" << prefix << "() {\n"
	"		free(self);\n"
	"	}\n\n"
	"	"<<prefix<<"& operator=(const "<<prefix<<"& copyFrom) { \n"
	"		memcpy(self,copyFrom.self,GetSize());\n"
	"	}\n\n"
	"	void operator()(const ArgTy* arg, ResTy* result) {\n"
	"		"<<Eval<<"(self, arg, result); \n"
	"	}\n\n" << updaters.str() << "};\n\n";
	stream << "#endif\n";
	stream << "#endif\n";
}

void FormatErrors(const std::string& xml, std::ostream& out, Kronos::Context& cx);

int __main()
{
	using namespace PAD;
	using namespace Kronos;
	using namespace std;

	Context &myContext(Context::Create());

	myContext.ModuleImportPaths().clear();
	myContext.ModuleImportPaths().push_back(Context::GetUserPath() + "/K3/Lib");

	class Logger : public DeviceErrorDelegate{
	public:
		void Catch(SoftError se) { cerr << se.what() << "\n"; }
		void Catch(HardError he) { throw he; }
	};

	Logger log;
	Session audio(false,&log);
	audio.InitializeHostAPI("WASAPI",&log);

	for(auto& d : audio) {
		cout << d << "\n";
	}

	stringstream testLib;

	testLib << 
		"Import Test "
		"Package IO { "
		"Audio-Clock(sig) { "
		"	Audio-Clock = Reactive:Resample(sig Reactive:Tick(#10 \"audio\")) "
		"} "
		"Parameter(name minimum middle maximum) { "
		"	Parameter = Reactive:Resample(External(arg 0) Reactive:Tick(#0 arg)) "
		"} "
		"Audio-In() {"
		"	Audio-In = Audio-Clock(External(\"audio\" (0 0))) "
		"} "
		"Left/Right(stereo) { (Left Right) = stereo } "
		"} "
		"Oscillator(freq) { "
		"	next = z-1(0 wrap + IO:Audio-Clock(freq)) "
		"	wrap = next - Floor(next) "
		"	Oscillator = 2 * wrap - 1 "
		"} } ";

	myContext.ImportBuffer("#test", testLib.str(), "");

	auto device = audio.FindDevice("wasapi","idt");
	if (device != audio.end()) {
		double phase = 0;

		KronosPlugin::CompilerDriver driver(myContext);
		driver.Compile("IO:Audio-In()");
		Shared<Class> currentClass(0);
		Shared<Instance> currentInstance(0);
		Trigger audioClock, paramClock;
		Var audioInput, paramInput;

		unordered_map<Type,tuple<string,float,Var,Trigger>> dspParams;

		device->BufferSwitch = [&](uint64_t ts, const AudioStreamConfiguration& conf, const float *in, float *out, unsigned frames) {			
			Class *myClass = currentInstance ? currentInstance->TypeOf() : 0;
			if (myClass != driver.GetClass()) {
				lock_guard<recursive_mutex> scopedLock(driver.GetMutex());
				float sr = conf.GetSampleRate();

				auto srsig = Type::Tuple(Type::GetString("audio"),Type::GetString("Rate"));

				if (driver.GetClass()->HasVar(srsig)) {
					driver.GetClass()->SetVar(srsig,&sr);
				}

				currentInstance = driver.NewInstance();
				if (currentInstance != nullptr) {
					audioClock = currentInstance->GetTrigger(Type::GetString("audio"));
					audioInput = in ? audioInput = currentInstance->GetVar(Type::GetString("audio")) : Var();

					dspParams.clear();
					for(auto i : driver.GetClass()->GetListOfVars()) {
						dspParams[i] = make_tuple(i.AsString(),0.f,currentInstance->GetVar(i),Trigger());
					}

					for(auto t : driver.GetClass()->GetListOfTriggers()) {
						std::get<3>(dspParams[t]) = currentInstance->GetTrigger(t);
					}

					cout << "* Parameters *\n";
					for(auto p : dspParams) {
						cout << get<0>(p) << " : " << (get<2>(p.second)?"input":"no input") << " " << (get<3>(p.second)?"trigger":"no trigger") << "\n";
					}
				}
			}

			if (audioClock) {
				unsigned numIns = conf.GetNumStreamInputs();
				unsigned numOuts = conf.GetNumStreamOutputs();
				//for(unsigned i(0);i<frames;++i) {
				//	audioInput(in + i * numIns);
				//	audioClock(out + i * numOuts);
				//}
				audioInput(in);
				audioClock(out,frames);
			} else {
				memset(out,0,sizeof(float)*conf.GetNumStreamOutputs()*frames);
			}
		};

		device->Open(device->DefaultStereo());


//		driver.LaunchEditor();

		float param = 0.5;
		vector<uint8_t> tmpBuf;
		bool evalMode(true);
		bool staticMode(false);

		while(true) {
			if (evalMode) {
				string expr;
				getline(cin,expr,'\n');

				if (expr == "#") staticMode = !staticMode;
				if (expr == "!") {
					evalMode = false;
				} else {
					stringstream log;
					try {
						Shared<Class> cls = myContext.Make("llvm",expr.c_str(),Type::GetNil(),&log,0,(Kronos::BuildFlags)0);
						if (staticMode) {
							ofstream header("MyDsp.h");
							ofstream object("MyDsp.obj",ios_base::binary);
							header << "/* Kronos 3 beta - Generated code \n"
									  " * "<<expr<< "\n"
									  " */\n\n";
							MakeCppHeader("MyDsp",cls,Type::GetNil(),cout);
							MakeCppHeader("MyDsp",cls,Type::GetNil(),header);
							cls->MakeStatic("MyDsp",nullptr,".obj",object,(Kronos::BuildFlags)0);
						} else {
							auto srsig = Type::Tuple(Type::GetString("audio"),Type::GetString("Rate"));
						
							if (cls->HasVar(srsig)) {
								float sampleRate = 44100;
								cls->SetVar(srsig,&sampleRate);
							}

							Shared<Instance> inst = cls->Cons();
							vector<uint8_t> result(cls->TypeOfResult().SizeOf());
							(*inst)(nullptr,result.data());
							cout << "Type => " << cls->TypeOfResult().AsString() << "\n";
							cls->TypeOfResult().ToStream(cout,result.data());
							cout << endl;
						}
					} catch (Kronos::ProgramError& e) {
						cerr << e.GetSourceFilePosition() << ": E"<<e.GetErrorCode()<<": " << e.GetErrorMessage() << endl;
						FormatErrors(log.str(), cerr, myContext);
					} catch (Kronos::Error& e) {
						cerr << e.GetErrorMessage() << endl;
					}
				}
			} else {
				driver.LaunchEditor();
				string param; float val;
				cin >> param >> val;
				cin.sync();

				if (param == "!") {
					evalMode = true;
				} else {
					for(auto& p : dspParams) {
						if (get<0>(p.second).find(param,0) != string::npos) {
							tmpBuf.resize(driver.GetClass()->TypeOfResult().SizeOf());
							get<1>(p.second) = val;
							get<2>(p.second)(&val);
							get<3>(p.second)(tmpBuf.data());
							break;
						}
					}
				}
			}
		}

		device->Close();
	}

	myContext.Destroy();
	return 0;
}
