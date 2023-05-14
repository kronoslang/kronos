#include "common/DynamicScope.h"
#include "common/PlatformUtils.h"
#include "TLS.h"
#include "Parser.h"
#include "RepositoryBuilder.h"
#include "TypeAlgebra.h"
#include "Evaluate.h"
#include "Invariant.h"
#include "DynamicVariables.h"
#include "FlowControl.h"
#include "paf/PAF.h"

#include <sstream>
#include <fstream>
#include <memory>
#include <locale>
#include <regex>
#include <iostream>

#ifdef HAVE_LLVM
#pragma warning(disable: 4126 4267)
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/DynamicLibrary.h"
#endif

struct cleaner { 
	std::function<void()> clean;
	cleaner(std::function<void()> f):clean(std::move(f)) { }
	~cleaner() { clean(); }
};

namespace K3 {
	void BuildReactivePrimitiveOps(Parser::RepositoryBuilder);
	void BuildVectorPrimitiveOps(Parser::RepositoryBuilder);
	void BuildSelectPrimitiveOps(Parser::RepositoryBuilder);
	void BuildNativePrimitiveOps(Parser::RepositoryBuilder);

	void BuildKernelFunctions(Parser::RepositoryBuilder& build) {
		RegionAllocator KernelBaseAllocator{ build.GetMemoryRegion() };

		auto arg = Nodes::GenericArgument::New();
		auto b1 = Nodes::GenericFirst::New(arg);
		auto b2 = Nodes::GenericRest::New(arg);

		build.AddMacro("arg", arg, false);
		build.AddMacro("nil", Nodes::Invariant::Constant::New(false), false);

//		build.AddPackage("Fallback").AddFunction("Eval", Nodes::Invariant::GenericNoFallback::New());

		build.AddPackage("Fallback").AddFunction("Eval", Nodes::Raise::New(
			Nodes::GenericPair::New(
				b1,
				Nodes::GenericPair::New(
					Nodes::Invariant::Constant::New(" called with illegal argument "),
					b2))));

		build.AddFunction("Eval", Nodes::Evaluate::New("eval", b1, b2), "fn arg...", "Evaluates 'fn' as a function with the arguments in 'arg...'");

		build.AddFunction("External", Nodes::GenericExternalVariable::New(b1, b2), "key default", "External input declaration with the identifier 'key' and type and default value provided by 'default'");
		build.AddFunction("External-Stream", Nodes::GenericStreamInput::New(b1, Nodes::GenericFirst::New(b2), Nodes::GenericRest::New(b2)), "stream-key default clock", "Stream input to this module from an external vector. The sample rate of the buffer is determined by 'clock'.");
		build.AddFunction("External-Asset", Nodes::GenericAsset::New(arg), "uri", "Loads an asset from 'uri' and returns its contents.");
        build.AddMacro("Audio-File-Tag", Nodes::Invariant::Constant::New(Type(&AudioFileTag)), true);

//		build.AddFunction("Raise", Nodes::Raise::New(arg), "e", "Raises a user exception of type 'e'");
//		build.AddFunction("Handle", Nodes::Handle::New(b1, b2), "try catch", "If specializing 'try' raises an exception, pass it to 'catch' as an argument and use the resulting value.");
//		this must be a special form because a function will not be evaluated with undefined argument.

		BuildSelectPrimitiveOps(build);
		BuildNativePrimitiveOps(build);
		BuildInvariantPrimitiveOps(build);
		BuildReactivePrimitiveOps(build.AddPackage("Reactive"));
		BuildVectorPrimitiveOps(build.AddPackage("Vector"));
	}

	CMAKE_THREAD_LOCAL TLS* __instance = 0;
    
    Asset::Asset():memory(nullptr, free) {
    }

	void TLS::SetCurrentInstance(TLS* instance) {
		__instance = instance;
	}

	size_t TLS::GetUID() {
		return ++curUID;
	}

	TLS* TLS::GetCurrentInstance() {
		return __instance;
	}

#ifndef NDEBUG
	bool TLS::ShouldTrace(const char *context, const char *label) {
		if (compilerTraceFilter.empty()) return false;
		return std::regex_search(std::string(context) + ":" + label, std::regex{ compilerTraceFilter });
	}
#endif

	static string GetParentDirectory(std::string fullpath) {
		std::string::size_type separator;
#ifdef WIN32
        std::string::size_type sep2;
		if ((separator = fullpath.find_last_of("\\")) != std::string::npos) {
			if ((sep2 = fullpath.find_last_of("/",separator)) != std::string::npos) {
				separator = sep2;
			}
#else
		if ((separator = fullpath.find_last_of("/")) != std::string::npos) {
#endif
			return fullpath.substr(0, separator + 1);
		} else return "";
	}

	static inline std::string MoveString(Kronos::IStr* b) { std::string s(b->data(), b->data() + b->size()); b->Delete(); return s; }

	void TLS::InitializeDefaultResolver() {
		std::vector<std::string> paths;

		if (getenv("KRONOS_RUNTIME_LIBRARY")) paths.push_back(std::string(getenv("KRONOS_RUNTIME_LIBRARY")));

		if (getenv("XDG_DATA_HOME")) {
			paths.push_back(getenv("XDG_DATA_HOME") + "/library"s);
		}

		if (getenv("XDG_DATA_DIRS")) {
			std::string dataDirs = getenv("XDG_DATA_DIRS");
			for (;dataDirs.size();) {
				auto pos = dataDirs.find_first_of('.');
				auto dataDir = dataDirs.substr(0, pos);
				paths.push_back(dataDir + "/kronos/library");
				if (pos != dataDirs.npos) {
					dataDirs = dataDirs.substr(pos + 1);
				} else {
					break;
				}
			}
		}

		paths.push_back(MoveString(Kronos::_GetUserPath()) + "/library/");
		paths.push_back(MoveString(Kronos::_GetSharedPath()) + "/library/");
#ifdef WIN32
		paths.push_back(std::regex_replace(GetProcessFileName(), std::regex("/bin/[^/]*"), "/library/"));
#else
#ifdef __APPLE__
		paths.push_back(MoveString(Kronos::_GetUserPath()) + "/Library/Kronos/");
#endif
		paths.push_back(std::regex_replace(GetProcessFileName(), std::regex("/bin/[^/]*"), "/share/kronos/library/"));
#endif
		paths.push_back("./library/");

		modulePathResolver = [paths](const char* uri, const char* ver, const char* file) {
			static char buf[2048];
			auto probeFile = [](const char *path) {
				FILE *test = fopen(path, "rt");
				if (test) {
					fclose(test); return true;
				}
				return false;
			};
			buf[0] = 0;
			for (auto in : paths) {
				auto testPath = in + "/" + file;
				if (probeFile(testPath.c_str())) {
					strncpy(buf, testPath.c_str(), sizeof(buf));
					buf[2047] = 0;
				}
			}

			return buf;
		};
	}

	Err<void> TLS::Initialize() {	
		ScopedContext cx(*this);
		auto builder{ codebase.GetKernelBuilder() };
		BuildKernelFunctions(builder);
		codebase.UpdateDefinitions();

		// dump the kernel function definitions by uncommenting this
		// codebase.dump_kernel_functions(std::cout);

		// parse prelude
        return codebase.ImportCoreLib("Prelude.k");
	}

	TLS::TLS(Kronos::ModulePathResolver res, void *user) {
		if (getenv("KRONOS_COMPILER_TRACE")) {
			compilerTraceFilter = getenv("KRONOS_COMPILER_TRACE");
		}
		SetModuleResolver(res, user);
		if (!res) {
			InitializeDefaultResolver();
		}
		auto err = Initialize();
		if (err.err) {
			if (err.err->GetSourceFilePosition()) {
				std::string url, show;
				int line, column;
				codebase.GetPosition(err.err->GetSourceFilePosition(), url, line, column, &show);
				if (url.size()) {
					std::cerr << "While preloading " << url << ":(" << line << "," << column << ")\n";
					std::cerr << show << "\n";
				}
			}
			std::cerr << err.err->GetErrorMessage() << "\n";
			abort();
		}
	}

	Kronos::BuildFlags TLS::GetCurrentFlags() {
		return GetCurrentInstance()->flags;
	}

	TypeDescriptor* TLS::GetTypeDescriptor(const std::string& key) {
		auto f(usertypes.find(key));
		if (f==usertypes.end())
			f = usertypes.insert(make_pair(key,TypeDescriptor(key))).first;

		return &f->second;
	}

	const void* TLS::Memoize(const Type& key) {
		auto f(typeKeys.find(key));
		if (f!=typeKeys.end()) return &*f;
		const void *uid = &(*typeKeys.insert(key).first);
		assert(typeAssoc.count(uid) == 0);
		typeAssoc[uid] = key;
		return uid;
	}

	Type TLS::Recall(const void* uid) {
		return typeAssoc[uid];
	}

	Nodes::CGRef TLS::ResolveSymbol(const char* qualifiedName) {
		auto self = TLS::GetCurrentInstance();
		auto sym = self->codebase.Lookup(qualifiedName);
		self->DidResolve(qualifiedName);
		return sym ? sym->graph : nullptr;
	}

	void TLS::RebindSymbol(const char *qualifiedName, Nodes::CGRef graph) {
		auto self = TLS::GetCurrentInstance();
		self->codebase.Rebind(qualifiedName, graph);
	}

	std::unordered_set<std::string> TLS::DrainRecentChanges() {
		std::unordered_set<std::string> changes;
		std::swap(changes, codebase.changed_symbols);
		return changes;
	}

	std::string TLS::GetModuleAndLineNumberText(const char* sourcePos, std::string* showLine) {
		std::stringstream text;
		int line, column;
		std::string uri;
		TLS::GetCurrentInstance()->codebase.GetPosition(sourcePos, uri, line, column, showLine);
		if (uri.empty()) return uri;
		text << uri << "(" << line << ";" << column << ")";
		return text.str();
	}

	Type TLS::GetRepositoryMetadata(const std::string& prefix) {
		Type lib;
/*		for (auto &sym : GetCurrentInstance()->codebase.get_symboltable()) {
			if (sym.first.size() >= prefix.size()) {
				for (size_t i(0);i < prefix.size();++i) {
					if (sym.first[i] != prefix[i]) goto next;
				}
				auto s = GetCurrentInstance()->codebase.lookup(sym.first);
				if (s)
				for (auto& md : s->metadata) {
					lib = Type::Pair(
						Type::Tuple(Type(sym.first.substr(prefix.size()).c_str()), Type(md.first.c_str()), Type(md.second.c_str()))
						,lib);
				}
			}
        next:;
		};*/
		return lib;
	}

	Asset& TLS::GetAsset(const std::string& name) {
		if (staticAssets.count(name) == 0 && assetLoader) {
			Type ty;
			auto mem = assetLoader(name.c_str(), ty);
			if (mem) {
				staticAssets[name].memory = { mem, free };
				staticAssets[name].type = ty;
			#ifdef HAVE_LLVM
				llvm::sys::DynamicLibrary::AddSymbol(name.c_str(), mem);
			#endif
			}
		}
        return staticAssets[name];
	}
};
