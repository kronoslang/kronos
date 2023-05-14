#include "kronos.h"

#include <cctype>
#include <string>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <deque>
#include <cassert>
#include <algorithm>
#include <stack>

namespace lithe {
	namespace grammar {
		namespace kronos {
#define TAG(s, name) const char *s = name; 
#include "kronos_tags.inc"
#undef TAG
		}
	}
}

namespace {
	using namespace lithe::grammar::kronos;
	using namespace lithe::grammar::kronos::tag;

	static std::vector<char> construct_punct_table() {
		static const std::string tokenpunct1 = "*/+-";
		static const std::string tokenpunct2 = "?!=<>";
		static const std::string tokenpunct3 = "|&^%";
		static const std::string tokenpunct4 = "._~:^'`´";
		static const std::string tokenpunct = tokenpunct1 + tokenpunct2 + tokenpunct3 + tokenpunct4;

		std::vector<char> table(256);
		for (size_t i = 0;i < table.size();++i) {
			if (tokenpunct1.find((char)i) != std::string::npos) table[i] = 1;
			else if (tokenpunct2.find((char)i) != std::string::npos) table[i] = 2;
			else if (tokenpunct3.find((char)i) != std::string::npos) table[i] = 3;
			else if (tokenpunct4.find((char)i) != std::string::npos) table[i] = 4;
			else table[i] = 0;
		}
		return table;
	}

	static const std::vector<char>& kpunct_table() {
		static const std::vector<char> table = construct_punct_table();
		return table;
	};

	static int iskpunct(int c) {
		if (c > 0 && c < 256) {
			return kpunct_table()[c];
		}
		return 0;
	}

	static const int PatternPrecedence = 9;
	static const int CustomPrecedenceBase = 4;

	const std::unordered_map<std::string, op_t> predefined_mappings{
        { "+",{ 1, false } },
        { "-",{ 1, false } },
        { "*",{ 0, false } },
        { "/",{ 0, false } },
        { "!=",{ 2, false } },
        { "==",{ 2, false } },
        { "=>",{ 4, true } },
        { ">",{ 2, false } },
        { ">=",{ 2, false } },
        { "<",{ 2, false } },
        { "<=",{ 2, false } },
        { "&",{ 3, false } },
        { "|",{ 3, false } },
        { ":",{ PatternPrecedence, true } },
		{ "=",{ 11, false } },
		{ "<-",{ 12, true } }
	};

	static const char *deleted = "DELETED";
	
	template <typename I> struct combine {
		void operator()(const char* label, I dest, std::initializer_list<I> children) {
			lithe::node subst;
			subst.strbeg = label; subst.strend = nullptr;
			subst.children.resize(children.size(), lithe::node(deleted));
			size_t i(0);
			for (auto& c : children) {
				if (subst.src_begin == nullptr || subst.src_begin > c->src_begin) {
					subst.src_begin = c->src_begin;
				}
				if (subst.src_end == nullptr || subst.src_end < c->src_end) {
					subst.src_end = c->src_end;
				}
				std::swap(subst[i++], *c);
			}
			std::swap(*dest, subst);
		}
	};

	static lithe::node merge_defns(lithe::node n) {
		auto i = n.children.begin();
		while (i != n.children.end()) {
			auto next = i; ++next;
			if (next == n.children.end()) break;
			if (i->strbeg == tag::function &&
				next->strbeg == tag::body) {
				// merge
				lithe::node defn;
				defn.strbeg = tag::defn;
				defn.strend = nullptr;
				defn.src_begin = i->src_begin;
				defn.src_end = next->src_end;
				defn.children.emplace_back(*i);
				defn.children.emplace_back(*next);
				*i = defn;

				next->strbeg = deleted;
				++next;
			}			
			i = next;
		}

		auto r_end = std::remove_if(n.children.begin(), n.children.end(),
									[](const lithe::node& c) { return c.strbeg == deleted; });
		if (r_end != n.children.end()) {
			n.children.erase(r_end, n.children.end());
		}
		return n;
	}


	static lithe::node group_infices(lithe::node n) {
		using namespace lithe::grammar::kronos;
		std::deque<decltype(n.children.begin())> infix_pass[13];
		for (auto ci = n.children.begin();ci != n.children.end();++ci) {
			if (ci->strbeg && ci->strend > ci->strbeg && iskpunct(*ci->strbeg)) {
				std::string buf(ci->strbeg, ci->strend);

				// this is a qualified name, not an operator
				if (buf[0] == ':' && buf[1] && !iskpunct(buf[1])) continue;

				auto pre = predefined_mappings.find(buf);
				if (pre != predefined_mappings.end()) {
					if ((buf[0] == ':' && buf[1] == 0) || pre->second.right_associative) {
						// pattern match is right-associative
						infix_pass[pre->second.precedence].emplace_front(ci);
					} else infix_pass[pre->second.precedence].emplace_back(ci);
				} else {
					infix_pass[CustomPrecedenceBase + kpunct_table()[*ci->strbeg]].emplace_back(ci);
				}
			}
		}

		auto prec = [&n](auto i) {
			do { if (n.children.begin() == i) break; --i; } while (i->strbeg == deleted);
			return i;
		};

		auto succ = [&n](auto i) {
			do { ++i; } while (i != n.children.end() && i->strbeg == deleted);
			return i;
		};

		combine<decltype(n.children.begin())> comb;

		for (auto& p : infix_pass) {
			for (auto& ir : p) {
				if (ir->strbeg == deleted) continue;

				auto p = prec(ir);
				auto s = succ(ir);

				bool has_left = ir != p;
				bool has_right = s != n.children.end();

				if (!strncmp(":", ir->strbeg, 1) || !strncmp("<-", ir->strbeg, 2)) {
					// ternary
					if (!has_left) return lithe::node::error(nullptr, ir->strbeg);
					if (!has_right) return lithe::node::error(nullptr, ir->strbeg);
					auto ss = succ(s);
					if (ss == n.children.end()) return lithe::node::error(nullptr, ir->strbeg);
					bool is_pattern = *ir->strbeg == ':';
					comb(is_pattern ? tag::pattern : tag::leftarrow, ir, { p,s,ss });
					if (!is_pattern) {
						// left arrow sucks in everything that's left on the right hand side
						while ((ss = succ(ss)) != n.children.end()) {
							assert(ss->strbeg != deleted);
							ir->children.emplace_back(lithe::node(deleted));
							std::swap(ir->children.back(), *ss);
						}
					}
				} else if (has_left && has_right) {
					comb(tag::infix, ir, { p,ir,s });
				} else if (has_left) {
					comb(tag::sec_left, ir, { p,ir });
				} else if (has_right) {
					comb(tag::sec_right, ir, { ir,s });
				} else {
					ir->children.emplace_back(*ir);
					ir->strbeg = tag::section;
					ir->strend = nullptr;
				}
			}
		}

		n.children.erase(std::remove_if(n.children.begin(), n.children.end(),
										[](const auto& i) { return i.strbeg == deleted;}),
						 n.children.end());

		return n;
	}
}

namespace lithe {
	namespace grammar {
		namespace kronos {
			int istokenchar(int c) {
				if (isalnum(c) || iskpunct(c)) return 1;
				else return 0;
			}

			const std::unordered_map<std::string, op_t>& infix_mappings() {
				return predefined_mappings;
			}

			static std::stack<lithe::node>& docstrings() {
				static thread_local std::stack<lithe::node> comments;
				return comments;
			}

			lithe::rule identifier() {
				return characters("symbol", istokenchar);
			}

			lithe::rule package_version() {
				using namespace grammar::common;
				auto space = I(grammar::common::whitespace());
				auto remote_version = identifier() | L(
					"Version tag",
					(digits(1) << O(T(".") << digits(1) << O(T(".") << digits(1)))));
				return E(tag::package, I("[") << O(space) << identifier() << space << remote_version << O(space << identifier()) << O(space) << I("]"));
			}

			lithe::rule parser(bool keep_comments) {
				using namespace grammar::common;

				// tokens 
				auto upto_eol = characters("up to newline", "\n", true, 0);

				auto whitespace = grammar::common::whitespace();

				auto check_brace = bad("Unmatched parens/bracket/brace", characters("braces", ")]}"));

				auto docstring = custom(
					"docstring", I(T(";") << O(characters("space","\t "))) << (upto_eol | T("\n")), 
				[](const lithe::node& n) {
					if (docstrings().size()) {
						auto& t{ docstrings().top() };
						if (t.src_begin == nullptr || t.src_begin > n.src_begin) {
							t.src_begin = n.src_begin;
						}
						if (t.src_end == nullptr || t.src_end < n.src_end) {
							t.src_end = n.src_end;
						}
						t.children.emplace_back(n);
					}
					return n;
				});


				auto identifier = characters("symbol", istokenchar);
				auto free_var = E(tag::free_var, I("$") << identifier);

				lithe::rule comment, space;

				if (keep_comments) {
					comment = E(tag::comment, T(";") << (docstring | upto_eol | T("\n")));
					space = L("whitespace", repeat(I(whitespace) | comment | I(","), 1));
				} else {
					comment = IE(tag::comment, T(";") << (docstring | upto_eol | T("\n")));
					space = L("whitespace", I(repeat(whitespace | comment | T(","), 1)));
				}

				auto opt_sp = O(space);

				auto number = grammar::common::numeric();

				auto invariant = E(tag::invariant, I("#") << number);
				auto lit_float = E(tag::lfloat, number);
				auto lit_double = E(tag::ldouble, number << I("d"));
				auto lit_int32 = E(tag::lint32, signed_integer() << I("i"));
				auto lit_int64 = E(tag::lint64, signed_integer() << I("q"));
				auto hex = E(tag::lhex, I("0x") << characters("hexadecimal", isxdigit));

				auto here_string = E(tag::hstring, I("\"\"\"\n") << for_(upto_eol << T("\n"), {}, I("\"\"\"\n")));
				auto lit_string = E(tag::lstring, I("\"") << require("Unexpected character in string literal",
					repeat(characters("character", "\n\\\"", true) | T("\\\"") | T("\\n") | T("\\t") | T("\\r") | T("\\\\"), 0) << I("\"")));

				auto lit = IE("literal", invariant | hex | here_string | lit_string | lit_int64 | lit_int32 | lit_double | lit_float);

				// expressions
				auto expr_rec = recursive();
				auto expr = IE("expression", expr_rec | check_brace);

				auto with_infix = [](rule r) {
					return custom("expression", r, group_infices);
				};

				auto with_defns = [](rule r) {
					return custom("statement", r, merge_defns);
				};

				auto unnamed = E(tag::unnamed, I("_"));
				auto req_expr = require("Unrecognized expression", expr);
				auto tuple = E(tag::tuple, with_infix(I("(") << require("Opening parenthesis not matched", opt_sp << for_(req_expr, space, I(opt_sp << T(")"))))));
				auto list = E(tag::list, with_infix(I("[") << require("Opening bracket not matched",  opt_sp << for_(req_expr, space, I(opt_sp << T("]"))))));

				auto fn_attr = E(tag::fnattr, I("#[") << require("Syntax error in function attribute list", opt_sp << for_(identifier, space, I(opt_sp << T("]")))));
				auto fn = E(tag::function, identifier << (tuple | list) << O(I(whitespace) << fn_attr));

				auto use = E(tag::use, I("Use") << space << require("Use directive requires a package identifier and an optional list of symbols to lift", identifier << O(tuple | list)));

				auto statement = use | expr;

				auto bucket_manager = [&](callback_stage stage, cursor& begin, cursor end) {
					switch (stage) {
					case callback_stage::preprocess: docstrings().emplace(tag::docstring);
						break;
					case callback_stage::postprocess: docstrings().pop();
						break;
					}
				};

				auto deposit_comments = [&](const node& n) {
					if (!n.is_error() && docstrings().size() && docstrings().top().size()) {
						return docstrings().top();
					}
					return n;
				};

				auto required_identifier = require("Identifier expected", identifier);

				auto fn_body = E(tag::body, with_defns(with_infix(I("{") << require("Opening brace not matched", callback("docstrings", 
					opt_sp << for_(require("Unrecognized statement", statement), space, custom("docstring", I(opt_sp << T("}")), deposit_comments)), bucket_manager)))));

				auto quoted = E(tag::quote, I("'") << expr);
				expr_rec->assign(quoted | unnamed | lit | tuple | list | fn | identifier | free_var | fn_body);

				// package definition
				auto package = recursive();

				auto type = E(tag::type, I("Type") << space << required_identifier << O(tuple | list));
				package->assign(E(tag::package, I("Package") << space << required_identifier << opt_sp << require("Package contents expected", I("{") << opt_sp <<
																		with_defns(with_infix(
																			for_(type | package | statement, space, I(opt_sp << T("}"))))))));

				// top-level definition
				auto remote_import = package_version(); 

				auto import = E(tag::import, I("Import") << require("Import directive requires core package, literal path or a package import specifier", space << (identifier | lit_string | remote_import)));
				
				return opt_sp << with_defns(with_infix(for_(
						(import | type | package | statement),
						space,
						O(space) << end()
					)));
			}
		}
	}
}