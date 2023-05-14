#include "config/system.h"
#include "kronos.h"
#include "driver/package.h"
#include "lithe/grammar/kronos.h"
#include "common/PlatformUtils.h"

#include <iostream>
#include <sstream>
#include <fstream>
#include <map>
#include <set>
#include <filesystem>

using FunctionData = std::map<std::string, std::string>;
using PackageData = std::map<std::string, FunctionData>;
using IndexData = std::map<std::string, std::set<std::string>>;
using FunctionIndex = std::map<std::string, std::pair<std::string, FunctionData>>;
FunctionIndex GlobalSymbols;

std::string krepl;

#ifdef WIN32
#define popen_utf8(CMD) _popen(CMD, "rt,ccs=UTF8")
#define pclose _pclose
#else
#define popen_utf8(CMD) popen(CMD, "r")
#endif


namespace md {
	const char *link = "link";
	const char *autolink = "autolink";
	const char *siblinglink = "siblink";
	const char *header = "header";
	const char *paragraph = "p";
	const char *img = "img";
	const char *code = "code";
	const char *code_syntax = "lang";
	const char *reflink = "ref";
	const char *hashtag = "#";
}

static lithe::rule mini_markdown() {
	using namespace lithe;
	using namespace lithe::grammar::common;
	auto eol = IE("<EOL>", O(T("\r")) << T("\n"));
	auto space = I(characters("whitespace", isspace));
	auto empty_line = IE("<empty line>", O(characters("space", " \t")) << eol);
	auto any_line = characters("line", "\n", true, 0) << eol;
	auto header_level = characters("header_level", "#");
	auto header_title = characters("header_title", "\n#", true);
	auto header_space = characters("space", isspace);

	auto header = E(md::header, header_level << space << header_title << I(any_line));

	auto text = characters("body text", isspace, true);
	auto in_square = I("[") << characters("text", "\n]", true, 0) << I("]");
	auto in_round = I("(") << characters("text", "\n)", true, 0) << I(")");
	auto link = E(md::link, in_square << (in_round | I("[]")));
	auto autolink = E(md::autolink, I("<") << characters("url", "\n>", true) << I(">"));
	auto sibling_link = E(md::siblinglink, I("[") << in_square << I("]"));
	auto image = E(md::img, I("!") << in_square << in_round);
	auto code = E(md::code, I("`") << characters("code", "\n`", true) << I("`"));
	auto reflink = E(md::reflink, in_square << I(":") << characters("url","\n",true) << I(eol));
	auto emph = E("em", I("*") << characters("emphasized", "*\n", true) << I("*"));
	auto hashtag = E(md::hashtag, I("#") << characters("hashtag", isspace, true));

	auto para_span = code | image | sibling_link | autolink | link | emph | hashtag | text | characters("space", " \t");
	auto para_line = repeat(para_span) << eol;
	auto paragraph = E(md::paragraph, repeat(para_line) << I(repeat(empty_line)));

	auto codeblock_delimiter = IE("code boundary", T("```"));
	auto codeblock = E(md::code, I(codeblock_delimiter << any_line) << O(E(md::code_syntax, I("#!") << characters("lang","\n",true))) << I(eol) << 
		for_(
			any_line, 
			{},
			I(codeblock_delimiter)));
		
	return repeat(IE("entity", empty_line | reflink | header | codeblock | paragraph)) << end();
}

namespace fs = std::experimental::filesystem;

fs::path websitePath;

static void WalkDir(const fs::path& path, std::set<fs::path>& files) {
	for (auto &p : fs::recursive_directory_iterator(path)) 
		if (fs::is_regular_file(p)) files.emplace(p);
}

static void ReadPackage(PackageData& s, std::string path, lithe::node& pack) {
	using namespace lithe::grammar::kronos;
	for (auto &n : pack.children) {
		if (n.strbeg == tag::defn) {
			if (n[1].children.size()) {
				std::string argList;
				for (auto& a : n[0][1].children) {
					if (argList.size()) argList.push_back(' ');
					argList += a.get_string();
				}

				if (n[1].children.size() && n[1].children.back().strbeg == tag::docstring && n[1].children.back().size()) {
					auto &doc(s[path + n[0][0].get_string()][argList]);
					for (auto &d : n[1].children.back().children) {
						if (doc.size()) doc.push_back('\n');
						doc += d.get_string();
					}
				}

			}
		} else if (n.strbeg == tag::package) {
			ReadPackage(s, path + n[0].get_string() + ":", n);
		}
	}
}

std::vector<std::string> Segment(std::string qn) {
	std::vector<std::string> ns;
	for (auto c : qn) {
		switch (c) {
		default: ns.back().push_back(c); break;
		case ':': ns.emplace_back();
		}
	}
	return ns;
}

static void WritePackageMarkdown(std::ostream& s, const PackageData& pd) {
	std::vector<std::string> ns;
	for (auto &fd : pd) {
		auto qn = Segment(fd.first);
		for (int i = 0;i < qn.size();++i) {
			if (ns.size() <= i) {
				s << "## ";
				while (i < qn.size()) {
					s << ":" << qn[i];
					ns.emplace_back(qn[i++]);
				}
				s << " ##\n";
				break;
			} else if (qn[i] != ns[i]) {
				while (ns.size() >= i) ns.pop_back();
			}
		}

		for (auto &form : fd.second) {
			s << "### " << fd.first << "(" << form.first << ") ###\n\n";
			s << form.second << "\n\n";
		}
	}
}

static std::string HTMLText(const std::string& raw) {
	std::string result;
	result.reserve(raw.size());
	for (auto c : raw) {
		switch (c) {
			default: result.push_back(c); break;
			case '&': result += "&amp;"; break;
			case '<': result += "&lt;"; break;
			case '>': result += "&gt;"; break;
			case '\"': result += "&quot;"; break;
			case '\'': result += "&#39;"; break;
		}
	}
	return result;
}

static std::string URLEncode(const std::string& raw) {
	std::string result;
	result.reserve(raw.size());
	char percentEncode[4];

	auto proto = raw.find("://");
	if (proto != raw.npos) {
		return raw.substr(0, proto) + "://" + URLEncode(raw.substr(proto + 3));
	}

	for (unsigned char c : raw) {
		if (isalnum(c)) result.push_back(c);
		else {
			switch (c) {
			case '\\': result.push_back('/'); break;
			case '-': case '_': case '.': case '~': case '#': case '/':
				result.push_back(c); break;
			default:
				sprintf(percentEncode, "%%%02x", (unsigned char)c);
				result += percentEncode;
				break;
			}
		}
	}
	return result;
}

struct Toc {
	std::map<std::string, Toc> children;
	void Place(std::string path) {
		auto slash = path.find('/');
		if (slash != path.npos) {
			children[path.substr(0, slash)].Place(path.substr(slash + 1));
		} else {
			children[path];
		}
	}

	void Write(const std::string& linkbase, const std::string& linkname, std::ostream& s) const {
		if (children.size()) {
			s << "<a href='" << URLEncode(linkbase) << URLEncode(linkname) << "/index.html'>" << HTMLText(linkname) << "</a>";
			s << "<ul>";
			for (auto &c : children) {
				s << "<li>";
				c.second.Write(linkbase + linkname + "/", c.first, s);
				s << "</li>";
			}
			s << "</ul>";
		} else {
			s << "<a href='" << URLEncode(linkbase) << URLEncode(linkname) << ".html'>" << HTMLText(linkname) << "</a>";
		}
	}

	void GenerateIndices(const fs::path& fsPath, const std::string& name, int depth, const Toc& master, const IndexData& masterTags) const;
};


static void Index(const std::string& text, const std::string& anchor, IndexData& index) {
	std::string indexWord;
	for (unsigned char c : text) {
		if (!isalpha(c)) {
			if (indexWord.size() > 2) {
				index[indexWord].emplace(anchor);
			}
			indexWord.clear();
		} else indexWord.push_back(tolower(c));
	}

	if (indexWord.size() > 2) index[indexWord].emplace(anchor);
}

std::string Resolve(std::string symbol, const std::list<std::string>& Using) {
	if (GlobalSymbols.count(symbol)) return symbol;
	for (auto &u : Using) {
		auto qn = u + symbol;
		auto sym = GlobalSymbols.find(qn);
		if (sym != GlobalSymbols.end()) {
			return sym->second.first + "#Section:" + qn.substr(1) + "(" + sym->second.second.begin()->first + ")";
		}
	}
	return "";
}

static void FindNodeText(const lithe::node& n, const char *& begin, const char *& end) {
	if (!n.is_error() && n.strend) {
		if (!begin || begin > n.strbeg) begin = n.strbeg;
		if (!end || end < n.strend) end = n.strend;
	}
	for (auto &c : n.children) {
		FindNodeText(c, begin, end);
	}
}

struct {
	std::string code;

	void Reset() {
		code.clear();
	}

	std::string Evaluate(std::string immediate) {
		char tmpfile[L_tmpnam];
		tmpnam(tmpfile);
		std::ofstream dumpCode(tmpfile);
		dumpCode << code;
		dumpCode.close();

#ifdef WIN32
		// escape for windows cmd and createprocess
		// spoiler alert: it's kinda dumb.
		std::string escaped;
		int in_whitespace = 0;
		std::string cmd_escape = "()%!^\\<>&|";
		for (auto c : immediate) {
			auto tmp = isspace(c);
			
			if (tmp) c = ' '; // change whitespace to space

			if (tmp != in_whitespace) {
				escaped += "^\"";
				in_whitespace = tmp;
			}
			if (c == '\"') {
				escaped += "^\\^";
			} else if (cmd_escape.find(c) != cmd_escape.npos) {
				escaped += '^';
			}
			escaped += c;
		}
		if (in_whitespace) escaped += "^\"";
		immediate = escaped;
#else
		for (auto c : immediate) {
			if (isspace(c)) c = ' ';
			switch (c) {
			case '\\': escaped.append("\\\\"); break;
			case '\'': escaped.append("\\\'"); break;
			default: escaped.append(c);
			}
		}
		immediate = "\'" + escaped + "\'";
#endif

		std::string command = krepl + " -i \"" + tmpfile + "\" " + immediate;
		auto repl = popen_utf8(command.c_str());
		if (repl) {
			std::string response;
			char buffer[1024];
			while (fgets(buffer, sizeof(buffer), repl)) {
				response += buffer;
			}
			if (pclose(repl) == 0) {
				remove(tmpfile);
				return response;
			}
		}
		remove(tmpfile);
		throw std::runtime_error("Execution of '" + immediate + "' failed");
	}

} REPLState;

static std::string HTMLEncode(std::string input) {
	std::string out;
	for (auto c : input) {
		switch (c) {
		default: out.push_back(c); break;
		case '<': out.append("&lt;"); break;
		case '>': out.append("&gt;"); break;
		case '&': out.append("&amp;"); break;
		}
	}
	return out;
}

static void KronosNodeToHtml(std::ostream& os, const lithe::node& n, std::list<std::string>& Using, int nestingDepth, bool mark_span = false) {
	using namespace lithe::grammar::kronos::tag;
	if (n.strbeg) {
		if (n.strend) {
			if (mark_span) os << "<span class='kronos-syntax token'>";
			auto qn = Resolve({ n.strbeg, n.strend }, Using);

			if (qn.size()) {
				for (int i = 0;i < nestingDepth;++i) qn = "../" + qn;
				os << "<a class='kronos-syntax symbol-reference' href='" << URLEncode(qn) << "'>";
			}
			os << HTMLEncode({n.strbeg, n.strend});
			if (qn.size()) {
				os << "</a>";
			}
			if (mark_span) os << "</span>&#8203;";
		} else {
			std::string cls;
			std::string braces = "";
			if (n.strbeg == tuple) {
				cls = "tuple"; braces = "()";
			} else if (n.strbeg == list) {
				cls = "list"; braces = "[]";
			} else if (n.strbeg == body) {
				cls = body; braces = "{}";
			} else if (n.strbeg == leftarrow) cls = "leftarrow";
			else if (n.strbeg == infix) {
				if (n[1].get_string() == "=") cls = "def";
				else cls = "infix";
			} else cls = n.get_header();
		
			for (auto &c : cls) {
				if (!isalnum(c)) c = '-';
				c = tolower(c);
			}

			os << "<span class='kronos-syntax " << cls << "'>";
			if (n.strbeg == import) os << "<span class='kronos-syntax keyword'>Import </span>";
			if (n.strbeg == use) {
				auto newUses = Using;
				for (auto &p : Using) newUses.emplace_back(p + n[0].get_string() + ":");
				std::swap(newUses, Using);
					
				os << "<span class='kronos-syntax keyword'>Use </span>";
			}
			if (braces.size()) os << braces[0] << "<span>";
			for (auto &c : n.children) KronosNodeToHtml(os, c, Using, nestingDepth, n.children.size() > 1);
			if (braces.size()) os << "</span>" << braces[1];
			os << "</span>";
		}
	} else {
		for (auto &c : n.children) {
			KronosNodeToHtml(os, c, Using, nestingDepth, n.children.size() > 1);
			using namespace lithe::grammar::kronos;
			if (c.strbeg == tag::function ||
				c.strbeg == tag::infix ||
				c.strbeg == tag::list ||
				c.strbeg == tag::tuple ||
				c.strbeg == tag::lstring) {

				const char *begin = nullptr, *end = nullptr;
				FindNodeText(c, begin, end);
				if (begin && end) {
					while (*end && *end != '\n') ++end;
					std::string code = std::string{ begin, end };
					auto output = REPLState.Evaluate(code);
					os << "<span class='kronos-syntax output'>" << HTMLEncode(output) << "</span>";
				}
			}
		}
	}
}

static void KronosCodeToHTML(std::ostream& os, const lithe::node& ast, int nestingDepth) {

	std::stringstream src;
	for (int i = 1;i < ast.children.size();++i) {
		src << ast[i].get_string();
	}

	static auto grammar = lithe::grammar::kronos::parser();
	auto text = src.str();
	auto parsed = grammar->parse(text);
	if (parsed.is_error()) throw std::runtime_error("Syntax error in Kronos code block!");

	std::list<std::string> Using{ ":" };

	os << "<code class='kronos' data-lang='kronos'>";
	KronosNodeToHtml(os, parsed, Using, nestingDepth);
	os << "</code>";
}

static void ToHTML(std::ostream& os, std::string& anchor, const lithe::node &ast, std::set<std::string>& tags, IndexData& index, int nestingDepth) {
	if (ast.strbeg == md::header) {
		std::stringstream title;
		for (int i = 1;i < ast.size();++i) {
			ToHTML(title, anchor, ast[i], tags, index, nestingDepth);
		}
		auto titleStr = HTMLText(title.str());
		while (isspace(titleStr.back())) titleStr.pop_back();
		std::string tag = "h" + std::to_string(ast[0].strend - ast[0].strbeg);
		if (anchor.size()) os << "</div>";
		anchor = "Section:" + titleStr;
		os << "<div id='" << anchor << "'>";
		os << "<" << tag << ">" << titleStr << "</" << tag << ">";
	} else if (ast.strbeg == md::link) {
		switch (ast.size()) {
		case 2:
			os << "<a href='" << URLEncode(ast[1].get_string()) << "'>" << HTMLText(ast[0].get_string()) << "</a>";
			break;
		case 1:
			os << "<a href='#Reference:" << URLEncode(ast[0].get_string()) << "'>" << HTMLText(ast[0].get_string()) << "</a>";
			break;
		default:
			throw std::runtime_error("malformed link");
		}
	} else if (ast.strbeg == md::reflink) {
		os << "<div class='reference-link' id='Reference:" << ast[0].get_string() << "'>";
		os << "<span class='reference-title'>" << HTMLText(ast[0].get_string()) << "</span> ";
		os << "<a href='" << URLEncode(ast[1].get_string()) << "'>" << ast[1].get_string() << "</a></div>";
	} else if (ast.strbeg == md::siblinglink) {
		os << "<a href='" << URLEncode(ast[0].get_string()) << ".html'>" << ast[0].get_string() << "</a>";
	} else if (ast.strbeg == md::code) {
		int i = 0;
		std::string synattr = "";
		if (ast.children.front().strbeg == md::code_syntax) {
			synattr = " data-lang='" + ast[0][0].get_string() + "'";
			i = 1;
		}

		if (ast[0][0].get_string() == "Kronos") {
			const char *begin = nullptr, *end = nullptr;
			FindNodeText(ast, begin, end);
			if (begin && end) {
				while (*end && *end != '\n') ++end;
				REPLState.code += std::string{ begin, end };
			}
			KronosCodeToHTML(os, ast, nestingDepth);
		} else {
			os << "<code" << synattr << ">";
			while (i < ast.children.size()) {
				ToHTML(os, anchor, ast[i++], tags, index, nestingDepth);
			} os << "</code>";
		}
	} else if (ast.strbeg == md::hashtag) {
		os << "<a class='hashtag' href='/Tags/" << URLEncode(ast[0].get_string()) << ".html'>" << ast[0].get_string() << "</a> ";
		tags.emplace(ast[0].get_string());
	} else { 
		if (ast.strbeg) {
			if (ast.strend == nullptr) {
				os << "<" << ast.strbeg << ">";
			} else {
				std::string text{ ast.strbeg, ast.strend };
				Index(text, anchor, index);
				os << HTMLText(text);
			}
		}
		
		for (auto &c : ast.children) ToHTML(os, anchor, c, tags, index, nestingDepth);

		if (ast.strbeg && ast.strend == nullptr) {
			os << "</" << ast.strbeg << ">";
		} 
	}
}

std::map<fs::path, std::string> Site;

static std::string MarkdownToHtml(const std::string& sourceString, IndexData& index, std::set<std::string>& tags, int nestingDepth) {
	static lithe::rule md = mini_markdown();
	std::stringstream write;
	auto tokens = md->parse(sourceString);
	if (tokens.is_error()) throw std::runtime_error(tokens.get_string());

	std::string anchor = "top";
	write << "<div id='top'>";
	ToHTML(write, anchor, tokens, tags, index, nestingDepth);
	write << "</div>";
	return write.str();
}

static void Render(fs::path dstFile, fs::path srcFile, IndexData& index, IndexData& tags) {
	std::ifstream infileStream(srcFile);
	std::stringstream source;
	source << infileStream.rdbuf();
	auto sourceString = source.str();

	std::set<std::string> myTags;
	IndexData myIndex;

	auto nestingDepth = std::distance(dstFile.begin(), dstFile.end()) - 1;

	REPLState.Reset();
	Site[dstFile] = MarkdownToHtml(source.str(), myIndex, myTags, nestingDepth);

	for (auto &t : myTags) {
		tags[t].emplace(dstFile.u8string());
	}

	for (auto &i : myIndex) {
		for (auto &a : i.second)
			index[i.first].emplace(dstFile.generic_u8string() + "#" + a);
	}
}

static void FromTemplate(std::ostream& write, int nestingDepth, const std::string& content, const Toc& toc, const IndexData& tags) {
	write << "<html><head>";
	for (int i = nestingDepth; i>=0; --i) {
		write << "<link rel='stylesheet' href='";
		for (int j = 0;j < i;++j) write << "../";
		write << "style.css'>";
	}
	write << "</head><body>";

	write << "<div class='nav'>";

	std::string basePath;
	for (int i = 0;i < nestingDepth;++i) basePath += "../";

	if (basePath.empty()) basePath = ".";
	else basePath.pop_back();

	write << "<div class='toc'>";
	toc.Write(basePath, "", write);
	write << "</div>";

	write << "<div class='tags'>";
	for (auto t : tags) {
		write << "<a class='tag' href='" << basePath << "/Tags/" << URLEncode(t.first) << ".html'>"
			<< HTMLText(t.first) << "</a>";
	}
	write << "</div>";

	write << "</div><div class='content'>";

	write << content;

	write << "</div></body></html>";
}

int main(int argn, const char* carg[]) {
	using namespace Kronos;

	if (argn < 2) {
		std::clog << "Usage: " << carg[0] << " /path/to/website\n\n";
		return -1;
	}

	websitePath = carg[1];

	krepl = GetProcessFileName();
	auto cmdAt = krepl.rfind("kdocc");
	if (cmdAt == krepl.npos) throw std::runtime_error("Could not deduce 'krepl' path from " + krepl);
	krepl.replace(krepl.begin() + cmdAt, krepl.begin() + cmdAt + strlen("kdocc"), "krepl");

    try {
		Packages::BitbucketClient bbClient;
		for(int i=2;i<argn;++i) {

			auto packageVer = lithe::grammar::kronos::package_version();
			std::string verString = carg[i];
			auto package = packageVer->parse(verString);
			if (package.is_error()) {
				std::clog << "'" << carg[i] << "' is not a valid package identifier\n";
				return -1;
			}

			auto parser = lithe::grammar::kronos::parser();
			auto repo = package[0].get_string(), ver = package[1].get_string();

			std::set<std::string> createdFiles;

			std::clog << "Generating documentation for [" << repo << " " << ver << "] ... ";

			std::unordered_set<std::string> files;
			try {
				files = bbClient.ListFiles(repo, ver);
			} catch (std::exception&) {
				std::clog << "* Network repository not available, fallback to local cache\n";
				std::set<fs::path> paths;
				auto basePath = bbClient.GetLocalFilePath(repo, ver);
				WalkDir(basePath, paths);
				for (auto &p : paths) {
					if (p.extension() == ".k") {
						files.emplace(p.generic_string().substr(basePath.size()));
					}
				}
			}
        
			for(auto &f:files) {
				std::ifstream readFile(bbClient.Resolve(repo, f, ver));
				if (readFile.is_open()) {
					std::stringstream readSource;
					readSource << readFile.rdbuf();
					auto sourceString = readSource.str();
					auto tokens = parser->parse(sourceString);
					PackageData pd;
					ReadPackage(pd, ":", tokens);

					if (pd.size()) {
						std::ofstream write;
						std::string opened = "";
						for (auto &fd : pd) {
							auto file = fd.first;
							while (file.size() && file.back() != ':') file.pop_back();
							for (auto &c : file) if (c == ':') c = '/';
							file.pop_back();
							std::string repoName = repo;
							for (auto &c : repoName) if (c == '/') c = '.';
							auto filePath = websitePath / "src/Reference" / (repoName + "-" + ver + file + ".md");
							auto refPath = fs::path("Reference") / (repoName + "-" + ver + file + ".html");

							GlobalSymbols.emplace(fd.first, std::make_pair(refPath.u8string(), fd.second));

							if (file != opened) {
								if (write.is_open()) write.close();

								if (createdFiles.count(file) == 0) {
									createdFiles.emplace(file);
									std::clog << "Creating '" << file << "'\n";
									std::experimental::filesystem::remove(filePath);
								}

								opened = file;
								fs::create_directories(filePath.parent_path());
								write.open(filePath, std::ios_base::app);

							}

							for (auto &form : fd.second) {
								write << "## " << fd.first.substr(1) << "(" << form.first << ") ##\n\n";
								write << form.second << "\n\n";
							}
						}
					}
				} else {
					throw std::runtime_error("Could not read '" + f + "'");
				}
			}
			std::clog << "Ok\n";
		}    

		std::set<fs::path> sourceFiles;
		IndexData index, tags;
		auto sourcePath = websitePath / "src";
		WalkDir(sourcePath, sourceFiles);

		Toc toc;

		for (auto &srcFile : sourceFiles) {
			auto destFile = srcFile.generic_u8string().substr(sourcePath.u8string().size() + 1);
			destFile = destFile.substr(0, destFile.find_last_of("."));
			toc.Place(destFile);
			std::clog << "Compiling '" << srcFile << "' -> '" << destFile << ".html' ... ";
			Render(destFile + ".html", srcFile, index, tags);
			std::clog << "Ok\n";
		}

		for (auto &page : Site) {
			auto htmlPath = websitePath / "public" / page.first;
			fs::create_directories(htmlPath.parent_path());
			std::ofstream write(htmlPath);

			int nestingDepth = std::distance(page.first.begin(), page.first.end()) - 1;
			FromTemplate(write, nestingDepth, page.second, toc, tags);
		}

		// auto-generate missing indices
		toc.GenerateIndices(websitePath / "public", "", 0, toc, tags);

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

void Toc::GenerateIndices(const fs::path& fsPath, const std::string& name, int depth, const Toc& master, const IndexData& masterTags) const {
	if (children.size()) {
		auto indexFile = fsPath / "index.html";

		std::stringstream md;
		if (name.empty()) md << "# Contents #\n\n";
		else md << "# Contents of " << name << " #\n\n";
		bool first = true;
		for (auto &c : children) {
			if (c.second.children.size()) {
				if (first) {
					first = false;
					md << (name.empty() ? "## Categories ##\n\n" : "## Subcategories ##\n\n");
				}
				md << "[" << c.first << "](" << c.first << "/index.html)\n\n";
				c.second.GenerateIndices(fsPath / c.first, c.first, depth + 1, master, masterTags);
			} 
		}

		first = true;

		for (auto &c : children) {
			if (!c.second.children.size()) {
				if (first) {
					first = false;
					md << "## Pages ##\n\n";
				}
				md << "[[" << c.first << "]]\n\n";
			}
		}

		if (!fs::exists(indexFile)) {
			IndexData index; std::set<std::string> tags;
			auto mdStr = MarkdownToHtml(md.str(), index, tags, depth);
			std::ofstream gen(indexFile);
			FromTemplate(gen, depth, mdStr, master, masterTags);
		}
	}
}
