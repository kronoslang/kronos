#include "Parser.h"
#include "Invariant.h"
#include "lithe/grammar/kronos.h"
#include "config/corelib.h"
#include "TLS.h"
#include "Evaluate.h"
#include "TypeAlgebra.h"
#include "Invariant.h"
#include "LibraryRef.h"
#include "common/PlatformUtils.h"

#include <fstream>
#include <sstream>
#include <regex>

#define Repository Repository2

namespace K3 {
	namespace Parser {
		static const char* DefinitionName[] = {
			"undefined",
			"variable",
			"function"
		};

		Form Form::Fn(CGRef expr) {
			Form f;
			f.body = expr;
			f.mode = Mode::Function;
			return f;
		}

		Form Form::Val(CGRef expr) {
			Form f;
			f.body = expr;
			f.mode = Mode::Macro;
			return f;
		}

		ParserError Repository::RedefinitionError(std::string qn, 
											 const char* newPoint, Form::Mode newMode, 
											 const char* oldPoint, Form::Mode oldMode) {
			using namespace std::string_literals;
			std::string URI, show;
			int line, column;

			GetPosition(oldPoint, URI, line, column, &show);

			std::string explanation = "";

			if (newMode != oldMode) {
				explanation = "It was previously defined as a "s + DefinitionName[oldMode] + ". ";
			} else if (newMode == Form::Function) {
				explanation = "Please use #[Extend] attribute to add polymorphic forms, or #[Override] to replace prior definition.";
			}

			return ParserError(newPoint, "Can not redefine '" + qn +
							   "'; " + explanation + " Previous definition at "
							   + URI + "(" + std::to_string(line) + ":"
							   + std::to_string(column) + ")\n" + show
			);
		}

		PartialDefinition RepositoryNode::Resolve(const std::string& qualifiedName, std::unordered_set<RepositoryNode*> &visited) {
			static const PartialDefinition empty;
			std::clog << "Resolve\n";
			if (visited.count(this)) return empty;
			std::clog << "Mark visit\n";
			visited.emplace(this);

			std::clog << "Generate pd\n";
			PartialDefinition pd;

			for (auto i : imports) {
				std::clog << "Append def\n";
				pd.Append(i->Resolve(qualifiedName, visited));
			}

			std::clog << "append local\n";
			auto c = changes.find(qualifiedName);
			if (c != changes.end()) {
				pd.Append(c->second);
			}

			return pd;
		}

		void RepositoryNode::Resolve(std::unordered_map<std::string, PartialDefinition>& definitions, std::unordered_set<RepositoryNode*>& visited) {
			if (visited.count(this)) return;
			visited.emplace(this);

			for (auto i : imports) {
				i->Resolve(definitions, visited);
			}

			if (definitions.size() < changes.size()) {
				for (auto& d : definitions) {
					auto c = changes.find(d.first);
					if (c != changes.end()) {
						d.second.Append(c->second);
					}
				}
			} else {
				for (auto& c : changes) {
					auto d = definitions.find(c.first);
					if (d != definitions.end()) {
						d->second.Append(c.second);
					}
				}
			}
		}

		static std::unordered_set<std::string> CacheKeysToSet(const std::unordered_map<std::string, PartialDefinition>& pd) {
			std::unordered_set<std::string> keys;
			for (auto& c : pd) {
				keys.emplace(c.first);
			}
			return keys;
		}

		void RepositoryNode::AddImport(RepositoryNode* i) {
			if (std::find(imports.begin(), imports.end(), i) != imports.end()) {
				return;
			}
			imports.emplace_back(i);
		}

		void RepositoryNode::Reset() {
			imports.clear();
			changes.clear();
		}

		void PartialDefinition::Append(const PartialDefinition& b) {
			for (int i = (int)b.forms.size() - 1; i >= 0; --i) {
				if (b.forms[i].HasAttr(Attributes::AlwaysOverride)) {
					recurData = b.recurData;
					forms.resize(b.forms.size() - i);
					for (int j = i; j < b.forms.size(); ++j) {
						forms[j - i] = b.forms[j];
					}
					return;
				}
			}

			for (auto& md : b.metadata) {
				auto& doc{ metadata[md.first] };
				if (doc.size()) doc += "\n\n";
				doc += md.second;
			}

			forms.insert(forms.end(), b.forms.begin(), b.forms.end());
			recurData.insert(recurData.end(), b.recurData.begin(), b.recurData.end());
		}

		PartialDefinition PartialDefinition::Value(CGRef v) {
			PartialDefinition pd;
			pd.forms.emplace_back(Form::Val(v));
			return pd;
		}

		Err<CGRef> PartialDefinition::Complete(Repository* r, const std::string& name) const {
			if (forms.empty()) {
				return (CGRef)nullptr;
			}

			Form::Mode mode = forms[0].mode;
			Type fn{ forms[0].body };
			Type rd{ false };

			// validate extension and override
			for (int i = 1; i < forms.size(); ++i) {

				if (forms[i].HasAttr(Attributes::AlwaysOverride)) {
					fn = Type{ false };
				} else if (mode != forms[i].mode || 
						   !forms[i].HasAttr(Attributes::Extend)) {
					if (forms[i].HasAttr(Attributes::MayOverride)) {
						fn = Type{ false };
					} else {
						if (r) {
							return r->RedefinitionError(
								name,
								forms[i].body->GetRepositoryAddress(),
								forms[i].mode,
								forms[i - 1].body->GetRepositoryAddress(),
								forms[i - 1].mode
							);
						} else {
							return ParserError(forms[i].body->GetRepositoryAddress(), 
											   "Redefinition of '" + name + "'");
						}
					}
				}
				mode = forms[i].mode;
				fn = Type::Pair(Type(forms[i].body), fn);

			}

			for (auto& r : recurData) {
				rd = Type::Pair(r, rd);
			}

			switch (mode) {
				case Form::Undefined:
					return nullptr;
				case Form::Macro:
					return forms.back().body;
				case Form::Function:
					assert(forms.size());
					break;
			}

			auto form = Type::User(
				&FunctionTag,
				Type::Tuple(
					Type(name.c_str()),
					rd, fn, Type{ false })
			);

			return Nodes::Invariant::Constant::New(form);
		}

		Err<symbol_t> PartialDefinition::CompleteMeta(Repository* r, const std::string& name) const {
			LET_ERR(graph, Complete(r, name));
			symbol_t sym;
			sym.graph = graph;
			sym.metadata = metadata;
			return sym;
		}


		Err<symbol_t> Repository::Build(const std::string& qualifiedName) {
			RegionAllocator r{ stubs };
			std::unordered_set<RepositoryNode*> vs;
			return 
				root.Resolve(qualifiedName, vs).CompleteMeta(this, qualifiedName);
		}

		const symbol_t* Repository::Lookup(const std::string& qualifiedName) {
			auto f = completeDefinitions.find(qualifiedName);
			if (f != completeDefinitions.end()) return &f->second;
			else return nullptr;
		}

		void Repository::InvalidateSymbolsInNode(RepositoryNode* n) { 
			for (auto s : n->changes) {
				changed_symbols.emplace(s.first);
			}
		}

		Err<void> Repository::UpdateDefinitions() {
			if (changed_symbols.size()) {
				std::unordered_map<std::string, PartialDefinition> total;
				std::unordered_set<RepositoryNode*> vs;

				for (auto& sym : changed_symbols) {
					total.emplace(sym, PartialDefinition{});
				}

				root.Resolve(total, vs);

				RegionAllocator r{ stubs };
				for (auto& d : total) {
					LET_ERR(complete, d.second.CompleteMeta(this, d.first));
					completeDefinitions[d.first] = complete;
				}
			}
			return { };
		}

		static std::string ExtractVersionNumber(std::string& semver) {
			auto dotPos = semver.find_first_of('.');
			if (dotPos != semver.npos) {
				semver = semver.substr(dotPos + 1);
			}
			return semver.substr(0, dotPos);
		}

		Err<void> Repository::CreateImportTask(RepositoryNode* parent, BufferKey uri, immediate_handler_t imm) {
			if (uri.package == "#LOCAL") {
				uri.package = relativeImportBase.package;
				uri.version = relativeImportBase.version;
				if (!std::regex_match(uri.path, std::regex("^([a-zA-Z+]:)?[/\\\\].*"))) {
					uri.path = relativeImportBase.path + uri.path;
				}
			}

			if (uri.package == "#CORE") {
				uri.package = defaultRepo;
				uri.version = defaultVersion;
			} 

			auto mod = modules.find(uri.package);
			if (mod != modules.end()) {
				auto importVersion = uri.version;
				auto loadedVersion = mod->second.version;

				std::string mostRecentVersion;

				if (importVersion != loadedVersion) {
					auto mi = ExtractVersionNumber(importVersion);
					auto ml = ExtractVersionNumber(loadedVersion);

					if (mi != ml) {
						return ParserError(nullptr, 
										   "Trying to import " + uri.version + " while " +
										   mod->second.version + " is already imported. The major "
										   "versions of these packages are incompatible.");
					}

					while(mostRecentVersion.empty()) {
						auto iDigit = ExtractVersionNumber(importVersion);
						auto mDigit = ExtractVersionNumber(loadedVersion);
						if (iDigit.empty()) {
							mostRecentVersion = mod->second.version; 
						} else if (mDigit.empty()) {
							mostRecentVersion = uri.version;
						} else if (strtoul(iDigit.c_str(),nullptr,10) > 
								   strtoul(mDigit.c_str(), nullptr, 10)) {
							mostRecentVersion = uri.version;
						} else if (strtoul(iDigit.c_str(), nullptr, 10) < 
								   strtoul(mDigit.c_str(), nullptr, 10)) {
							mostRecentVersion = mod->second.version;
						} else if (iDigit > mDigit) {
							mostRecentVersion = uri.version;
						} else if (iDigit < mDigit) {
							mostRecentVersion = mod->second.version;
						} 
					}

					if (mostRecentVersion != uri.version) {
						std::clog << "* Substituting version " << mostRecentVersion << " for " << uri.str() << " requested by the import.\n";						
						uri.version = mostRecentVersion;
					}

					if (mostRecentVersion != mod->second.version) {
						std::clog << "* Upgrading [" << mod->first << " " << mod->second.version << "] to version " << mostRecentVersion << "\n";
						for (auto& file : mod->second.files) {
							importQueue.emplace_back(
								ImportTask{
									{ mod->first, mostRecentVersion, file.first },
									file.second,
									{},
									relativeImportBase
								}
							);
						}
						mod->second.version = mostRecentVersion;
					}
				}

				auto loadedFile = mod->second.files.find(uri.path);
				if (loadedFile != mod->second.files.end()) {
					// already loaded
					parent->AddImport(loadedFile->second);
					InvalidateSymbolsInNode(loadedFile->second);
					return { };
				}
			} else {
				mod = modules.emplace(
					uri.package,
					RepositoryModule{ 
						{},
						uri.version
					}).first;
			}

			auto nk = NodeKey(uri.package, uri.path);
			if (nodes.find(nk) == nodes.end()) {
				nodes.emplace(nk, std::make_unique<RepositoryNode>());
			}

			mod->second.files[uri.path] = nodes[nk].get();
			parent->AddImport(nodes[nk].get());

			importQueue.emplace_back(
				ImportTask{
					std::move(uri),
					nodes[nk].get(),
					imm,
					relativeImportBase
				}
			);
			return { };
		}

		static lithe::rule KronosParser = lithe::grammar::kronos::parser();

		Err<void> Repository::ParseIntoNode(RepositoryNode* node, const std::string& tmpCode, immediate_handler_t imm) {
			// store state for rollback
			rollback.emplace(node, *node);

			InvalidateSymbolsInNode(node);
			node->Reset();
			node->retainSource = tmpCode;

			auto& code{ node->retainSource };

			const char* sb = code.data();
			const char* se = code.data() + code.size();
			auto parseTree = (*KronosParser)(sb, se);

			if (parseTree.is_error()) {
				std::stringstream msg;
				parseTree.to_stream(msg);
				return ParserError(parseTree.children.size() 
								   ? parseTree.children.front().strbeg 
								   : nullptr, 
								   msg.str());
			}

			RegionAllocator allocateFrom{ node->graphs };

			LET_ERR(syms, (GenerateSymbols(
				parseTree,
				node->incremental,
				[this, node](BufferKey imp) -> Err<void> {
					CHECK_ERR(CreateImportTask(node, imp, { }));
					return { };
				},
				[imm](const char* sym, CGRef expr) -> Err<void> { 
					if (imm) imm(sym, expr);
					return { }; 
				})));

			for (auto& def : syms) {
				changed_symbols.emplace(def.first);
				node->changes[def.first].Append(def.second);

				if (node->canOverwrite) {
					for (auto& f : node->changes[def.first].forms) {
						f.attr = (Attributes)((int)Attributes::MayOverride | (int)f.attr);
					}
				}
			}
			return { };
		}

		struct RestoreBase {
			BufferKey o;
			BufferKey& n;
			RestoreBase(BufferKey& s) :n(s) {
				o = n;
			}
			~RestoreBase() {
				n = o;
			}
		};

		Err<void> Repository::Perform(ImportTask it) {
			RestoreBase folderScope{ relativeImportBase };
			relativeImportBase = it.uri;
			
			auto relativePathEnd = relativeImportBase.path.find_last_of("/\\");
			if (relativePathEnd != relativeImportBase.path.npos) {
				relativeImportBase.path.erase(relativePathEnd + 1);
			} else {
				relativeImportBase.path = "";
			}

			std::string pathToFile;

			if (it.uri.package == "#LOCAL") {
				pathToFile = it.uri.path;
			} else {
				pathToFile = TLS::GetCurrentInstance()->ResolveModulePath(
					it.uri.package.c_str(),
					it.uri.path.c_str(),
					it.uri.version.c_str());
			}

			std::ifstream readFile{ pathToFile };
			if (readFile.is_open()) {
				it.node->fileSystemPath = GetCanonicalAbsolutePath(pathToFile);

				std::stringstream rs;
				rs << readFile.rdbuf();
				
				InvalidateSymbolsInNode(it.node);
				it.node->Reset();

				it.node->URI = it.uri;
				auto check = ParseIntoNode(it.node, rs.str(), it.imm);
				if (check.err) {
					std::string importChain;

					importChain = " while importing " + it.node->URI.str();
					
					return ParserError(
						check.err->GetSourceFilePosition(),
						check.err->GetErrorMessage() + importChain
					);
				} else {
					return check;
				}
			} else {
				return FileError("Could not open " + it.uri.str() + " (" + pathToFile + ")");
			}
			return { };
		}

		static void DumpDot(std::ostream& dot, RepositoryNode* node, std::unordered_set<RepositoryNode*>& visited) {
			bool first = visited.empty();
			if (visited.count(node)) return;
			visited.emplace(node);

			if (first) {
				dot << "\n\ndigraph dependencies {\n\tnode [shape=box]\n";
			}

			dot << "\tn" << (uintptr_t)node << " [label=\"" << node->URI.str() << "\"]\n";

			for (auto i : node->imports) {
				dot << "\tn" << (uintptr_t)i << " -> n" << (uintptr_t)node << "\n";
				DumpDot(dot, i, visited);
			}
			
			if (first) {
				dot << "}\n\n";
			}
		}

		Err<void> Repository::Transaction(std::function<Err<void>()> tx) {
			rollback.clear();
			changed_symbols.clear();
			Err<void> success;
			
			success = tx();			
			if (success.err) {
				importQueue.clear();

				for (auto& rb : rollback) {
					*rb.first = rb.second;
				}

				for (auto& mod : modules) {
					for (auto i = mod.second.files.begin(); i != mod.second.files.end();) {
						auto thisFile = i++;
						if (thisFile->second->changes.empty()) {
							mod.second.files.erase(thisFile);
						}
					}
				}

				changed_symbols.clear();
			}
			rollback.clear();
			return success;
		}

		Err<void> Repository::PerformQueue() {
			while (importQueue.size()) {
				auto it = importQueue.back();
				importQueue.pop_back();
				CHECK_ERR(Perform(it));
			}
#if 0
			std::unordered_set<RepositoryNode*> vs;
			DumpDot(std::cout, &root, vs);
#endif

			return UpdateDefinitions();
		}

		Err<void> Repository::ImportTree(BufferKey fromLeaf, immediate_handler_t imm) {	
			assert(importQueue.empty());
			CHECK_ERR(CreateImportTask(&root, fromLeaf, imm));
			CHECK_ERR(PerformQueue());
			return {};
		}

		Repository::Repository() {
			const char* repo = getenv("KRONOS_CORE_LIBRARY_REPOSITORY");
			const char* repoVersion = getenv("KRONOS_CORE_LIBRARY_VERSION");
			if (!repo) repo = KRONOS_CORE_LIBRARY_REPOSITORY;
			if (!repoVersion) repoVersion = KRONOS_CORE_LIBRARY_VERSION;

			defaultRepo = repo;
			defaultVersion = repoVersion;

			root.AddImport(&kernel);
			root.URI = { "< root", "context", ">" };
			kernel.URI = { repo, "#CORE", repoVersion };
		}

		Err<void> Repository::ImportFile(const char* path, bool canOverwrite) {
			relativeImportBase = { "#LOCAL", "", "" };
			return Transaction([=]() -> Err<void> { 
				BufferKey uri{ "#LOCAL", "", path };
				return ImportTree(uri, { });
			});
		}

		Err<void> Repository::ImportBuffer(const char* sourceCode, bool canOverwrite, immediate_handler_t imm) {
			return Transaction([=]() -> Err<void> {
				relativeImportBase = { "#LOCAL","","" };
				parser_state_t incr;
				if (adhoc.size()) {
					incr = adhoc.back().incremental;
				}
				auto& newNode = *adhoc.emplace(adhoc.end());
				newNode.canOverwrite = true;
				newNode.incremental = incr;
				CHECK_ERR(ParseIntoNode(&newNode, sourceCode, imm));
				root.AddImport(&newNode);
				InvalidateSymbolsInNode(&newNode);
				return PerformQueue();
			});
		}

		Err<void> Repository::ImportCoreLib(const char* file) {
			return Transaction([=]() -> Err<void> {
				relativeImportBase = { "#CORE", "", "" };
				CHECK_ERR(ImportTree({ "#CORE", "", file }, { }));
				return { };
			});
		}

		void Repository::Rebind(const std::string& qn, Nodes::CGRef expr) {
			completeDefinitions[qn].graph = expr;
		}

		RepositoryBuilder Repository::GetKernelBuilder() {
			return RepositoryBuilder{ ":", *this };
		}

		RepositoryBuilder::~RepositoryBuilder() {
		}

		void Repository::GetPosition(const char* memPos, std::string& uri, int& line, int& column, std::string* show_line) {
			for (auto& m : modules) {
				for (auto& f : m.second.files) {
					auto& src{ f.second->retainSource };
					if (src.data() < memPos &&
						src.data() + src.size() > memPos) {

						uri = f.second->fileSystemPath;
						line = 0;
						column = 0;

						const char* lineBeg = src.data();
						const char* pos = src.data();
						while(pos < memPos) {
							if (*pos++ == '\n') { 
								++line; column = 0;								
								lineBeg = pos;
							} else {
								++column;
							}
						}

						if (show_line) {
							while (*pos && *pos != '\n') ++pos;
							while (isspace(*lineBeg) && lineBeg < pos) {
								++lineBeg; --column;
							}
							*show_line = "\t" + std::string(lineBeg, pos) + "\n\t"
								+ std::string(column, ' ') + "^^^\n";
						}
						return;
					}
				}
			}
			uri.clear(); line = column = -1;
		}

		void RepositoryBuilder::AddMacro(const char* relative, CGRef val, bool thing) {
			std::string sym = path + relative;
			r.kernel.changes[sym].forms.emplace_back(Form::Val(val));
			r.changed_symbols.emplace(sym);
		}

		void RepositoryBuilder::AddFunction(const char* relative, CGRef val, const char* args, const char* doc, const char* fallback) {
			std::string sym = path + relative;
			if (fallback) {
				AddFunction(
					relative, 
					Evaluate::New(fallback,
						Lib::Reference::New({ fallback }),
						GenericPair::New(Lib::Reference::New({ sym }),
							GenericPair::New(Invariant::Constant::New(Type(relative)),
								GenericArgument::New()))));
			}

			Form f = Form::Fn(val);
			f.attr = Attributes::Extend;
			r.kernel.changes[sym].forms.emplace_back(f);
			if (args && doc && strlen(doc)) {
				auto& md{ r.kernel.changes[sym].metadata["("s + args + ")"] };
				if (md.size()) md += "\n\n";
				md += doc;
			}
			r.changed_symbols.emplace(sym);
		}

		MemoryRegion* RepositoryBuilder::GetMemoryRegion() {
			return r.kernel.graphs;
		}
	}
}

/* 

	Import buffer

	- construct local definitions -> changes
	- push invalidates to importees (changes)
	- walk imports (will push invalidates to me)

	- after import cycle, root should have all the invalidates. Then validate the constructs.
*/