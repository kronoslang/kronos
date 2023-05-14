#pragma once

#include "NodeBases.h"
#include <unordered_map>
#include <set>
#include <string>

namespace K3 {
	using namespace Nodes;

	/* api compatibility wrapper */
	class Repository;
	class Package {
		Repository& repo;
		std::string path;
	public:
		Package(Repository& r, std::string p) :repo(r), path(std::move(p)) { if (path.empty( ) || path.back( ) != ':') path.push_back(':'); };

		void AmendComment(const std::string& comment) { }
		void AmendCommentFor(const std::string& label, const std::string& comment);

		bool AddFunction(const std::string& path, const Graph<Nodes::Generic>& graph, const char *arglist = 0, const char *comment = 0, const char *fallback = 0);
		bool AddMacro(const std::string& path, const Graph<Nodes::Generic>& graph, bool allowRedefinition);
		void AddParameterList(const std::string& identifier, const std::string &argList);
		void AddSymbolSource(const std::string& identifier, const std::string& argList, const std::string &srcCode);

		Package AddPackage(const std::string& path);

		const std::string& GetPath() const { return path; }

		void BuildKernelFunctions();
		Repository& GetRepository() { return repo; }
	};

	class Repository {
	public:
		class Symbol : public RefCounting {
			std::string qualifiedName;
			std::vector<Graph<const Generic>> forms;
		public:
			Symbol(const std::string& qn):qualifiedName(qn) { }
			std::vector<Graph<const Generic>>& GetForms() { return forms; }
			const std::vector<Graph<const Generic>>& GetForms() const { return forms; }
			const std::string& GetQualifiedName() const { return qualifiedName; }
			std::string Comment;
			std::string Source;
			std::vector<std::string> ArgumentLists;
			CGRef GetGraph() const;
		};

		struct ResolvedSymbol : public RefCounting {
			Graph<Nodes::Generic> resolved;
			std::vector<Nodes::CGRef> RecursionPoints;
		};

		class Namespace : public RefCounting {
			typedef std::unordered_map<std::string, Ref<Namespace>> NsMapTy;
			typedef std::unordered_map<std::string, Ref<Symbol>> SymMapTy;
			NsMapTy namespaces;
			SymMapTy symbols;
			std::string qualifiedName;
		public:
			Namespace(const std::string& qn) :qualifiedName(qn) {}
			const Symbol* Find(const std::string& identifier) const;
			Symbol& Get(const std::string& identifier);

			const Namespace* FindNamespace(const std::string& identifier) const;
			Namespace& GetNamespace(const std::string& identifier, const std::function<void(void)>& onCreate = [](){});

			const std::string& GetQualifiedName() const { return qualifiedName; }

			Type GetMetadata( ) const;
		};
	private:
		std::set<std::string> changedSymbols;
		std::unordered_map<std::string, Ref<ResolvedSymbol>> resolutionCache;
		std::unordered_multimap<std::string, std::string> dependants;
		std::function<void(const std::string&)> symbolChangeListener;
		Ref<Namespace> root;

		Namespace* FindParentPackage(std::string &path, bool needMutable);
		void InvalidateQualifiedName(const std::string&);
	public:
		Repository();

		const Symbol* Find(std::string qulifiedName);
		Symbol& Get(std::string qualifiedName);
		Type GetMetadata(std::string namespaceRoot);

		/* resolve symbols inside this symbol */
		const ResolvedSymbol* Resolve(std::string qualifiedName);

		/* check if namespace exists */
		bool NamespaceExists(std::string qualifiedName);

		operator Package() { return Package(*this, ":"); }

		void ListenToSymbolChanges(const std::function<void(const std::string&)>& callback);
		void SendChangeNotifications( );
		void NotifyOfChangesWRT(const Repository& diffTo);
	};
}