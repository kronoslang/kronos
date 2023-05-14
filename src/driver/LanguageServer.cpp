#include "LanguageServer.h"
#include "JsonRPCEndpoint.h"
#include "common/PlatformUtils.h"
#include "driver/package.h"

#include "lithe/lithe.h"
#include "lithe/grammar/kronos.h"

#include "config/corelib.h"

#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
#include <deque>
#include <functional>
#include <sstream>
#include <fstream>
#include <regex>

namespace Kronos {
	namespace LanguageServer {
		using AstNode = lithe::node;

		enum CompletionKind {
			Package = 9,
			Keyword = 14,
			Function = 3,
			Symbol = 6,
			Type = 14
		};

		struct CompletionData {
			CompletionKind kind;
			std::string argList, argSnippet;
			std::string doc;
			const char *occurs;
			std::string qualifiedName;

			bool operator==(const CompletionData& rhs) const {
				return std::make_tuple(kind, argList) ==
					std::make_tuple(rhs.kind, rhs.argList);

			}

			bool operator<(const CompletionData& rhs) const {
				return std::make_tuple(kind, argList) < std::make_tuple(rhs.kind, rhs.argList);
			}
		};

		struct SymbolData {
			using CompletionRecord = std::pair<std::string, CompletionData>;

			std::map<const char*, SymbolData> children;
			std::multimap<std::string, CompletionData> defines;
			std::vector<std::string> uses = { ":" };
			const char *end = nullptr;
			std::string inNamespace = ":";

			void Define(const std::string& qualifiedName, CompletionData cd) {
				auto symbolName = qualifiedName;
				auto colonPos = symbolName.find_last_of(":");
				if (colonPos != symbolName.npos &&
					colonPos > 0 &&
					colonPos < symbolName.size() - 1) {
					symbolName = symbolName.substr(colonPos + 1);
				}

				if (symbolName.front() == ':') symbolName = symbolName.substr(1);

				cd.qualifiedName = qualifiedName;
				for (auto &c : symbolName) c = tolower(c);
				defines.emplace(symbolName, std::move(cd));
			}

			SymbolData& Find(const char *pos) {
				if (children.empty()) return *this;
				auto ub = children.upper_bound(pos);
				if (ub == children.begin()) return *this;
				--ub;
				if (pos >= ub->second.end) return *this;
				return ub->second.Find(pos);
			}

			std::vector<std::string> GetPossibleQualifiedNames(const char* pos, std::string tok) {
				auto &sd = Find(pos);
				std::vector<std::string> qns;
				qns.emplace_back(tok);
				if (tok.size() && tok[0]!= ':')
				for (auto &u : sd.uses) {
					qns.emplace_back(u + tok);
				}
				return qns;
			}

			std::string SplitQualifiedName(std::string& name) {
				std::string pack;
				auto cpos = name.find_last_of(":");
				if (cpos != name.npos) {
					pack = name.substr(0, cpos + 1);
					name = name.substr(cpos + 1);
				}
				for (auto &c : name) c = tolower(c);
				return pack;
			}

			void GatherLocalDefinitions(const char *pos, std::string sym, std::vector<const char*>& defs) {
				auto pack = SplitQualifiedName(sym);
				auto &sd = Find(pos);
				if (&sd != this) {
					auto def = sd.defines.find(sym);
					if (def != sd.defines.end()) {
						if (IsInPackage(pack, def->second.qualifiedName)) {
							defs.emplace_back(def->second.occurs);
						}
					}
				}
			}

			void GatherGlobalDefinitions(const std::vector<std::string>& tok, std::vector<const char*>& defs) {
				for (auto sym : tok) {
					auto pack = SplitQualifiedName(sym);
					auto gv = defines.find(sym);
					if (gv != defines.end() && IsInPackage(pack, gv->second.qualifiedName)) {
						defs.emplace_back(gv->second.occurs);
						return;
					}
				}

				if (defs.empty()) {
					for (auto& c : children) c.second.GatherGlobalDefinitions(tok, defs);
				}
			}

			CompletionRecord GetLocalHoverInfo(const char *pos, std::string tok) {
				auto cpos = tok.find_last_of(":");
				std::string package;
				
				if (cpos != tok.npos) {
					package = tok.substr(0, cpos + 1);
					tok = tok.substr(cpos + 1);
				}

				for (auto &c : tok) c = tolower(c);

				auto &sd = Find(pos);
				if (&sd != this) {
					auto lv = sd.defines.find(tok);
					if (lv != sd.defines.end()) return { lv->first, lv->second };
				}
				return {};
			}

			CompletionRecord GetGlobalHoverInfo(const std::vector<std::string>& tokens) {
				for (auto cr : tokens) {
					for (auto &c : cr) c = tolower(c);
					auto cpos = cr.find_last_of(":");

					if (cpos != cr.npos) {
						cr = cr.substr(cpos + 1);
					} 

					auto gv = defines.find(cr);
					if (gv != defines.end()) return { cr, gv->second };
				}

				for (auto &c : children) {
					auto cr = c.second.GetGlobalHoverInfo(tokens);
					if (cr.first.size()) return cr;
				}
				return {};
			}

			bool IsInPackage(const std::string& package, const std::string& qualifiedName) {
				if (package.empty()) return true;

				if (package.front() != ':' && qualifiedName.front() == ':') {
					return IsInPackage(package, qualifiedName.substr(1));
				}

				return qualifiedName.compare(0, package.size(), package) == 0;
			}

			void GatherLocalCompletions(const char *pos, const std::string& inPackage, const std::string& prefix, std::set<CompletionRecord>& completions, std::vector<std::string>& uses) {
				auto& sd = Find(pos);
				if (&sd != this) {
					for(auto i = defines.lower_bound(prefix), e = defines.upper_bound(prefix + "~"); i!= e; ++i) {
						if (IsInPackage(inPackage, i->second.qualifiedName))
							completions.emplace(i->second.qualifiedName, i->second);
					}
				}
				uses = sd.uses;
			}

			void GatherGlobalCompletions(const std::string& inPackage, const std::string& prefix, const std::vector<std::string>& uses, std::set<CompletionRecord>& completions) {
				for (auto i = defines.lower_bound(prefix), e = defines.upper_bound(prefix + "~"); i != e; ++i) {
					const auto& qn{ i->second.qualifiedName };

					if (qn.find(inPackage) != qn.npos) {
						std::string shortest{ qn };
						for (auto &u : uses) {
							if (qn != u && qn.find(u) == 0) {
								auto shorten = qn.substr(u.size());
								if (shorten.size() < shortest.size()) shortest = shorten;
							}
						}
					

						if (IsInPackage(inPackage, shortest)) {
							completions.emplace(shortest, i->second);
						}
					}
				}
			}
		};

        std::unique_ptr<Packages::DefaultClient> bbClient;

        struct DocumentContext {
			struct ServerInstance {
				std::unordered_map<std::string, std::unique_ptr<DocumentContext>> documents;

				DocumentContext& Get(std::string uri) {
#ifdef WIN32
					for (auto& c: uri) if (c == '\\') c = '/';
#endif
					auto& docRef = documents[uri];

					if (!docRef) {
						docRef = std::make_unique<DocumentContext>(*this, uri);
						docRef->Load(uri);
						docRef->LoadStubs();
					}

					return *docRef;
				}

				DocumentContext& Get(const picojson::value& textDocument) {
					auto uri = textDocument.get("uri").to_str();
					if (textDocument.contains("text")) {
						auto& docRef = documents[uri];
						if (!docRef) docRef = std::make_unique<DocumentContext>(*this, uri);
						docRef->Update(textDocument.get("text").to_str());
					}
					return Get(uri);
				}

				DocumentContext& Get(const picojson::value& v, const char*& pos) {
					auto& dc(Get(v.get("textDocument")));
					auto row = (int)v.get("position").get("line").get<double>();
					auto col = (int)v.get("position").get("character").get<double>();
					pos = dc.GetPosition(row, col);
					return dc;
				}
			};

			ServerInstance& srv;
			std::unordered_map<std::string, DocumentContext*> imports;

			const std::string documentUri;
			std::string document;
			std::vector<const char*> documentLines;

			SymbolData analysis;

			DocumentContext(ServerInstance& si, std::string uri) :srv(si), documentUri(uri) {
			}

			void LoadStubs() {
				auto stubs = bbClient->Resolve(KRONOS_CORE_LIBRARY_REPOSITORY, "builtins.stub", KRONOS_CORE_LIBRARY_VERSION);
				auto prelude = bbClient->Resolve(KRONOS_CORE_LIBRARY_REPOSITORY, "Prelude.k", KRONOS_CORE_LIBRARY_VERSION);
				if (stubs) Import(stubs); else std::cerr << "Did not find kernel builtin definitions\n";
				if (prelude) Import(prelude); else std::cerr << "Did not find Prelude\n";
			}

			DocumentContext(const DocumentContext& s):srv(s.srv) {
				assert(document.empty());
			}

			static bool Probe(const std::string& doc) {
				FILE *test = fopen(doc.c_str(), "rb");
				if (test) { fclose(test); return true; }
				else return false;
			}

			std::string GetTokenAt(const char *pos) {
				using lithe::grammar::kronos::istokenchar;
                
                if (pos < document.data() || pos >= document.data() + document.size()) return "???";
                
				auto b = pos, e = pos;
				while (b > document.data() && istokenchar(b[-1])) --b;
				while (e < document.data() + 1 + document.size() && istokenchar(e[1])) ++e;

				if (*b == '\'') ++b;

				return std::string(b, e + 1);
			}

			static const char *TokenStart(const AstNode& n) {
				for (auto i = n.children.begin(); i != n.children.end(); ++i) {
					auto te = TokenStart(*i);
					if (te) return te;
				}
				return n.strend ? n.strbeg : nullptr;
			}

			static const char *TokenEnd(const AstNode& n) {
				for (auto i = n.children.rbegin(); i != n.children.rend(); ++i) {
					auto te = TokenEnd(*i);
					if (te) return te;
				}
				return n.strend;
			}

			void BindTarget(SymbolData& sd, AstNode& tuple) {
				using namespace lithe::grammar::kronos;
				if (tuple.strbeg == tag::tuple || tuple.strbeg == tag::list) {
					for (int i = 0;i < tuple.children.size();++i) {
						BindTarget(sd, tuple[i]);
					}
				} else {
					sd.Define(tuple.get_string(), CompletionData{ Symbol, "", "", "local variable", tuple.strbeg });
				}
			}

			void Import(std::string path) {
#ifdef WIN32
				for (auto &c : path) if (c == '\\') c = '/';
#endif
				if (imports.count(path) == 0) {
					imports.emplace(path, &srv.Get("file:///" + path));
				}
			}

			void AnalyzeScope(SymbolData& sd, AstNode& pack, int start) {
				using namespace lithe::grammar::kronos;
				for(int i = start; i<pack.children.size(); ++i) {
					auto &t = pack.children[i];
					if (t.strbeg == tag::import) {
						if (t[0].strbeg == tag::lstring) {
                            //  TODO: in-package relative imports not implemented
							Import(t[0][0].get_string());
                        } else if (t[0].strbeg == tag::package) {
                            auto repo = t[0][0].get_string();
                            auto ver = t[0][1].get_string();
                            std::string file = "main.k";
                            if (t[0].children.size() > 2) file = t[0][2].get_string();
                            Import(bbClient->Resolve(repo, file, ver));
						} else {
                            Import(bbClient->Resolve(KRONOS_CORE_LIBRARY_REPOSITORY, 
													 t[0].get_string() + ".k", 
													 KRONOS_CORE_LIBRARY_VERSION));
						}
					} else if (t.strbeg == tag::package) {
						auto &subPack = sd.children[t[0].strbeg];
						subPack.end = TokenEnd(t);
						subPack.inNamespace = sd.inNamespace + t[0].get_string() + ":";
						subPack.uses = sd.uses;
						subPack.uses.emplace_back(subPack.inNamespace);
						AnalyzeScope(subPack, t, 1);
						sd.defines.insert(subPack.defines.begin(), subPack.defines.end());
						analysis.Define(subPack.inNamespace, CompletionData{ Package, "", "", "Package", t[0].strbeg });
					} else if (t.strbeg == tag::defn) {
						auto& subPack = sd.children[TokenEnd(t[0])];
						subPack.end = TokenEnd(t[1]);
						if (subPack.end) {
							// not empty
							while (*subPack.end != '}') subPack.end++;
							subPack.inNamespace = sd.inNamespace;
							subPack.uses = sd.uses;
							AnalyzeScope(subPack, t[1], 0);

							std::string doc;

                            if (t[1].children.back().strbeg == tag::docstring)
                            for (auto& docnode : t[1].children.back().children) {
								auto nd = docnode.get_string();
								if (!doc.empty()) {
									if (nd == " ") doc += "\n\n";
									else doc += " ";
								}
								doc += nd;
							}
							std::stringstream argList, argSnippet;
							for (int i = 0;i < t[0][1].children.size();++i) {
								if (i) {
									argList << " "; argSnippet << " ";
								}
								argSnippet << "${" << i + 1 << ":" << t[0][1][i].get_string() << "}";
								argList << t[0][1][i].get_string();
								subPack.Define(t[0][1][i].get_string(), CompletionData{ Symbol, "", "",
														"function parameter for `" + t[0][0].get_string() + "`",  t[0][1][i].strbeg });
							}
							analysis.Define(sd.inNamespace + t[0][0].get_string(),
													 CompletionData{ Function, argList.str(), argSnippet.str(), doc, t[0][0].strbeg });
						}
					} else if (t.strbeg == tag::infix && t[1].get_string() == "=") {
						BindTarget(sd, t[0]);
					} else if (t.strbeg == tag::use) {
						for (size_t i = 0, e = sd.uses.size();i < e;++i) {
							sd.uses.emplace_back(sd.uses[i] + t[0].get_string() + ":");
						}
					} else if (t.strbeg == tag::type) {
						sd.Define(sd.inNamespace + t[0].get_string(), CompletionData{ Type, "", "", "Type identifier", t[0].strbeg });
					}
				}
			}

			std::vector<std::pair<const char*, std::string>> diagnostics;
			std::vector<std::pair<const char*, std::string>> sent_diagnostics;

			void Analyze(const std::string& document) {
				auto grammar = lithe::grammar::kronos::parser();
				auto tokens = grammar->parse(document);

				if (tokens.is_error()) {
					diagnostics.emplace_back(tokens[0].strbeg, tokens.get_string());
				} else {
					analysis = SymbolData{};
					imports.clear();
					diagnostics.clear();
					AnalyzeScope(analysis, tokens, 0);
				}
			}

			bool FindIn(const char *pos, const std::string& text, int& row, int& column) {
				if (pos < text.data() || pos > text.data() + text.size()) return false;
				row = 0;
				column = 0;
				for (const char *seek = text.data(); seek < pos; ++seek) {
					if (*seek == '\n') {
						++row;
						column = 0; 
					} else {
						++column;
					}
				}
				return true;
			}

			bool LocatePosition(const char *pos, std::string& uri, int& row, int &column) {
				if (FindIn(pos, document, row, column)) {
					uri = documentUri;
					return true;
				}

				for (auto &doc : srv.documents) {
					uri = doc.second->documentUri;
					if (FindIn(pos, doc.second->document, row, column)) return true;
				}
				return false;
			}

			const char *GetPosition(int row, int column) {
				if (row >= 0 && row < documentLines.size()) {
					auto pos = documentLines[row] + column;
					if (pos >= document.data() + document.size()) return nullptr;
					return pos;
				}
				return nullptr;
			}

			std::string SlurpFile(const std::string& filePath) {
				std::ifstream file(filePath);
				std::ostringstream doc;
				doc << file.rdbuf();
				return doc.str();
			}

			void Update(const std::string& newDoc, const char *start, const char *end) {
				auto startOffset = start - document.data();
				auto endOffset = end - document.data();
				Update(document.substr(0, startOffset) + newDoc + document.substr(endOffset));
			}

			void Update(const std::string& newDoc) {
				document = newDoc;
				Analyze(document);
				analysis.end = document.data() + document.size();

				documentLines.clear();
				documentLines.emplace_back(document.data());

				for (size_t feed = 0ull; (feed = document.find('\n', feed)) != std::string::npos;)
					documentLines.emplace_back(document.data() + ++feed);
			}

			void Load(const std::string& filePath) {
                std::string fileScheme = "file:///";
                if (filePath.compare(0, fileScheme.size(), fileScheme) == 0) {
                    Update(SlurpFile(filePath.substr(fileScheme.size())));
                }
			}


			using CycleSet = std::unordered_set<DocumentContext*>;
			void GatherDefinitions(const std::vector<std::string>& sym, std::vector<const char*>& defs, CycleSet& visited) {
				if (visited.count(this)) return;
				visited.emplace(this);
				analysis.GatherGlobalDefinitions(sym, defs);
				for (auto i : imports) {
					i.second->GatherDefinitions(sym, defs, visited);
				}
			}

			void GatherDefinitions(const char *pos, const std::string& sym, std::vector<const char*>& defs) {
				CycleSet init;
				analysis.GatherLocalDefinitions(pos, sym, defs);
				GatherDefinitions(analysis.GetPossibleQualifiedNames(pos,sym), defs, init);
			}


			SymbolData::CompletionRecord GetHoverInfo(const std::vector<std::string>& tok, CycleSet& visited) {
				if (visited.count(this)) return {};
				visited.emplace(this);

				SymbolData::CompletionRecord cr = analysis.GetGlobalHoverInfo(tok);				

				for (auto i : imports) {
					if (!cr.first.empty()) return cr;
					cr = i.second->GetHoverInfo(tok, visited);
				}
				return cr;
			}

			SymbolData::CompletionRecord GetHoverInfo(const char *pos, std::string tok) {
				auto cr = analysis.GetLocalHoverInfo(pos, tok);
				if (cr.first.empty() == false) return cr;
				CycleSet init;
				return GetHoverInfo(analysis.GetPossibleQualifiedNames(pos, tok), init);
			}

			void GatherCompletions(const std::string& package, const std::string& prefix, const std::vector<std::string>& uses, std::set<SymbolData::CompletionRecord>& completions, CycleSet& visited) {
				if (visited.count(this)) return;
				visited.emplace(this);
				analysis.GatherGlobalCompletions(package, prefix, uses, completions);
				for (auto i : imports) {
					i.second->GatherCompletions(package, prefix, uses, completions, visited);
				}
			}

			std::string GetPrefix(const char *pos) {
				const char *beg = pos, *end = pos;
				using lithe::grammar::kronos::istokenchar;
				while (pos > document.data() && istokenchar(beg[-1])) --beg;
				return { beg, end };
			}

			std::string GatherCompletions(const char *pos, std::set<SymbolData::CompletionRecord>& completions) {
				CycleSet init;
				std::vector<std::string> uses;
				auto prefixC = GetPrefix(pos);
				std::string package;
				std::string prefix = prefixC;

				if (auto lastColon = prefixC.find_last_of(":")) {
					if (lastColon != prefixC.npos) {
						prefixC = prefixC.substr(0, lastColon + 1);
						package = prefixC;
						prefix = prefix.substr(lastColon + 1);
					}
				}

				for (auto& c : prefix) c = tolower(c);

				analysis.GatherLocalCompletions(pos, package, prefix, completions, uses);
				GatherCompletions(package, prefix, uses, completions, init);

				return prefixC;
			}
		};

		using JsonRPCMethod = std::function<picojson::value(const picojson::value&)>;

		template <typename EP>
		static void InjectMembers(EP& endpoint, std::shared_ptr<DocumentContext::ServerInstance> srv) {
			endpoint["initialize"] = [](const picojson::value& v) {
				return object{
					{ "capabilities", object{
						{ "completionProvider", object{
							{ "triggerCharacters", ":" },
							{ "resolveProvider", true }
						} },
						{ "definitionProvider", true },
						{ "hoverProvider", true },
						{ "textDocumentSync", 2.0 }
					} }
				};
			};

			endpoint["textDocument/definition"] = [srv](const picojson::value& v) {
				const char *pos;
				auto& docCx = srv->Get(v, pos);

				std::vector<const char*> definitions;
				docCx.GatherDefinitions(pos, docCx.GetTokenAt(pos), definitions);

				picojson::array defs;
				for (auto &d : definitions) {
					std::string uri;
					int row, column;
					if (docCx.LocatePosition(d, uri, row, column)) {
						defs.emplace_back(object{
							{ "uri", uri },
							{ "range", object {
								{ "start", object { {"line", (double)row} , {"character", (double)column} } },
								{ "end", object{ { "line", (double)row },{ "character", (double)column } } }
							} }
						});
					}
				}
				return defs;
			};

			endpoint["textDocument/hover"] = [srv](const picojson::value& v) -> picojson::value {
				const char *pos;
				auto& docCx = srv->Get(v, pos);
				auto hoverInfo = docCx.GetHoverInfo(pos, docCx.GetTokenAt(pos));

				if (hoverInfo.first.empty()) return picojson::value{};

				std::stringstream md;
				auto name = hoverInfo.second.qualifiedName;
				if (name.size() && name.front() == ':') name = name.substr(1);
				switch (hoverInfo.second.kind) {
				default:
					md << "### " << name << " ###";
					break;
				case Function:
					md << "### " << name << "( " << hoverInfo.second.argList << " ) ###\n";
					break;
				}

				if (hoverInfo.second.doc.empty() == false) {
					md << "\n" << hoverInfo.second.doc;
				}

				return object{ {
					{"contents", md.str() }
				} };
			};

			endpoint["textDocument/completion"] = [srv](const picojson::value& v) {
				const char *pos;
				auto& docCx = srv->Get(v, pos);
				std::set<SymbolData::CompletionRecord> completions;
				auto prefix = docCx.GatherCompletions(pos, completions);

				picojson::array completionItems;
				for (auto &c : completions) {

					auto sym = c.first;
					auto lastColon = sym.find_last_of(':');
					if (lastColon != std::string::npos) {
						sym = sym.substr(lastColon + 1);
					}

					switch (c.second.kind) {
					default:
						completionItems.emplace_back(object{
							{ "label", c.first },
							{ "kind", (double)c.second.kind },
							{ "sortText", c.first }
						});
						break;
					case Function:
						completionItems.emplace_back(object{
							{ "label", c.first },
							{ "kind", (double)c.second.kind },
							{ "insertText", c.first + "(" + c.second.argSnippet + ")" },
							{ "insertTextFormat", 2.0 },
							{ "sortText", sym },
							{ "documentation", c.second.doc }
						});
						break;
					}

					switch (c.second.kind) {
					case Function:
						completionItems.back().get<object>()["commitCharacters"] =
							picojson::array{"(", "["};
						break;
					case Package:
						completionItems.back().get<object>()["commitCharacters"] =
							picojson::array{ ":"};
						break;
					default:
						break;
					}
				}
				return completionItems;
			};

			endpoint["textDocument/didOpen"] = [srv](const picojson::value& v) {
				auto &docCx = srv->Get(v.get("textDocument"));
				if (v.contains("text")) {
					docCx.Update(v.get("text").to_str());
				}
				return picojson::value{};
			};

			endpoint["initialized"] = [](const picojson::value&) {
				return picojson::value{};
			};

			endpoint["textDocument/didChange"] = [&endpoint, srv](const picojson::value& v) {
				auto &docCx = srv->Get(v.get("textDocument"));
				auto changes = v.get("contentChanges").get<picojson::array>();
				for (const auto& change : changes) {
					auto update = change.get("text").get<std::string>();
					if (change.contains("range")) {
						auto range = change.get("range");
						auto startRow = (int)range.get("start").get("line").get<double>();
						auto startCol = (int)range.get("start").get("character").get<double>();
						auto endRow = (int)range.get("end").get("line").get<double>();
						auto endCol = (int)range.get("end").get("character").get<double>();
						
						auto start = docCx.GetPosition(startRow, startCol);
						auto end = docCx.GetPosition(endRow, endCol);

						docCx.Update(update, start, end);
					} else {
						docCx.Update(update);
					}
				}


				if (!std::equal(docCx.diagnostics.begin(), docCx.diagnostics.end(),
							   docCx.sent_diagnostics.begin(), docCx.sent_diagnostics.end())) {
					docCx.sent_diagnostics = docCx.diagnostics;
					endpoint.ClearNotify(docCx.documentUri);
					while (docCx.diagnostics.size()) {
						auto diag = docCx.diagnostics.back();
						docCx.diagnostics.pop_back();

						std::string uri; int row, column;
						if (docCx.LocatePosition(diag.first, uri, row, column)) {
							auto pos = object{
								{ "line", (double)row}, { "character", (double)column }
							};
							endpoint.PushNotify(docCx.documentUri, picojson::object{
								{ "range", object {
									{ "start", pos },
									{ "end", pos }
								} },
								{ "source", GetProcessFileName() },
								{ "message", diag.second }
												});
						}
					}
				}

				return picojson::value();
			};

			endpoint["shutdown"] = [](const picojson::value& v) {
				return picojson::object();
			};

			endpoint["exit"] = [](const picojson::value&) -> picojson::object {
				exit(0);
			};
		}

		JsonRPC::IEndpoint::Ref Make(const char *repository, const char *repositoryVersion) {
			struct LangSrvEndpoint : public JsonRPC::Endpoint {
				std::unordered_map<std::string, std::vector<picojson::value>> ServerNotify;

				void PushNotify(const std::string& uri, const picojson::object& diag) {
					ServerNotify[uri].emplace_back(diag);
				}

				void ClearNotify(const std::string& uri) {
					ServerNotify[uri] = {};
				}

				bool GetPendingMessage(picojson::value& v) {
					if (ServerNotify.empty()) return false;

					auto sn = ServerNotify.begin();

					picojson::object rpc{
						{ "jsonrpc", "2.0" },
						{ "method", "textDocument/publishDiagnostics" },
						{ "params", picojson::object {
							{ "uri", sn->first },
							{ "diagnostics", sn->second }
						} }
					};

					v = rpc;

					ServerNotify.erase(sn);
					return true;
				}
			};

            bbClient = std::make_unique<Packages::DefaultClient>();
			auto endpoint = std::make_shared<LangSrvEndpoint>();
			InjectMembers(*endpoint, std::make_shared<DocumentContext::ServerInstance>());
			return endpoint;
		}
	}
}
