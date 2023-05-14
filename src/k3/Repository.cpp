#include "UserErrors.h"
#include "Repository.h"
#include "SymbolResolutionTransform.h"
#include "Evaluate.h"
#include "Invariant.h"
#include "TypeAlgebra.h"
#include "LibraryRef.h"
#include "TLS.h"
#include "EnumerableGraph.h"

#include <algorithm>
#include <iostream>

namespace K3 {
	CGRef Repository::Symbol::GetGraph() const {
		CGRef graph = nullptr;
		for (auto f : GetForms()) {
			if (graph) graph = GenericPair::New(f, graph);
			else graph = f;
		}
		graph->HasInvisibleConnections();
		return graph;
	}

	static Type GetSymbolMetadata(const std::string& qn, const Repository::Symbol& sym) {
		Type argList(false);
		for (auto al : sym.ArgumentLists) {
			Type alTy = Type(al.c_str( ));
			argList = argList.IsNil() ? alTy : Type::Pair(alTy, argList);
		}
		return 	Type::Tuple(
			Type("Symbol"),
			Type(qn.c_str( )),
			argList,
			Type(sym.Comment.c_str( )),
			Type(sym.GetGraph( )));

	}

	Type Repository::Namespace::GetMetadata( ) const {
		std::vector<Type> metadata;
		metadata.reserve(namespaces.size( ) + symbols.size( ) + 1);
		auto packStr = Type("Package");
		for (auto ns : namespaces) {
			metadata.push_back(
				Type::Tuple(
				packStr,
				Type(ns.first.c_str()),
				ns.second->GetMetadata( )));
		}

		for (auto sym : symbols) {
			metadata.push_back(GetSymbolMetadata(sym.first, sym.second));
		}

		if (GetQualifiedName() == ":") {
			std::vector<std::vector<const char*>> internalSyms = {
				{ "z-1","init sig~","Initially 'init'. Subsequently 'sig~' delayed by one clock tick. The types of 'init' and 'sig~' must match. 'sig~' is an unordered connection and permits unit delay recursion." },
				{ "rbuf","init order sig~","Ring buffer with 'order' samples initialized to the type and value of 'init'. The types of 'init' and 'sig~' must match. 'sig~' is an unordered connection and permits unit delay recursion."},
				{ "Make", "tag content", "Wraps 'content' in a user type identified by type tag 'tag'"},
				{ "Break", "tag value", "Unwraps content from the 'value' of an user type whose type tag matches 'tag'"}
			};

			for (auto&& sv : internalSyms) {
				metadata.push_back(Type::Tuple(
					Type("Symbol"),
					Type(sv[0]),
					Type(sv[1]),
					Type(sv[2]),
					Type("Internal")));
			}
		}

		std::sort(metadata.begin( ), metadata.end( ));

		Type mdType = Type::Nil;
		for (auto md = metadata.rbegin( ); md != metadata.rend( ); ++md) {
			mdType = Type::Pair(*md, mdType);
		}
		return mdType;
	}

	const Repository::Symbol* Repository::Namespace::Find(const std::string& identifier) const {
		auto f = symbols.find(identifier);
		if (f == symbols.end()) return nullptr;
		else return f->second;
	}

	Repository::Symbol& Repository::Namespace::Get(const std::string& id) {
		auto f = symbols.find(id);
		if (f == symbols.end()) {
			return *symbols.insert(make_pair(id, new Symbol(GetQualifiedName() + id))).first->second;
		}
		else {
			if (f->second.Unique()) return f->second;
			else return *(symbols[id] = new Symbol(*f->second));
		}
	}

	const Repository::Namespace* Repository::Namespace::FindNamespace(const std::string &id) const {
		auto f = namespaces.find(id);
		if (f == namespaces.end()) return nullptr;
		else return f->second;
	}

	Repository::Namespace& Repository::Namespace::GetNamespace(const std::string& id, const std::function<void(void)>& onCreate) {
		auto f = namespaces.find(id);
		if (f == namespaces.end()) {
			onCreate();
			return *namespaces.insert(make_pair(id, new Namespace(GetQualifiedName() + id + ":"))).first->second;
		}
		else {
			if (f->second.Unique()) return f->second;
			else return *(namespaces[id] = new Namespace(*f->second));
		}
	}

	void Repository::InvalidateQualifiedName(const std::string& sym) {
		changedSymbols.insert(sym);

		auto f = resolutionCache.find(sym);
		if (f != resolutionCache.end( )) {
			auto dsr = dependants.equal_range(sym);
			std::list<std::pair<std::string,std::string>> dependantSymbols(dsr.first, dsr.second);
			resolutionCache.erase(f);
			dependants.erase(dsr.first, dsr.second);
			for (auto dn : dependantSymbols) {
				InvalidateQualifiedName(dn.second);
			}
		}
	}

	void Repository::ListenToSymbolChanges(const std::function<void(const std::string&)>& cb) {
		symbolChangeListener = cb;
	}

	void Repository::SendChangeNotifications( ) {
		auto syms = changedSymbols;
		changedSymbols.clear( );
		if (symbolChangeListener) for (auto&& s : syms) symbolChangeListener(s);
	}

	void Repository::NotifyOfChangesWRT(const Repository &newRepo) {
		auto rccopy(resolutionCache);
		for (auto rs : rccopy) {
			auto nrs = newRepo.resolutionCache.find(rs.first);
			if (nrs == newRepo.resolutionCache.end() ||
				rs.second != nrs->second) {
				InvalidateQualifiedName(rs.first);
			}
		}
	}

	Repository::Namespace* Repository::FindParentPackage(std::string &path, bool needMutable) {
		Namespace *current = root;

		if (path.size() > 0 && path[0] == ':') {
			path = path.substr(1);
		}

		size_t pos;
		while ((pos = path.find_first_of(':')) != path.npos) {
			string segment = path.substr(0, pos);

			current = needMutable ? &current->GetNamespace(segment, [&](){ InvalidateQualifiedName(current->GetQualifiedName() + segment); })
				: const_cast<Namespace*>(current->FindNamespace(segment));

			if (current == nullptr) return nullptr;

			path = path.substr(pos + 1);
		}

		if (current == root && needMutable && root.Unique() == false) {
			root = new Namespace(*root);
			return root;
		}

		return current;
	}

	const Repository::Symbol* Repository::Find(std::string qualifiedName) {
		const Namespace *ns = FindParentPackage(qualifiedName, false);
		return ns ? ns->Find(qualifiedName) : nullptr;
	}

	Type Repository::GetMetadata(std::string qualifiedName) {
		const Namespace *ns = FindParentPackage(qualifiedName, false);

		auto sym = ns->Find(qualifiedName);
		if (sym) return GetSymbolMetadata(qualifiedName, *sym);

		if (ns == nullptr) return Type::Nil;
		auto subns = qualifiedName.empty() ? ns : ns->FindNamespace(qualifiedName);
		if (subns == nullptr) return Type::Nil;
		return subns->GetMetadata( );
	}

	Repository::Symbol& Repository::Get(std::string qualifiedName) {
		auto pack = FindParentPackage(qualifiedName, true);
		auto&& sym = pack->Get(qualifiedName);
		InvalidateQualifiedName(sym.GetQualifiedName());
		return sym;
	}

	bool Repository::NamespaceExists(std::string qualifiedName) {
		return FindParentPackage(qualifiedName, false) != nullptr;
	}

	Repository::Repository() :root(new Namespace(":")) { }

	const Repository::ResolvedSymbol* Repository::Resolve(std::string qualifiedName) {
		auto f = resolutionCache.find(qualifiedName);
		if (f == resolutionCache.end( )) {
			const Symbol* sym = Find(qualifiedName);
			if (sym == nullptr) return nullptr;

			auto resolved = resolutionCache[qualifiedName] = new ResolvedSymbol();
			auto symGraph = sym->GetGraph();

			Transform::SymbolResolution resolutionTransform(*this, "", symGraph, resolved);
			resolved->resolved = resolutionTransform.Go( );

			for (auto dn : resolutionTransform.GetDependencies( )) {
				dependants.insert(make_pair(dn, qualifiedName));
			}

			for (auto form : Qxx::FromGraph(resolved->resolved).OfType<Invariant::Constant>( )
								 .Where([](Invariant::Constant *c) { return c->GetType().IsGraph( ); } )) {
				for (auto n : Qxx::FromGraph(form->GetType().GetGraph()).OfType<Evaluate>( )) {
					Lib::LibraryGraph *lg;
					if (n->GetUp(0)->Cast(lg) && lg->GetSymbol( ) == resolved)
						resolved->RecursionPoints.push_back(n);
				}
			}
			
			resolved->resolved->HasInvisibleConnections( );
			return resolved;
		} else {
			return f->second;
		}
	}

	void Package::AmendCommentFor(const std::string& label, const std::string& comment) {
		auto& sym = repo.Get(GetPath() + label);
		if (sym.Comment.size() && sym.Comment.back() != '\n') sym.Comment.push_back('\n');
		sym.Comment += comment;
	}

	bool Package::AddFunction(const std::string& path, const Graph<Nodes::Generic>& graph, const char *arglist, const char *comment, const char *fallback) {
		auto& sym(repo.Get(GetPath() + path));

		CGRef root = graph;
		Invariant::Constant *c;
		while (root->TypeID() == Lib::PackageDirective::ClassID()) root = root->GetUp(0);
		if (root->TypeID() == GenericRest::ClassID() && root->GetUp(0)->Cast(c) && c->GetType().IsNil()) {
			// this form always fails, so discard the previous forms
			sym.GetForms().clear();
		}

		if (fallback && sym.GetForms().empty()) {
			RegionAllocator fallbackAllocator;

			sym.GetForms().push_back(
				Invariant::Constant::New(Type::InvariantGraph(
					Evaluate::New(TLS::GetCurrentInstance()->Memoize(path + "-Fallback"),
					Lib::Symbol::New(fallback),
					GenericPair::New(Lib::Symbol::New(sym.GetQualifiedName()), GenericArgument::New())))));
		}

		auto bindPos(Parser::CurrentSourcePosition = graph->GetRepositoryAddress());
		sym.GetForms().push_back(Invariant::Constant::New(Type::InvariantGraph((CGRef)graph)));

		if (arglist) sym.ArgumentLists.push_back(arglist);
		if (comment) {
			if (sym.Comment.size() && sym.Comment.back() != '\n') sym.Comment += '\n';
			sym.Comment += comment;
		}

		return true;
	}

	bool Package::AddMacro(const std::string& path, const Graph<Nodes::Generic>& graph, bool allowOverWrite) {
		auto& sym(repo.Get(GetPath() + path));

		if (allowOverWrite) sym.GetForms( ).clear( );
		else if (sym.GetForms().size()) return false;

		sym.GetForms().push_back((CGRef)graph);
		return true;
	}

	void Package::AddParameterList(const std::string& id, const std::string& argList) {
		auto& sym(repo.Get(GetPath() + id));
		sym.ArgumentLists.push_back(argList);
	}

	void Package::AddSymbolSource(const std::string& id, const std::string& argList, const std::string& sourceCode) {
		auto& sym(repo.Get(GetPath() + id));
		sym.Source += id + "( " + argList + " ) {\n" + sourceCode + "\n}\n\n";
	}

	Package Package::AddPackage(const std::string& path) {
		return Package(repo, GetPath() + path);
	}
}

#include "Invariant.h"
#include "FlowControl.h"
#include "DynamicVariables.h"

namespace K3 {
	void BuildReactivePrimitiveOps(Package);
	void BuildVectorPrimitiveOps(Package);
	void BuildSelectPrimitiveOps(Package);
	void BuildNativePrimitiveOps(Package);

	void Package::BuildKernelFunctions() {
		RegionAllocator KernelBaseAllocator;

		auto arg = Nodes::GenericArgument::New();
		auto b1 = Nodes::GenericFirst::New(arg);
		auto b2 = Nodes::GenericRest::New(arg);

		AddPackage("Fallback").AddFunction("Eval", Nodes::Invariant::GenericNoFallback::New());

		AddFunction("Eval", Nodes::Invariant::GenericNoFallback::New());
		AddFunction("Eval", Nodes::Evaluate::New("eval", b1, b2), "func arg","Evaluates 'func' as a function with the argument 'arg'");

		AddFunction("External", Nodes::GenericExternalVariable::New(b1, b2), "key default", "External input declaration with the identifier 'key' and type and default value provided by 'default'");
		AddFunction("External-Data", Nodes::GenericDataProvider::New(b1, b2), "provider key", "Provides data from the IO module 'provider'. 'key' identifies a particular data item. Specifics depend on the provider.");
		AddFunction("External-Stream", Nodes::GenericStreamInput::New(b1, GenericFirst::New(b2),GenericRest::New(b2)), "stream-key default clock", "Stream input to this module from an external vector. The sample rate of the buffer is determined by 'clock'.");
		AddFunction("External-Out", Nodes::GenericStreamOutput::New(b1, b2), "sig keys", "Splits the signal according to structure of 'keys', registering a reactive stream output for each atom in 'keys'.");

		AddFunction("Raise", Nodes::Raise::New(arg), "e", "Raises a user exception of type 'e'");

		BuildSelectPrimitiveOps(*this);
		BuildNativePrimitiveOps(*this);
		BuildInvariantPrimitiveOps(*this);
		BuildReactivePrimitiveOps(AddPackage("Reactive"));
		BuildVectorPrimitiveOps(AddPackage("Vector"));
	}
}