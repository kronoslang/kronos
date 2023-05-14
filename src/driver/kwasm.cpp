#include "kronos.h"
#include "picojson.h"
#include "config/corelib.h"
#include "config/system.h"

#include <sstream>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <emscripten/emscripten.h>

using namespace Kronos;

void jsStackTrace() {
	EM_ASM({
		console.log(stackTrace());
	});
}

// single-threaded
class WasmCompiler {
	emscripten::val fetchFn;
	emscripten::val specializationMonitor;
	std::unordered_set<std::string> resolvedPaths;
	Kronos::Context cx;

	static const char *PathResolver(const char* mod, const char *file, const char *ver, void *user) {
		assert(mod && file && ver && user);
		auto self = (WasmCompiler*)user;		
		return self->Resolve(mod, file, ver);
	}

	const char *Resolve(const std::string& pack, const std::string& file, const std::string& version) {
		std::string absPath;
		if (pack.empty() || pack == "#core" || (pack == KRONOS_CORE_LIBRARY_REPOSITORY && version == KRONOS_CORE_LIBRARY_VERSION)) {
			absPath = "/library/";
			absPath.append(file);
		} else {
			if (fetchFn.as<bool>()) {
				auto resp = fetchFn(pack, version, file);
				if (resp.as<bool>()) {
					absPath = resp.as<std::string>();
				}
			} else {
				absPath = "/cache/" + pack + "/" + version + "/";
				absPath.append(file);
			}
		}
		auto retain = resolvedPaths.emplace(absPath).first;
		std::cout << "[" << pack << " " << version << " " << file << "] -> " << *retain << std::endl;
		return retain->data();
	}

	static const char* TypeString(std::ostream& o, const char *fmt) {
		while (*fmt) {
			auto c = *fmt++;
			if (c == '%') {
				c = *fmt++;
				switch (c) {
				case 'f': o << "\"Float\""; break;
				case 'd': o << "\"Double\""; break;
				case 'i': o << "\"Int32\""; break;
				case 'q': o << "\"Int64\""; break;
				case '%': o << "%"; break;
				case ']': return fmt;
				case '[':
					{
						char *loop_start;
						auto loop_count = strtoull(fmt, &loop_start, 10);
						while (loop_count--) {
							fmt = TypeString(o, loop_start + 1);
						}
						break;
					}
				default:
					assert(0 && "Bad format string");
				}
			} else o << c;
		}
		return fmt;
	}


	struct SoundFile {
		std::vector<float> data;
		Type type = GetNil();
	};

	std::unordered_map<std::string, SoundFile> assets;

	void* WasmAssetLinker(const char* uri, const IType** type) {
		auto sf = assets.find(uri);
		if (sf != assets.end()) {
			*type = sf->second.type.Get();
			(*type)->Retain();
			return (void*)sf->second.data.data();
		}
		auto ty{ GetNil() };
		*type = ty.Get();
		(*type)->Retain();
		return nullptr;
	}

	static void* WasmAssetLinker(const char* uri, const IType** type, void* user) {
		return ((WasmCompiler*)user)->WasmAssetLinker(uri, type);
	}

public:
	std::intptr_t LinkAudioAsset(const std::string& uri, double sampleRate, int numChannels, int numFrames, emscripten::val data) {
		SoundFile sf;
		std::clog << "Linking " << uri << " " << numChannels << " x " << numFrames << " @ " << sampleRate << "\n";

		sf.type =
			GetUserType(GetBuiltinTag(TypeTag::AudioFile), 
				GetPair(
				GetConstant(sampleRate),
				GetList(
					GetTuple(GetFloat32Ty(), numChannels),
					numFrames)));

		sf.data = emscripten::vecFromJSArray<float>(data);
		if (sf.data.size() == numChannels * numFrames) {
			auto ai = assets.emplace(uri, std::move(sf)).first;
			return (std::intptr_t)ai->second.data.data();
		} else {
			return 0;
		}
	}

	void SetSpecializationMonitor(emscripten::val sm) {
		specializationMonitor = sm;
	}
	 
	void SetFetchFn(emscripten::val fn) {
		fetchFn = fn;
	}

	void LogErr() {
		Kronos::CatchLastError([&](auto exTy, auto exCode, auto exMsg, auto exPos) {
			std::cerr << "* Error " << exCode << "*\n" << exMsg;
		});
	}

	WasmCompiler()
		:specializationMonitor(emscripten::val::undefined()),
		fetchFn(emscripten::val::undefined()) 
	{ }

	std::vector<char> moduleBuffer;
	std::string resolutionTrace;
	std::string symbolChanges;

	picojson::object errorMessage;

	void Initialize() {
		std::clog << 
		"Kronos   [" KRONOS_PACKAGE_VERSION "]\n"
		"Library  [" KRONOS_CORE_LIBRARY_REPOSITORY " " KRONOS_CORE_LIBRARY_VERSION "]\n"
		"Binaryen [" BINARYEN_VERSION "]\n";
		cx = CreateContext(PathResolver, this);
#ifndef NDEBUG
		std::clog << "[ DEBUG MODE ]\n";
#endif

		cx.ImportFile(Resolve(KRONOS_CORE_LIBRARY_REPOSITORY, "binaryen.k", KRONOS_CORE_LIBRARY_VERSION));

		cx.ImportFile(Resolve(KRONOS_CORE_LIBRARY_REPOSITORY, "VM.k", KRONOS_CORE_LIBRARY_VERSION)); LogErr();
		cx.SetAssetLinker(WasmAssetLinker, this);
		LogErr();
		cx.RegisterSpecializationCallback("rpc-monitor", [](void* user, int diags, const IType* ty, int64_t tyUid) {
			if (!diags) {
				auto self = (WasmCompiler*)user;
				if (self->specializationMonitor.as<bool>()) {
					std::stringstream sz;
					sz << "{\"jsonrpc\":2.0,\"method\":\"rpc-size\",\"params\":[\"";
					StreamBuf sbuf(sz.rdbuf());
					ty->_GetFirst()->ToStream(sbuf, nullptr, IType::WithoutParens);
					sz << "\"," << std::to_string(ty->SizeOf()) << "]}";
					self->specializationMonitor(sz.str());
				}
				return;
			}

			std::stringstream tyStr;
			StreamBuf sbuf(tyStr.rdbuf());
			ty->ToStream(sbuf, nullptr, IType::JSON);

			std::stringstream monitor;
			monitor << "{\"jsonrpc\":2.0,\"method\":\"rpc-monitor\",\"params\":{\"success\":"
				<< (tyUid ? "true" : "false") << ","
				<< "\"result\":";

			TypeString(monitor, tyStr.str().c_str());
			monitor << "}}";

			auto self = (WasmCompiler*)user;
			if (self->specializationMonitor.as<bool>()) self->specializationMonitor(monitor.str());
										  }, this);
		LogErr();
	}

	emscripten::val CompileFromSource(const std::string& src, bool instance, bool sidemodule) {
		GenericGraph g;
		moduleBuffer.clear();

		auto catchBlock = [&](auto exTy, auto exCode, auto exMsg, auto exPos) {
			std::cerr << "Error" << std::endl;
			std::stringstream log;
			if (g.Get()) {
				std::cerr << "Generating error log..." << std::endl << src << "\n";
				cx.Specialize(g, GetNil(), &log, 0);
				if (auto err = _GetAndResetThreadError()) err->Delete();
			}

			errorMessage["error"] = (double)exCode;
			errorMessage["message"] = exMsg;
			errorMessage["log"] = log.str();
		};

		errorMessage.clear();
		std::ostringstream output;
		g = cx.Parse(src.c_str(), true);
		if (Kronos::CatchLastError(catchBlock)) {
			goto try_finally;
		} else {
			auto tg = cx.Specialize(g, GetNil(), nullptr, 0);
			if (Kronos::CatchLastError(catchBlock)) goto try_finally;

			cx.Make("", ".wasm", output, "binaryen", tg, (BuildFlags)(
				(instance ? BuildFlags::OmitEvaluate : BuildFlags::OmitReactiveDrivers) |
				(sidemodule ? (BuildFlags)0 : BuildFlags::WasmStandaloneModule)));

			if (Kronos::CatchLastError(catchBlock)) goto try_finally;

			auto o = output.str();
			moduleBuffer = { o.data(), o.data() + o.size() };
		}
	try_finally:
		symbolChanges = MoveString(cx.DrainRecentSymbolChanges());
		resolutionTrace = MoveString(cx.GetResolutionTrace());
		std::clog << "Wasm blob is " << moduleBuffer.size() << " bytes\n";
		return emscripten::val{ emscripten::typed_memory_view(moduleBuffer.size(), moduleBuffer.data()) };
	}

	std::string GetLastError() {
		return picojson::value{ errorMessage }.serialize();
	}

	std::string Parse(const std::string& src, bool invalidate) {
		picojson::object parseObject;
		picojson::array parseResults;

		cx.Parse(src.c_str(), true, [&parseResults, this](const char* sym, GenericGraph gg) {
			parseResults.emplace_back(std::to_string(this->cx.UIDFromType(gg.AsType())));
					});

		CatchLastError([&parseObject](auto exTy, auto exCode, auto exMsg, auto exPos) mutable {
				parseObject["error"] = exMsg;
			}, [&parseObject, &parseResults]() {
				parseObject["results"] = parseResults;
			});

		return picojson::value{ parseObject }.serialize();
	}

	std::string GetDependencies() {
		return resolutionTrace;
	}

	std::string GetSymbolChanges() {
		return symbolChanges;
	}

	void Die() {
		abort();
	}

	std::string GetLibraryMetadata() {
		std::stringstream json;
		cx.GetLibraryMetadataAsJSON(json);
		return json.str();
	}

	std::string GetModuleCache() {
		using namespace std::string_literals;
		return "{ \""s + KRONOS_CORE_LIBRARY_REPOSITORY + "\":"
			      "[\""s + KRONOS_CORE_LIBRARY_VERSION + "\"]}";
	}

	std::string GetModuleCacheFile(const std::string& pack, const std::string& ver, const std::string& file) {
		std::stringstream slurp;
		auto path = Resolve(pack, file, ver);
		if (path) {
			std::ifstream read{ path };
			if (read.is_open()) slurp << read.rdbuf();
		}
		return slurp.str();
	}

	int WriteFile(const std::string& path, const std::string& body) {
		std::ofstream write{ path };
		write << body;
		return write.is_open();
	}
};

using namespace emscripten;
EMSCRIPTEN_BINDINGS(KronosWASM) {
	register_vector<char>("ByteVector");
	class_<WasmCompiler>("KWASM")
		.constructor<>()
		.function("Initialize", &WasmCompiler::Initialize)
		.function("Parse", &WasmCompiler::Parse)
		.function("WriteFile", &WasmCompiler::WriteFile)
		.function("SetSpecializationMonitor", &WasmCompiler::SetSpecializationMonitor)
		.function("SetFetchFn", &WasmCompiler::SetFetchFn)
		.function("GetSymbolChanges", &WasmCompiler::GetSymbolChanges)
		.function("GetModuleCache", &WasmCompiler::GetModuleCache)
		.function("GetModuleCacheFile", &WasmCompiler::GetModuleCacheFile)
		.function("GetLibraryMetadata", &WasmCompiler::GetLibraryMetadata)
		.function("GetDependencies", &WasmCompiler::GetDependencies)
		.function("GetLastError", &WasmCompiler::GetLastError)
		.function("LinkAudioAsset", &WasmCompiler::LinkAudioAsset, allow_raw_pointers())
		.function("Die", &WasmCompiler::Die)
		.function("CompileFromSource", &WasmCompiler::CompileFromSource);
}