#pragma once

#include "common/DynamicScope.h"
#include "common/Err.h"
#include "Errors.h"
#include "UserErrors.h"
#include "Graph.h"
#include "NodeBases.h"
#include "RepositoryBuilder.h"

#include <string>
#include <map>
#include <unordered_map>
#include <deque>
#include <list>
#include <functional>
#include <stdexcept>
#include <vector>
#include <list>

namespace lithe {
	struct node;
}

#define PROPAGATE_ERROR(expr) { auto ___val = expr; if (val.err) return *val.err; else return *val;}

namespace K3 {
	class RepositoryState;

	using immediate_handler_t = std::function<void(const char*, Nodes::CGRef)>;

	using AstNode = lithe::node;

	namespace Parser {
		struct ParserError : public Kronos::IError {
			const char *parse_pt;
			std::string msg;
			ParserError(const char *parse_pt, const std::string msg) :parse_pt(parse_pt),msg(msg) {}

			const char* GetErrorMessage() const noexcept override {
				return msg.c_str();
			};
			ErrorType GetErrorType() const noexcept override {
				return Kronos::IError::SyntaxError;
			}
			const char* GetSourceFilePosition() const noexcept override {
				return parse_pt;
			}

			int GetErrorCode() const noexcept override {
				return Error::BadInput;
			}

			void Delete() noexcept override {
				delete this;
			}

			IError* Clone() const noexcept override {
				return new ParserError(parse_pt, msg);
			}
		};

		struct FileError : public Kronos::IError {
			std::string msg;
			FileError(const std::string msg) : msg(msg) {}

			const char* GetErrorMessage() const noexcept override {
				return msg.c_str();
			};
			ErrorType GetErrorType() const noexcept override {
				return Kronos::IError::RuntimeError;
			}
			const char* GetSourceFilePosition() const noexcept override {
				return nullptr;
			}

			int GetErrorCode() const noexcept override {
				return Error::FileNotFound;
			}

			void Delete() noexcept override {
				delete this;
			}

			IError* Clone() const noexcept override {
				return new FileError(msg);
			}
		};

		extern DynamicScope<const char*> CurrentSourcePosition;

		using namespace K3::Nodes;

		enum class Attributes {
			None = 0,
			Pattern = 1,
			Extend = 2,
			AlwaysOverride = 4,
			MayOverride = 8
		};

		struct Form {
			CGRef body;
			Attributes attr = Attributes::None;

			bool HasAttr(Attributes a) const {
				return ((int)attr & (int)a) != 0;
			}

			enum Mode {
				Undefined,
				Macro,
				Function,
			} mode = Undefined;

			static Form Fn(CGRef expr);
			static Form Val(CGRef expr);
		};

		class Repository2;

		struct symbol_t {
			Graph<Nodes::Generic> graph;
			std::unordered_map<std::string, std::string> metadata;
		};

		struct PartialDefinition {
			std::vector<Form> forms;
			std::vector<Type> recurData;
			std::unordered_map<std::string, std::string> metadata;
			Err<CGRef> Complete(Repository2*, const std::string& name = "") const;
			Err<symbol_t> CompleteMeta (Repository2*, const std::string& name = "") const;
			static PartialDefinition Value(CGRef expr);
			void Append(const PartialDefinition& other);
		};

		struct parser_state_t {
			std::deque<std::string> namespaces = { ":" };
			std::unordered_map<std::string, std::string> explicit_uses;
		};

		using CI = std::vector<AstNode>::const_iterator;
		using resolution_t = std::pair<const AstNode*, std::function<Err<PartialDefinition>()>>;
		using resolver_map_t = std::unordered_map<std::string, std::vector<resolution_t>>;

		struct BufferKey {
			std::string package, version, path;
			operator std::string() const {
				if (path == "main.k") {
					return "[" + package + " " + version + "]";
				} else {
					return "[" + package + " " + version + " " + path + "]";
				}
			}

			std::string str() const {
				return *this;
			}
		};

		using DefinitionMap = std::unordered_map<std::string, PartialDefinition>;

		Err<DefinitionMap> GenerateSymbols(const AstNode& parseTree, 
							parser_state_t& state,
							std::function<Err<void>(BufferKey)> importHook,
							immediate_handler_t imm);

		struct RepositoryNode {
			Ref<MemoryRegion> graphs = new MemoryRegion;
			std::unordered_map<std::string, PartialDefinition> changes;

			std::vector<RepositoryNode*> imports;

			std::string retainSource;
			bool canOverwrite = false;

			PartialDefinition Resolve(const std::string& qualifiedName, std::unordered_set<RepositoryNode*>& visited);
			void Resolve(std::unordered_map<std::string, PartialDefinition>& aggregate, std::unordered_set<RepositoryNode*>& visited);

			void AddImport(RepositoryNode*);
			void Reset();

			parser_state_t incremental;
			BufferKey URI;
			std::string fileSystemPath;
		};

		struct RepositoryModule {
			std::unordered_map<std::string, RepositoryNode*> files;
			std::string version;
		};

		class Repository2 {
			friend struct RepositoryBuilder;
			Ref<MemoryRegion> stubs = new MemoryRegion();
			std::unordered_map<std::string, RepositoryModule> modules;
			std::unordered_map<std::string, symbol_t> completeDefinitions;
			std::unordered_map<RepositoryNode*, RepositoryNode> rollback;
			std::unordered_map<std::string, std::unique_ptr<RepositoryNode>> nodes;
			std::list<RepositoryNode> adhoc;
			RepositoryNode root;
			RepositoryNode kernel;
			Err<symbol_t> Build(const std::string& qualifiedName);
			std::string defaultRepo, defaultVersion;

			static std::string NodeKey(std::string const& package, std::string const& file) {
				return package + ":" + file;
			}

			struct ImportTask {
				BufferKey uri;
				RepositoryNode* node;
				immediate_handler_t imm;
				BufferKey importer;
			};
			
			std::vector<ImportTask> importQueue;
			
			Err<void> ImportTree(BufferKey firstLeaf, immediate_handler_t);
			Err<void> CreateImportTask(RepositoryNode* importer, BufferKey identifier, immediate_handler_t);
			Err<void> ParseIntoNode(RepositoryNode* node, const std::string& sourceCode, immediate_handler_t);
			Err<void> Perform(ImportTask);

			BufferKey relativeImportBase;

			Err<void> PerformQueue();

			void InvalidateSymbolsInNode(RepositoryNode*);

			Err<void> Transaction(std::function<Err<void>()>);

		public:
			std::unordered_set<std::string> changed_symbols;

			Repository2();

			const symbol_t* Lookup(const std::string& qualifiedName);
			void Rebind(const std::string& qualifiedName, Nodes::CGRef expr);

			ParserError RedefinitionError(std::string qn,
										  const char* newPoint, Form::Mode newMode,
										  const char* oldPoint, Form::Mode oldMode);

			void SetCoreLib(std::string repo, std::string version) {
				defaultRepo = repo;
				defaultVersion = version;
			}

			const std::string& GetCoreLibPackage() {
				return defaultRepo;
			}

			const std::string& GetCoreLibVersion() {
				return defaultVersion;
			}

			void ExportMetadata(std::ostream& json);

			Err<void> ImportFile(const char* path, bool canOverwrite);
			Err<void> ImportBuffer(const char* code, bool canOverwrite, immediate_handler_t = {});
			Err<void> ImportCoreLib(const char* file);

			RepositoryBuilder GetKernelBuilder();

			void GetPosition(const char* mem_pos, std::string& uri, int& line, int& column, std::string* show_line);

			Err<void> UpdateDefinitions();
		};
	};
};