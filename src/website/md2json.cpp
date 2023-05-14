#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <regex>

#include <sys/stat.h>
#include <time.h>

#ifdef WIN32
#include <direct.h>
#include <io.h>

#else 
#include <unistd.h>
#define _MAX_PATH 4096
#endif

#include "lithe/lithe.h"
#include "lithe/grammar/common.h"
#include "lithe/grammar/kronos.h"
#include "driver/picojson.h"
#include "driver/package.h"
#include "subrepl.h"

extern struct stat exeStat;

picojson::value attachments = picojson::object{ };

const std::string b64encode(const void* data, const size_t& len);

std::string attach(picojson::value& link, const std::string& path, const std::string& ext, const std::string& mime);

namespace md {
	const char *link = "link";
	const char *autolink = "autolink";
	const char *siblinglink = "siblink";
	const char *h1 = "h1";
	const char *h2= "h2";
	const char *h3= "h3";
	const char *h4= "h4";
	const char *h5= "h5";
	const char *h6= "h6";
	const char *paragraph = "p";
	const char *quote = "blockquote";
	const char *img = "img";
	const char* asset = "asset";
	const char *code = "code.listing";
	const char *codesym = "code.symbol";
	const char *code_syntax = "lang";
	const char *reflink = "ref";
	const char *hashtag = "a.hashtag";
	const char* raw_html = "html";
}

static lithe::node consolidate_paragraph(lithe::node e) {
	lithe::node consolidated;
	consolidated.strbeg = e.strbeg;
	consolidated.strend = e.strend;
	consolidated.src_begin = e.src_begin;
	consolidated.src_end = e.src_end;
	for (auto &c : e.children) {
		if (consolidated.children.size() 
			&& c.children.empty()
			&& consolidated.children.back().strend == c.strbeg) {

			consolidated.children.back().strend = c.strend;
		} else {
			consolidated.children.emplace_back(c);
		}
	}
	return consolidated;
}

static lithe::rule mini_markdown() {
	using namespace lithe;
	using namespace lithe::grammar::common;

	auto eol = IE("<EOL>", (O(T("\r")) << T("\n")) | end());
	auto space = I(characters("whitespace", isspace));
	auto empty_line = IE("<empty line>", O(characters("space", " \t")) << eol);
	auto any_line = characters("line", "\n", true, 0) << eol;
	auto header_level = characters("header_level", "#");
	auto header_title = characters("header_title", "\n#", true);
	auto header_space = characters("space", isspace);

	auto head1 = E(md::h1, I("#") << space << header_title << I(any_line));
	auto head2 = E(md::h2, I("##") << space << header_title << I(any_line));
	auto head3 = E(md::h3, I("###") << space << header_title << I(any_line));
	auto head4 = E(md::h4, I("####") << space << header_title << I(any_line));
	auto head5 = E(md::h5, I("#####") << space << header_title << I(any_line));
	auto head6 = E(md::h6, I("######") << space << header_title << I(any_line));

	auto header = head1 | head2 | head3 | head4 | head5 | head6;

	auto span = recursive();
	auto text = characters("text", " \t\r\n()[]", true) | T("(") | T(")") | T("[") | T("]");
	auto in_square = I("[") << characters("text", "\n]", true, 0) << I("]");
	auto in_round = I("(") << characters("text", "\n)", true, 0) << I(")");
	auto image = E(md::img, I("!") << in_square << in_round);
	auto asset = E(md::asset, I("[asset://") << characters("mime", "\n]", true) << I("]") << in_round);
	auto link = E(md::link, 
				  I("[") << 
				  lithe::custom("content",
					for_(span, {}, I("]")), consolidate_paragraph) 
				  << (in_round | I("[]")));
	auto autolink = E(md::autolink, I("<") << characters("url", "\n>", true) << I(">"));
	auto sibling_link = E(md::siblinglink, I("[") << in_square << I("]"));
	auto code = E(md::codesym, I("`") << characters("code", "\n`", true) << I("`"));
	auto reflink = E(md::reflink, in_square << I(":") << characters("url", "\n", true) << I(eol));
	auto emph = E("em", I("*") << characters("emphasized", "*\n", true) << I("*"));
	auto hashtag = E(md::hashtag, I("#") << characters("hashtag", isspace, true));

	span->assign(code | image | asset | sibling_link | autolink | link | emph | hashtag | text | characters("space", " \t"));
	auto para_line = repeat(span) << (eol);
	auto quote_line = I(">") << O(space) << para_line;

	auto paragraph =  lithe::custom("paragraph", 
									  E(md::paragraph, 
										for_(repeat(span), eol, I(empty_line | end()))),
									  consolidate_paragraph);

	auto raw_html = E(md::raw_html, T("<") << characters("line", "\n", true));

	auto quote = lithe::custom("quote",
							   E(md::quote, for_(I(">") << repeat(span), eol, I(empty_line))),
							   consolidate_paragraph);

	auto codeblock_delimiter = IE("code boundary", T("```"));
	auto codeblock = E(md::code, I(codeblock_delimiter << any_line) << O(E(md::code_syntax, I("#!") << characters("lang", "\n", true) << I(eol)))  
					   <<
					   for_(
						   any_line,
						   {},
						   I(codeblock_delimiter)));

	return for_(
		IE("entity", empty_line | reflink | header | codeblock | quote | raw_html | paragraph),
		{},
		end());
}

subrepl_state repl;

picojson::value render_kronos_node(const lithe::node& n, bool span = false) {
	if (n.strbeg) {
		if (n.strend) {
			if (span) {
				return picojson::array{
					"span.kronos-syntax.token",
					std::string{n.strbeg, n.strend}
				};
			}
			return std::string{ n.strbeg, n.strend };
		} else {
			using namespace lithe::grammar::kronos::tag;
			std::string cls, braces;
			if (n.strbeg == tuple) {
				cls = "tuple"; braces = "()";
			} else if (n.strbeg == lstring) {
				cls = "string"; braces = "\"\"";
			} else if (n.strbeg == list) {
				cls = "list"; braces = "[]";
			} else if (n.strbeg == body) {
				cls = body; braces = "{}";
			} else if (n.strbeg == leftarrow) cls = "leftarrow";
			else if (n.strbeg == infix) {
				if (n[1].get_string() == "=") cls = "def";
				else cls = "infix";
			} 
						
			else cls = n.get_header();

			for (auto &c : cls) {
				if (!isalnum(c)) c = '-';
				c = tolower(c);
			}

			picojson::array node{ {"span.kronos-syntax." + cls} };

			if (n.strbeg == import) {
				node.emplace_back(picojson::array{ { "span.kronos-syntax.keyword", "Import " } });
			} else if (n.strbeg == use) {
				node[0] = "Use";
			}

			for (auto &c : n.children) {
				node.emplace_back(render_kronos_node(c, n.children.size() > 1));
			}

			if (cls == "anonymous") node.emplace_back("_");

			if (braces.size()) {
				if (node.size() < 2) node.emplace_back(" ");
				node[0] = "span";
				return picojson::array{ {"span.kronos-syntax." + cls, braces.substr(0,1), node, braces.substr(1,1) } };
			}
			return node;
		}
	} else {
		std::cerr << "Untagged node " << n << "\n";
		exit(-1);
	}
}

static void find_node_boundaries(const lithe::node& n, const char *& begin, const char *& end) {
	if (!n.is_error() && n.strend) {
		if (!begin || begin > n.strbeg) begin = n.strbeg;
		if (!end || end < n.strend) end = n.strend;
	}

	const char *mybeg = nullptr, *myend = nullptr;

	for (auto &c : n.children) {
		find_node_boundaries(c, mybeg, myend);
	}

	if (!mybeg || !myend) return;

	auto grow = [&](const char *tag, const char* delim) {
		if (n.strbeg == tag) {
			do --mybeg; 
			while (mybeg[0] != delim[0]);
			
			do ++myend; 
			while (myend[-1] != delim[1]);
		}
	};

	auto rewind = [&](const char* tag, std::string word) {
		if (n.strbeg == tag) {
			while (strncmp(mybeg, word.data(), word.size())) --mybeg;
		}
	};

	using namespace lithe::grammar::kronos;

	grow(tag::body, "{}");
	grow(tag::lstring, "\"\"");
	grow(tag::tuple, "()");
	grow(tag::list, "[]");

	rewind(tag::import, "Import");
	rewind(tag::use, "Use");
	rewind(tag::type, "Type");
	rewind(tag::invariant, "#");

	if (n.strbeg == tag::function) {
		if (n[1].size() == 0) {
			myend++;
		}
	}


	if (n.strend == tag::import) {
		if (*myend == ']') ++myend;
	}

	if (!begin || mybeg < begin) begin = mybeg;
	if (!end || myend > end) end = myend;
}

void node_to_span(picojson::array& parent, const char *& cursor, lithe::node const & n) {
	using namespace std::string_literals;
	if (n.strend) {
		assert(n.children.empty());
		if (cursor < n.strbeg) {
			parent.emplace_back(picojson::array{ "span.other", std::string{ cursor, n.strbeg } });
		}
		cursor = n.strend;
		parent.emplace_back(std::string{ n.strbeg, n.strend });
	} else {
		using namespace lithe::grammar::kronos;
		picojson::array element;

		std::string classname("span.");

		if (n.strbeg) {
			if (n.strbeg == tag::tuple || n.strbeg == tag::package) classname += "tuple";
			else if (n.strbeg == tag::list) classname += "list";
			else if (n.strbeg == tag::leftarrow) classname += "left-arrow";
			else for (auto cstr = n.strbeg; *cstr; ++cstr) {
				if (isalpha(*cstr)) classname.push_back(*cstr);
				else classname.push_back('-');
			}
		} else {
			classname += "other";
		}

		const char *beg = nullptr, *end = nullptr;
		find_node_boundaries(n, beg, end);

		element.push_back(classname);

		if (beg) {
			if (beg > cursor) {
				parent.push_back(picojson::array{ "span.other", std::string(cursor, beg) });
			}
			cursor = beg;
		}

		
		int operatorIndex = -1;
		if (n.strbeg == tag::infix) {
			operatorIndex = 1;
		} else if (n.strbeg == tag::section) {
			operatorIndex = 0;
		} else if (n.strbeg == tag::sec_left) {
			operatorIndex = 1;
		} else if (n.strbeg == tag::sec_right) {
			operatorIndex = 0;
		} 
		for (int i = 0; i < n.size();++i) {
			if (i == operatorIndex) {
				picojson::array op{ "span.operator" };
				node_to_span(op, cursor, n[i]);
				element.push_back(std::move(op));
			} else {
				node_to_span(element, cursor, n[i]);
			}
		}

		if (end) {
			if (cursor < end) {
			   element.push_back(picojson::array{ "span.other", std::string(cursor, end) });
			}
			cursor = end;
		}

		parent.emplace_back(std::move(element));
	}
}

std::string escape(std::string s) {
	std::string e;
	for (auto c : s) {
		switch (c) {
			default: e.push_back(c); break;
			case '\n':e += "\\n"; break;
			case '\r':e += "\\r"; break;
			case '\t':e += "\\t"; break;
			case '\"':e += "\\\""; break;
			case '\\':e += "\\\\"; break;
		}
	}
	return e;
}


picojson::value render_kronos_code(std::string code) {
	auto ast = lithe::grammar::kronos::parser(true)->parse(code);
	if (ast.is_error()) {
		ast.to_stream(std::cerr);
		std::cerr << std::string(ast[0].strbeg).substr(0, 16) << "...\n";
		exit(-1);
	}

	const char *cursor = code.data();
	while (isspace(*cursor)) ++cursor;

	using namespace lithe::grammar::kronos;

	picojson::array codeblock{ {"code.kronos.listing"} };
	using namespace std::string_literals;

	for (auto &c : ast.children) {
		const char *begin = c.src_begin, *end = c.src_end;
		
		node_to_span(codeblock, cursor, c);

		if (c.strbeg == tag::function ||
			c.strbeg == tag::infix ||
			c.strbeg == tag::list ||
			c.strbeg == tag::tuple ||
			c.strbeg == tag::lstring ||
			c.strend) {

			if (c.strbeg == tag::infix && c[1].get_string() == "=") {
				if (c[0].get_string() == "snd") {
					while (*end && *end != '\n') ++end;

					std::string code{ begin, end };
					auto id = repl.code + code;
					auto tmpfile = "repl_"s + Packages::fnv1a(id.data(), id.size()).to_str() + ".mp3";

					repl.evaluate(
						"Actions:Render(\"" + escape(tmpfile) + "\" 441000 44100 { " + code + " snd } )");

					repl.audio.emplace_back(code);

					auto uid = attach(attachments, tmpfile, ".mp3", "audio/mpeg");
					remove(tmpfile.c_str());
					codeblock.emplace_back(picojson::array{ {"audio", uid} });
					continue;
				}
			} else {
				while (*end && *end != '\n') ++end;
				auto output = repl.evaluate({ begin, end });
				codeblock.emplace_back(picojson::array{ {"samp.kronos-syntax.output", output} });
				repl.eval.emplace_back(std::string{ begin,end });
			}
		}
		auto snippet = " "s + std::string{ begin,end } + "\n";
		repl.code += snippet;
		repl.code += "\n";
	}
	return codeblock;
}

picojson::value convert(const lithe::node& n) {
	picojson::array children;
	children.reserve(n.children.size() + 1);
	if (n.strbeg) {
		if (n.strbeg == md::code && n.size()) {
			if (n[0].strbeg == md::code_syntax && n[0].size()) {
				if (n[0][0].get_string() == "Kronos") {
					std::stringstream code;
					for (int i = 1;i < n.size();++i) {
						n[i].to_stream(code);
					}
					return render_kronos_code(code.str());
				} else if (n[0][0].get_string() == "EmbedHTML") {
					std::stringstream html;
					for (int i = 1; i < n.size(); ++i) {
						n[i].to_stream(html);
					}
					return picojson::array{ md::raw_html, html.str()};
				}
			}
		}

		if (n.strend) {
			return std::string{ n.strbeg, n.strend };
		} else {
			children.push_back(std::string{ n.strbeg });
		}
	} else {
		children.emplace_back(picojson::value{});
	}
	for (auto &c : n.children) {
		children.emplace_back(convert(c));
	}
	return children;
}

bool is_fresh(const char* in, const char* out) {
	struct stat in_s, out_s;
	if (stat(out, &out_s)) {
		return false;
	}

	stat(in, &in_s);

	return 
		out_s.st_mtime >= in_s.st_mtime &&
		out_s.st_mtime >= exeStat.st_mtime;
}

int compile_page(const char *in, const char *out) {
	std::clog << in << " -> " << out << "\n";

	Packages::MakeMultiLevelPath(out);

#ifdef NDEBUG
	if (is_fresh(in, out)) {
		std::clog << "(not modified)\n";
		return 0;
	}
#endif
	
	std::ifstream read(in);

	std::ofstream writeFile;
	if (strcmp(out, "-")) {
		writeFile.open(out);
		if (!writeFile.is_open()) {
			std::cerr << "Can't write " << out << "\n" << strerror(errno) << "\n";
			exit(-1);
		}
	}

	std::ostream& write{ writeFile.is_open() ? writeFile : std::cout };

	char cwd[_MAX_PATH] = { 0 };
	getcwd(cwd, sizeof(cwd));

	if (!read.is_open()) {
		std::cerr << "Can't open " << in << "\n" << strerror(errno) << "\n";
		exit(-1);
	}

	try {

		std::string source{ in };
		std::string basename, ext;
		auto extDotPos = source.find_last_of('.');
		if (extDotPos != source.npos) {
			basename = source.substr(0, extDotPos);
			ext = source.substr(extDotPos);
		} else basename = source;

		if (ext == ".md") {

			struct stat inst;
			stat(source.c_str(), &inst);

			char buf[128];
			strftime(buf, sizeof(buf), "%FT%TZ", gmtime(&inst.st_mtime));
			std::string isodate = buf;
			strftime(buf, sizeof(buf), "%FT%TZ", gmtime(&inst.st_ctime));
			std::string isodateCreate = buf;

			std::stringstream md;
			md << read.rdbuf();
			auto md_str = md.str();
			auto grammar = mini_markdown();
#ifndef NDEBUG
//			lithe::trace = true;
#endif
			auto parse = grammar->parse(md_str);
			if (parse.is_error()) {
				parse.to_stream(std::cerr);
				std::cerr << std::string(parse[0].strbeg).substr(0, 16) << "...\n";
				return -1;
			}

			auto htmlAsJson = convert(parse);

			picojson::object audioTests;
			picojson::object evalTests;

			auto uniqueName = [names = std::unordered_set<std::string>{}](std::string content) mutable {
				auto hash = Packages::fnv1a(content.data(), content.size()).digest();
				std::string nm = hash;
				int suffix = 1;
				while (names.count(nm)) {
					nm = hash + "-" + std::to_string(suffix++);
				}
				names.emplace(nm);
				return nm;
			};

			std::regex whitespace("\\s+");
			for (auto& a : repl.audio) {
				auto caseName = "Audio-" + uniqueName(a);
				repl.code += "\n:Test:" + caseName + "() { " + a + " snd }\n";
				a = std::regex_replace(a.substr(6), whitespace, " ");
				audioTests[caseName] = picojson::object{ {"label", a} };
			}

			for (auto& e : repl.eval) {
				auto caseName = "Eval-" + uniqueName(e);
				repl.code += "\n:Test:" + caseName + "() { Handle(" + e + " '_ ) }\n";
				e = std::regex_replace(e, whitespace, " ");
				evalTests[caseName] = picojson::object{ {"label", e} };
			}

			picojson::value attach;
			if (attachments.contains("~attachments")) {
				attach = attachments.get("~attachments");
			}
			
			picojson::object json{
				{ "markdown", picojson::object{
					{ basename , md_str }
				}},
				{ "sourcepath", picojson::object{
					{ basename, cwd + std::string("/") + basename }
				}},
				{ "public", picojson::object{
					{ basename , htmlAsJson }
				}},
				{ "modified", picojson::object {
					{ basename, isodate }
				}},
				{ "created", picojson::object {
					{ basename, isodateCreate }
				} },
				{ "~attachments", attach },
			};

			if (audioTests.size() || evalTests.size()) {
				auto lastSep = basename.find_last_of("\\/");
				if (lastSep == basename.npos) lastSep = 0;

				std::string name = "";
				for(;lastSep<basename.size();++lastSep) { 
					auto c = basename[lastSep];
					if (isalnum(c)) {
						name.push_back(c);
					} else {
						if (name.size() && isalnum(name.back())) name.push_back('-');
					}					
				}				

				json["tests"] = picojson::object{ { name,
					picojson::object {
						{ "source", repl.code },
						{ "audio", audioTests },
						{ "eval", evalTests },
				} } };
			}

			write << json;

		} else if (ext == ".json") {
			write << read.rdbuf();
		} else if (ext == ".css" || ext == ".js") {
			std::stringstream str;
			str << read.rdbuf();
			write << picojson::object{
				{ "resources", picojson::object {
					{ in, str.str() }
				} }
			};
		} else {
			std::clog << "Unrecognized extension " << source << "\n";
		}
	} catch (std::exception & e) {
		std::clog << e.what() << "\n";
		writeFile.close();
		if (strcmp(out,"-")) remove(out);
		exit(-1);
	}

	return 0;
}