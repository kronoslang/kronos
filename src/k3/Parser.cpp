#include "lithe/grammar/kronos.h"

#include "common/PlatformUtils.h"
#include "Evaluate.h"
#include "TypeAlgebra.h"
#include "Invariant.h"
#include "LibraryRef.h"
#include "Conversions.h"
#include "EnumerableGraph.h"
#include "Parser.h"
#include "UserErrors.h"
#include "Stateful.h"
#include "FlowControl.h"

#include <fstream>
#include <sstream>
#include <memory>
#include <tuple>
#include <algorithm>
#include <iomanip>

namespace K3 {
	namespace Parser {
		using namespace lithe::grammar::kronos;

		using I = std::vector<AstNode>::iterator;

		static void SeekPosition(const char *seek, const char *pos, int& line, int &column, std::string* show_line) {
			const char *line_beg = seek;
			while (seek != pos) {
				if (*seek++ == '\n') { line++; column = 0; line_beg = seek; } else column++;
			}
			if (show_line) {
				while (*seek && *seek != '\n') ++seek;

				while (isspace(*line_beg)) ++line_beg;

				*show_line = std::string(line_beg, seek) + "\n";
				while (line_beg < pos) {
					switch (*line_beg++) {
					case '\t': *show_line += '\t'; break;
					default: *show_line += ' '; break;
					}
				}
				*show_line += '^';
			}
		}

		DynamicScope<const char*> CurrentSourcePosition(nullptr);

		static lithe::rule parser = lithe::grammar::kronos::parser();

		using namespace K3::Nodes;

		struct capture_link {
			resolver_map_t& scope_symbols;
			std::list<std::pair<std::string, std::vector<Lib::Reference*>>> captured_syms;
			capture_link* previous;

			~capture_link() {

			}

			bool is_free(const std::string& sym) const {
				return scope_symbols.count(sym) == 0;
			}
			bool is_in_scope(const std::string& sym) const {
				return previous && (scope_symbols.count(sym) || previous->is_in_scope(sym));
			}
			bool should_capture(const std::string& sym) const {
				if (sym[0] == ' ' || !is_free(sym)) return false;
				return previous && previous->is_in_scope(sym);
			}
		};

		void write_text(const AstNode& n, std::ostream& o) {
			if (n.strend) o.write(n.strbeg, n.strend - n.strbeg);
			else if (n.strbeg) o << n.strbeg;
			for (auto &c : n.children) write_text(c, o);
		}

		static std::string read_string(const AstNode& n) {
			std::stringstream ss;
			write_text(n, ss);
			return ss.str();
		}

		static std::string read_children(const AstNode& n) {
			std::stringstream ss;
			for (auto c : n.children) write_text(c, ss);
			return ss.str();
		}

		static std::string unescape_str(const AstNode& n) {
			std::stringstream ss;
			for (int i(0);i < n.children.size();++i) {
				if (!strncmp(n[i].strbeg, "\\\\", 2)) ss << "\\";
				else if (!strncmp(n[i].strbeg, "\\n ", 2)) ss << "\n";
				else if (!strncmp(n[i].strbeg, "\\r", 2)) ss << "\r";
				else if (!strncmp(n[i].strbeg, "\\t", 2)) ss << "\t";
				else if (!strncmp(n[i].strbeg, "\\\"", 2)) ss << "\"";
				else ss.write(n[i].strbeg, n[i].strend - n[i].strbeg);
			}
			return ss.str();
		}

		const char *get_position(const AstNode& n) {
			if (n.strend) return n.strbeg;
			for (auto &c : n.children) {
				auto pos = get_position(c);
				if (pos) return pos;
			}
			return nullptr;
		}

		const char* get_position(CGRef ast_node) {
			return ast_node->GetRepositoryAddress();
		}

		const char* get_position(const PartialDefinition& pd) {
			for (auto& f : pd.forms) {
				auto pos = get_position(f.body);
				if (pos) return pos;
			}
			return nullptr;
		}

		CGRef fn_apply(const char* name, const char *name_end, CGRef fn, CGRef args) {
			return Evaluate::New(name, fn, args, name_end);
		}

		Err<PartialDefinition> generate_fn(std::string ns, std::string name, CI beg, CI end, const AstNode& args, const AstNode& attributes, capture_link* cl, parser_state_t lookup);

	#define PROPAGATE_ERR(statement) { auto err = statement; if (err.err) return std::move(err.err); }

		static void defineFn(resolver_map_t& defs, std::string sym, const AstNode& c, std::function<Err<PartialDefinition>()> f) {
			resolution_t def{&c, std::move(f)};
			defs[sym].emplace_back(std::move(def));
		}

		static std::string sym_validator(std::string& sym) {
			if (sym.size() && sym.front() == ':') return {};
			auto constraints = sym.find_first_of(':', 1);
			if (constraints != sym.npos) {
				auto validator = sym.substr(constraints + 1);
				sym = sym.substr(0, constraints);
				return validator;
			}
			return {};
		}

		enum class Validation {
			None,
			StripValidator,
			Enable
		};

		static void define(resolver_map_t& defs, std::string sym, const AstNode& c, Validation v, std::function<Err<CGRef>()> f) {
			if (v != Validation::None) {
				if (sym.size()) {
					auto validator = sym_validator(sym);
					if (v == Validation::Enable && validator.size()) {
						f = [sym, validator, valueFn = f]() -> Err<CGRef> {
							auto validatorSym = Lib::Reference::New({ validator, ":" + validator, ":Constraints:" + validator });
							LET_ERR(val, valueFn());
							auto explanation = "Type of '" + sym + "' must match '" + validator + "'";
							return Invariant::ExplainConstraint::New(Evaluate::New("validator", validatorSym, val), explanation, val);
						};
					}
				}
			}

			resolution_t def{
				&c,
				[f]() -> Err<PartialDefinition> {
					LET_ERR(val, f());
					return PartialDefinition::Value(val);
				}
			};
			defs[sym].emplace_back(std::move(def));
		}

		static Err<void> destructure(resolver_map_t& defs, const AstNode& c, Validation v, std::function<Err<CGRef>()> rhs) {
			if (c.strend) {
				define(defs, string{ c.strbeg, c.strend }, c, v, [rhs]() { return rhs();});
				return {};
			}

			if (c.strbeg == tag::unnamed) {
				return {};
			}

			if (c.children.empty()) return {};
			int i(0);
			for (;i < c.children.size() - 1;++i) {
				auto err = destructure(defs, c.children[i], v, [rhs]() -> Err<CGRef> { LET_ERR(r, rhs());  return GenericFirst::New(r); });
				if (err.err) return err;
				rhs = [rhs]() -> Err<CGRef> { LET_ERR(r, rhs()); return GenericRest::New(r); };
			}
			if (c.strbeg == tag::tuple) return destructure(defs, c.children[i], v, rhs);
			else if (c.strbeg == tag::list) return destructure(defs, c.children[i], v, [rhs]() -> Err<CGRef> { LET_ERR(r, rhs()); return GenericFirst::New(r); });
			else return ParserError(get_position(c), "Definition error");
		}

		static Err<void> destructure(resolver_map_t& defs, const AstNode& c, Validation v, CGRef rhs) {
			return destructure(defs, c, v, [rhs]() {return rhs;});
		}

		static Err<CGRef> MakeRingBuffer(RingBufferTimeBase tb, const std::vector<CGRef>& n) {
			CGRef initializer;
			CGRef order;
			
			switch (n.size()) {
			case 2:
				initializer = Invariant::Constant::New(0);
				order = n[0];
				break;
			case 3:
				initializer = n[0];
				order = n[1];
				break;
			default:
				return ParserError(nullptr, "rbuf must have two or three arguments; optional initializer, invariant order and signal input.");
			}

			auto rb = GenericRingBuffer::New(tb, initializer, order);
			rb->Connect(n.back());
			return (CGRef)rb;
		}

#define CHECK_ARITY(N) if (n.size() != N) return ParserError(nullptr, #N " arguments are required!");
		using nrv = const std::vector<CGRef>&;
		static const std::unordered_map<std::string, std::function<Err<CGRef>(const std::vector<CGRef>&)>> builtins = {
			{ "Polymorphic", [](nrv n) { auto p = Polymorphic::New(); for (auto u : n) p->Connect(u); return p;}},
			{ "Explain-Constraint", [](nrv n) -> Err<CGRef> { CHECK_ARITY(3); return Invariant::ExplainConstraint::New(n[0], n[1], n[2]); } },
			{ "When",   [](nrv n) { auto w = When::New(); for (auto u : n) w->Connect(u); return w; } },
			{ "Handle", [](nrv n) -> Err<CGRef> { CHECK_ARITY(2); auto h = Handle::New(n[0], n[1]); return (CGRef)h; } },
			{ "Raise",  [](nrv n) -> Err<CGRef> {
				if (n.size() < 1) {
					return Raise::New(Invariant::Constant::New(Type::Nil));
				}
				auto err = n.back();
				for (int i = (int)n.size() - 2; i >= 0; --i) {
					err = GenericPair::New(n[i], err);
				}
				return Raise::New(err);
			} },
			{ "z-1",    [](nrv n) -> Err<CGRef> {
				GenericRingBuffer* rb;
				switch (n.size()) {
					case 1: rb = GenericRingBuffer::New(smp, Invariant::Constant::New(0), Invariant::Constant::New(1)); break;
					case 2: rb = GenericRingBuffer::New(smp, n[0], Invariant::Constant::New(1)); break;
					default: return ParserError(nullptr, "z-1 must have one or two arguments; optional initializer and signal input."); break;
				}
				rb->Connect(n.back()); 
				return GenericRingBuffer::rbuf(rb); 
			} },
			{ "rbuf",   [](nrv n) -> Err<CGRef> { auto rb = MakeRingBuffer(smp, n); if (rb.err) return std::move(rb.err); else return GenericRingBuffer::rbuf(*rb); } },
			{ "rcsbuf", [](nrv n) { return MakeRingBuffer(smp, n); } },
			{ "tbuf",   [](nrv n) -> Err<CGRef> { auto rb = MakeRingBuffer(sec, n); if (rb.err) return std::move(rb.err); else return GenericRingBuffer::rbuf(*rb); } },
			{ "tcsbuf", [](nrv n) { return MakeRingBuffer(sec, n); } },
			{ "Specialization-Monitor", [](nrv n) -> Err<CGRef> {
				CHECK_ARITY(2);
				return GenericSpecializationCallback::New(n[0], n[1]);
			} }
		};

		static const std::unordered_multimap<std::string, std::pair<std::string, std::string>> builtin_metadata = {
			{ "Polymorphic",{ "(forms...)", "Apply each of the alternative 'forms' until a valid typed form is found." } },
			{ "When", { "(condition branch)", "When 'condition' is True, value is taken from 'branch'. Additional conditions and branches may be supplied, and they will be tried in order. When uses non-standard short-circuiting evaluation. If no branches are taken, specialization fails."}},
			{ "Handle", { "(try catch-fn)", "Attempts to specialize the 'try' value. If an exception is generated, call 'catch-fn' with the exception as a parameter. Any run-time variant data in the exception will be purged and replaced with an invariant type tag. Handle uses non-standard evaluation, only attempting to specialize 'catch-fn' if required."}},
			{ "z-1",{ "(sig~)", "Provides signal memory with a unity delay. The delay is equivalent to one activation frame of the clock that drives 'sig~'. Before any activations, z-1 will have a zero value. " } },
			{ "z-1", { "(init sig~)", "Provides signal memory with a unit delay. The delay is equivalent to one activation frame of the clock that drives 'sig~'. Before any activations, z-1 will output the value in 'init'. The types of 'init' and 'signal' must be identical. Cyclic definitions via 'sig~' are allowed. "} },
			{ "rbuf",{ "(order sig~)", "Provides a signal memory ring buffer with 'order' elements. The 'order' parameter must be an invariant constant. Initially, the ring buffer is filled with zero. Cyclic definitions via 'sig~' are allowed." } },
			{ "rbuf", { "(init order sig~)", "Provides a signal memory ring buffer with 'order' elements. The 'order' parameter must be an invariant constant. The type and initial value of the elements is determined by 'init'. Cyclic definitions via 'sig~' are allowed." } },
			{ "tbuf", { "(init duration sig~)", "Provides a signal memory ring buffer with 'duration' in seconds, relative to the sample rate of `sig~`. The buffer is initialized with as many copies of `init` as required." } },
			{ "rcsbuf", { "(init order sig~)", "Provides a signal memory ring buffer with 'order' elements. The 'order' parameter must be an invariant constant. The type and initial value of the elements is determined by 'init'. \n\nOUTPUTS: (buffer output index) where 'output' is the value evicted from the buffer, 'buffer' is the content of the ring buffer prior to the current activation frame, and 'index' is the position of the write head in the ring buffer. Cyclic definitions via 'sig~' are allowed." } },
			{ "tcsbuf", { "(init duration sig~)", "Provides a signal memory ring buffer with 'duration' relative to the clock rate of `sig~`. The 'duration' must be an invariant constant. The type and initial value of the elements is determined by 'init'. \n\nOUTPUTS: (buffer output index) where 'output' is the value evicted from the buffer, 'buffer' is the content of the ring buffer prior to the current activation frame, and 'index' is the position of the write head in the ring buffer. Cyclic definitions via 'sig~' are allowed." } }
		};

		struct expr {
			const parser_state_t pst;
			capture_link* cl;
			std::unordered_map<std::string, Lib::Reference*> lvars;
			
			Err<CGRef> cons_tuple(const AstNode& n, CGRef t)  {
				for (auto i = n.children.rbegin();i != n.children.rend();++i) {
					LET_ERR(n, cons(*i));
					t = t ? GenericPair::New(n, t) : n;
				}
				return t ? t : Invariant::Constant::New(false);
			}

			static std::string get_infix_fn(std::string op) {
				if (op.front() == '`' && op.back() == '`') return op.substr(1, op.size() - 2);
				return "Infix" + op;
			}

			Err<PartialDefinition> cons_fn(const std::string& ns, const std::string& name, const AstNode& args, CI beg, CI end, AstNode attributes = {}) {
				auto result = generate_fn(ns, name, beg, end, args, attributes, cl, pst);
				if (result.err) {
					return ParserError(result.err->GetSourceFilePosition(),
									   result.err->GetErrorMessage() + ", while parsing "s + ns + name);
				}
				return *result;
			};

#define CASE(ty) if (n.strbeg == tag :: ty)
			Err<CGRef> cons(const AstNode& n)  {
				auto binding(CurrentSourcePosition = get_position(n));
				CASE(quote) {
					LET_ERR(afn, cons_fn("", "", AstNode(), n.children.begin(), n.children.end()));
					LET_ERR(fn, afn.Complete(nullptr));
					return fn;
				}
				CASE(leftarrow) {
					AstNode body(tag::body);
					body.children.emplace_back(tag::tuple);
					for (int i(2);i < n.children.size();++i) body[0].children.emplace_back(n[i]);

					LET_ERR(a, cons(n[1]))
					LET_ERR(sym, refer(":Invoke-With", get_position(n)))
					LET_ERR(afn, cons_fn("", "", n[0], body.children.begin(), body.children.end()))
					LET_ERR(fn, afn.Complete(nullptr));

					return fn_apply("<-",nullptr, sym,
									GenericPair::New(a, fn));
				}
				CASE(infix) {
					std::string sym, op = read_string(n[1]);
					if (op == "=>") {
						LET_ERR(afn, cons_fn("", "", n[0], n.children.begin() + 2, n.children.end()));
						LET_ERR(fn, afn.Complete(nullptr));
						return fn;
					}
					LET_ERR(lhs, cons(n[0]));
					LET_ERR(rhs, cons(n[2]));
					sym = get_infix_fn(op);
					LET_ERR(tmp, cons(AstNode{ sym.c_str() }));
					return fn_apply(n[1].strbeg, n[1].strend, tmp, GenericPair::New(lhs, rhs));
				}
				CASE(pattern) {
					auto when = When::New();
					LET_ERR(a, cons(n[0]));
					LET_ERR(b, cons(n[1]));
					LET_ERR(c, cons(n[2]));
					when->Connect(a);
					when->Connect(b);
					when->Connect(Invariant::Constant::New(true));
					when->Connect(c);
					return (CGRef)when;
				}
				CASE(function) {
					if (n[0].strend && n[0].strbeg) {
						auto builtin = builtins.find(std::string(n[0].strbeg, n[0].strend));
						if (builtin != builtins.end()) {
							std::vector<CGRef> up(n[1].children.size());
							for (int i(0); i < n[1].children.size(); ++i) {
								LET_ERR(upv, cons(n[1][i]));
								up[i] = upv;
							}
							return builtin->second(up);
						}
					}
					LET_ERR(a, cons(n[0]));
					LET_ERR(b, cons(n[1]));
					return fn_apply(read_string(n[0]).c_str(), nullptr, a, b);
				}
				CASE(tuple) {
					return cons_tuple(n, nullptr);
				}
				CASE(list) {
					return cons_tuple(n, Invariant::Constant::New(false));
				}
				CASE(lfloat) {
					auto lit = read_children(n);
					return Convert::New(Convert::Float32, Invariant::Constant::New(strtod(lit.c_str(), nullptr)));
				}
				CASE(ldouble) {
					auto lit = read_children(n);
					return Convert::New(Convert::Float64, Invariant::Constant::New(strtod(lit.c_str(), nullptr)));
				}
				CASE(lint32) {
					auto lit = read_children(n);
					return Convert::New(Convert::Int32, Invariant::Constant::New((int)strtol(lit.c_str(), nullptr, 10)));
				}
				CASE(lint64) {
					auto lit = read_children(n);
					return Convert::New(Convert::Int64, Invariant::Constant::New((int64_t)strtoll(lit.c_str(), nullptr, 10)));
				}
				CASE(lhex) {
					auto lit = read_children(n);
					if (lit.size() > 8) {
						return Convert::New(Convert::Int64, Invariant::Constant::New((int64_t)strtoull(lit.c_str(), nullptr, 16)));
					} else {
						return Convert::New(Convert::Int32, Invariant::Constant::New((int32_t)strtoul(lit.c_str(), nullptr, 16)));
					}
				}
				CASE(lstring) {
					if (n.children.empty()) return Invariant::Constant::New(Type(""));
					return Invariant::Constant::New(Type(unescape_str(n).c_str()));
				}
				CASE(hstring) {
					if (n.children.empty()) return Invariant::Constant::New(Type(""));
					std::stringstream ss;
					for (auto &c : n.children) ss << c.get_string();
					return Invariant::Constant::New(Type(ss.str().c_str()));
				}
				CASE(invariant) {
					auto lit = read_children(n);
					return Invariant::Constant::NewBigNumber(lit.c_str());
				}
				CASE(body) {
					if (n.children.empty()) {
						return  ParserError(get_position(n), "Function contains no forms");
					}
					LET_ERR(afn, cons_fn("", "", AstNode(), n.children.begin(), n.children.end()));
					return afn.Complete(nullptr);
				}
				CASE(sec_right) {
					AstNode tmp(tag::infix);
					tmp.children = { AstNode(tag::unnamed),n[0],n[1] };
					AstNode quote(tag::quote);
					quote.children = { tmp };
					return cons(quote);
				}
				CASE(sec_left) {
					AstNode tmp(tag::infix);
					tmp.children = { n[0], n[1], AstNode(tag::unnamed) };
					AstNode quote(tag::quote);
					quote.children = { tmp };
					return cons(quote);
				}
				CASE(section) {
					std::string sym(get_infix_fn(read_string(n[0])));
					return cons(AstNode(sym.c_str()));
				}
				CASE(unnamed) {
					LET_ERR(n, refer("_", get_position(n)));
					return (CGRef)n;
				}
				assert(n.children.empty());
				auto name = read_string(n);
				LET_ERR(sym, refer(name, get_position(n)));
				return (CGRef)sym;
			}

			Err<Lib::Reference*> refer(const std::string& name, const char *pos) {
				auto f = lvars.find(name);
				if (f == lvars.end()) {
					LET_ERR(ref, make_refer(name, pos));
					f = lvars.emplace(name, ref).first;
				}
				return f->second;
			}

			Err<Lib::Reference*> make_refer(const std::string& name, const char *pos) {
				if (name.front() == ':') {
					// absolute name; no lookup
					return Lib::Reference::New({ name });
				}

				if (builtins.find(name) != builtins.end()) {
					return ParserError(pos, "'" + name + "' is a reserved word and can't be used as a symbol!");
				}

				if (pst.explicit_uses.count(name)) {
					return Lib::Reference::New({ name, pst.explicit_uses.at(name) });
				}

				std::vector<std::string> lookups(pst.namespaces.size() + 1);
				lookups[0] = name;
				for (size_t i(0);i < pst.namespaces.size();++i) lookups[i + 1] = pst.namespaces[i] + name;
				return Lib::Reference::New(std::move(lookups));
			}
		};

		static void write_arg(std::ostream& s, std::string arg, std::ostream& docs) {
			auto validator = sym_validator(arg);
			if (validator.size()) {
				docs << "- `" << arg << "` " << validator << "\n";
			}
			s << arg;
		}

		static void write_args(std::ostream& s, const AstNode& n, std::ostream& docs) {
			if (n.strbeg == tag::tuple) s << "(";
			else if (n.strbeg == tag::list) s << "[";
			else {
				if (n.strend) write_arg(s, { n.strbeg,n.strend }, docs);
				else write_arg(s, n.strbeg, docs);
				return;
			}
			for (int i(0); i < n.children.size(); ++i) {
				if (i) s << " ";
				write_args(s, n[i],docs);
			}
			if (n.strbeg == tag::tuple) s << ")";
			if (n.strbeg == tag::list) s << "]";
		}

		static void gather_symbols(const AstNode& c, std::unordered_set<std::string>& syms) {
			if (c.strend) syms.emplace(c.strbeg,c.strend);
			else {
				for(auto& cc:c.children) gather_symbols(cc, syms);
			}
		}

		using parse_hook = std::function<Err<void>(const AstNode&)>;
		using parse_hooks = std::unordered_map<const char*, parse_hook>;

		Err<void> generate_syms(CI beg, CI end, capture_link& scope, parser_state_t& pstate, const parse_hooks& hooks) {
			resolver_map_t& local_syms(scope.scope_symbols);
			// must place all the symbols into scope first to enable captured variables to be recognized

			for (auto i(beg);i != end;++i) {
				auto &n(*i);

				auto ext = hooks.find(n.strbeg);
				if (ext != hooks.end()) {
					auto err = ext->second(n);
					if (err.err) return err;
					continue;
				}

				CASE(use) {
					auto package = read_string(n[0]);
					if (n.children.size() == 2) {
						std::unordered_set<std::string> syms;
						gather_symbols(n[1], syms);
						for (auto &s : syms) {
							pstate.explicit_uses[s] = ":" + package + ":" + s;
						}
					} else {
						pstate.namespaces.emplace_back(":" + package + ":");
					}
					continue;
				}

				CASE(type) {
					std::string name(read_string(n[0]));
					std::string type_name = pstate.namespaces[0] + name;
					define(local_syms, name, n, Validation::None, [type_name,pos = get_position(n)]() { 
						auto binding(CurrentSourcePosition = pos);
						return  GenericTypeTag::New(type_name);
					});

					if (n.size() > 1) {
						// structural sugar
						std::stringstream arglist, consDoc;
						write_args(arglist, n[1], consDoc);
						auto args = arglist.str();
						for (auto& c : args) c = tolower(c);


						resolver_map_t structure;
						destructure(structure, n[1], Validation::Enable, GenericArgument::New());

						// generate constructor
						defineFn(local_syms, name + ":Cons", n, 
								 [type_name, structure, args, mdoc = consDoc.str()] 
								 () -> Err<PartialDefinition> {
							PartialDefinition constructor;
							CGRef checkedArgs = Invariant::Constant::New(Type::Nil);

							for (auto& m : structure) {
								if (m.second.size()) {
									LET_ERR(c, m.second.front().second());
									if (c.forms.size()) {
										checkedArgs = GenericPair::New(c.forms.front().body, checkedArgs);
									}
								}
							}

							checkedArgs = Invariant::GenericRequire::New(checkedArgs, GenericArgument::New());

							std::string failure = "Invalid constituent types for nominal `" + type_name + args + "`";

							constructor.forms = {
								Form{
									Raise::NewFatalFailure(failure.c_str()),
									Attributes::None,
									Form::Function
								},
								Form{
									GenericMake::New(GenericTypeTag::New(type_name), checkedArgs),
									Attributes::Extend,
									Form::Function
							} };
							constructor.metadata[args] = "Construct an instance of `" + type_name + "` type.\n\n" + mdoc;
							return constructor;
						});

						// destructuring
						resolver_map_t members;
						destructure(members, n[1], Validation::StripValidator, GenericBreak::New(GenericTypeTag::New(type_name), GenericArgument::New()));
						std::string typeArgName = name;
						for (auto& c : typeArgName) c = tolower(c);

						// generate accessors
						for (auto& m : members) {
							if (m.second.size()) {
								defineFn(local_syms, name + ":" + m.first, n, [=]() -> Err<PartialDefinition> {
									PartialDefinition accessor;
									LET_ERR(acc, m.second.front().second());
									if (acc.forms.size() != 1) {
										return ParserError(get_position(n[1]), "Invalid member accessor");
									}
									acc.forms.front().attr = Attributes::Pattern;
									acc.forms.front().mode = Form::Function;
									auto memberName = m.first;
									for (auto& c : memberName)  c = tolower(c);
									acc.metadata[typeArgName] = "Retrieve member `" + memberName + "` of an instance of type `" + name + "`. This is a pattern matching accessor.";
									return acc;
								});
							}
						}

						// generate reflection and constraint

						auto typeCheck = Invariant::GenericEqualType::New(
							Invariant::GenericClassOf::New(GenericArgument::New()),
							GenericTypeTag::New(type_name));

						defineFn(local_syms, name + "?", n,
							[=]() -> Err<PartialDefinition> {
								auto w = When::New();
								w->Connect(typeCheck);
								w->Connect(Invariant::Constant::New(true));

								w->Connect(Invariant::Constant::New(/*else*/ true));
								w->Connect(Invariant::Constant::New(false));
								PartialDefinition typeQuery;
								typeQuery.forms = {
									Form{ w, Attributes::None, Form::Function }
								};
								typeQuery.metadata[typeArgName] = "Returns `True` if `" + typeArgName + "` is of type `" + type_name + "`";
								return typeQuery;
						});

						defineFn(local_syms, ":Constraints" + pstate.namespaces[0] + name + "!", n,
							[=]() -> Err<PartialDefinition> {
									 PartialDefinition typeRequire;
									 auto w = When::New();
									 w->Connect(typeCheck);
									 w->Connect(GenericArgument::New());
									 typeRequire.forms = {
										 Form{ w, Attributes::Pattern, Form::Function }
									 };
									 typeRequire.metadata[typeArgName] = "Pattern-matching identity function that is valid when argument is of type `" + type_name + "`";
									 return typeRequire;
						});
					}

					continue;
				}

				expr expr_cons{ pstate, &scope };

				CASE(defn) {
					auto name = read_string(n[0][0]);
					while(true) {
						// multiple functions can be defined at once as Foo/Bar/Baz(parameters) { ... }
						auto slash = name.find_first_of('/');
						auto subname = name.substr(0, slash);
						auto ns = scope.previous ? "" : pstate.namespaces.front();
						defineFn(local_syms, subname, n, [ns,expr_cons,subname,&n]() mutable -> Err<PartialDefinition> {
							auto binding(CurrentSourcePosition = get_position(n));
							if (n[1].children.empty()) {
								return ParserError(CurrentSourcePosition, "Empty function!");
							}
							AstNode attr;
							if (n[0].children.size() > 2) {
								attr = n[0][2];
							}
							
							LET_ERR(definition, expr_cons.cons_fn(ns, subname, n[0][1], n[1].children.begin(), n[1].children.end(), attr));
							
							std::stringstream argString, paramTypeDoc;
							write_args(argString, n[0][1], paramTypeDoc);

							auto& doc{ definition.metadata[argString.str()] };
							if (doc.size()) {
								doc.push_back('\n');
							}
							doc += paramTypeDoc.str();

							if (n[1].children.size()) {
								if (n[1].children.back().strbeg == tag::docstring) {
									for (auto& ds : n[1].children.back().children) {
										if (doc.size()) doc.push_back('\n');
										auto d = ds.get_string();
										doc += d;
									}
								}
							}

							return definition;
						});
						if (slash == name.npos) break;
						name = name.substr(slash+1);
					}
					continue;
				}
				CASE(infix) {
					auto op = read_string(n[1]);
					if (op == "=") {
						auto binding(CurrentSourcePosition = get_position(n));
						auto err = destructure(local_syms, n[0], Validation::Enable, [expr_cons, up = n[2], state = std::shared_ptr<Err<CGRef>>()]() mutable -> Err<CGRef> {
							if (state) return *state;
							state = std::make_shared<Err<CGRef>>(expr_cons.cons(up));
							return *state;
						});

						if (err.err) return err;

						continue;
					}
				}
				CASE(docstring) {
					continue;
				}

				ext = hooks.find(tag::immediate);
				
				if (ext != hooks.end()) {
					auto err = ext->second(n);
					if (err.err) return err;
					continue;
				}
				define(local_syms, "", n, Validation::Enable, [&n, expr_cons]() mutable { return expr_cons.cons(n); });
			}
			return {};
		}

		std::vector<std::string> get_namespaces(std::string ns) {
			std::vector<std::string> lookup;
			size_t pos = ns.npos;
			while ((pos = ns.find_last_of(':', pos)) != ns.npos) {
				lookup.emplace_back(ns.substr(0, pos + 1));
				if (pos == 0) break;
				else pos--;
			};
			return lookup;
		}

		static Err<std::unordered_map<std::string, std::vector<CGRef>>> resolve_local_bindings(capture_link& lexical_scope) {
			// resolve local names
			std::unordered_map<std::string, std::vector<CGRef>> bindings;
			for (auto &ss : lexical_scope.scope_symbols) {
				for (auto &d : ss.second) {
					LET_ERR(value, d.second());
					LET_ERR(expr, value.Complete(nullptr));
					bindings[ss.first].emplace_back(expr);
				}
			}

			for (auto &b : bindings) {
				for (auto &form : b.second) {
					for (auto r : Qxx::FromGraph(form).OfType<Lib::Reference>()) {
						if (r->GetLocalResolution() == nullptr) {
							auto lsym = bindings.find(r->GetName());
							if (lsym != bindings.end()) {
								r->Resolve(lsym->second[0]);
							} else if (lexical_scope.should_capture(r->GetName())) {
								auto i = lexical_scope.captured_syms.begin();
								for (;i != lexical_scope.captured_syms.end();++i) {
									if (i->first == r->GetName()) {
										if (std::find(i->second.begin(),i->second.end(),r) == i->second.end())
											i->second.emplace_back(r);
										break;
									}
								}
								if (i == lexical_scope.captured_syms.end()) {
									std::vector<Lib::Reference*> refs = { r };
									lexical_scope.captured_syms.emplace_back(r->GetName(), std::move(refs));
								}
							}
						}
					}
				}
			}

			return bindings;
		}

		static Type pack_fn(const char *sym, Type graph_list, Type recur_pts = Type()) {
			return Type::User(&FunctionTag, Type::Tuple(Type(sym), recur_pts, graph_list, Type::Nil));
		}

		class ShareSubgraphs : public Transform::Identity<const Generic> {
			struct Hash {
				bool operator()(CGRef a) const {
					return a->GetHash();
				}
			};

			struct Eq {
				bool operator()(CGRef a, CGRef b) const {
					return a->Compare(*b) == 0;
				}
			};

			std::unordered_set<CGRef, Hash, Eq> shareSubgraphs;
			CGRef recursivePoint = nullptr;
		public:
			ShareSubgraphs(CGRef root, CGRef recursivePt) :Identity(root), recursivePoint(recursivePt) { }

			CGRef operate(CGRef src) override {
				if (src == recursivePoint) return recursivePoint;
				auto f = shareSubgraphs.find(src);
				if (f == shareSubgraphs.end()) {
					return *shareSubgraphs.emplace(src->IdentityTransform(*this)).first;
				} else {
					return *f;
				}
			}
		};

		static bool has_attribute(const AstNode& attributes, const std::string& attrName) {
			for (auto &c : attributes.children) {
				if (c.get_string() == attrName) return true;
			}
			return false;
		};

		Err<PartialDefinition> generate_fn(std::string sym_ns, std::string sym, CI beg, CI end, const AstNode& args, const AstNode& attributes, capture_link* cl, parser_state_t pst) {
			resolver_map_t defs;
			capture_link scope{ defs, {}, cl };
			static const string ret_val = "return value"; // due to whitespace this can't collide with program defs
			
			auto arg_root = Lib::Reference::New({ " arg" }, false);
			auto recur = sym_ns.empty() ? Lib::Reference::New({ " recur" }, false) : Lib::Reference::New({ sym_ns + sym });

			if (sym.empty()) define(defs, "_", args, Validation::None, [arg_root]() {return arg_root;});
			define(defs, "arg", args, Validation::None, [arg_root]() {return arg_root;});
			define(defs, "Recur", args, Validation::None, [recur] {return recur;});

			destructure(defs, args, Validation::Enable, arg_root);
			generate_syms(beg, end, scope, pst, {
			{ tag::package,[](const AstNode& n) {
				return ParserError(get_position(n),"Functions cannot contain packages");
			} },
			{ tag::import,[](const AstNode& n) {
				return ParserError(get_position(n),"Functions cannot import packages");
			} }
			});

			if (scope.scope_symbols[""].size()) {
				if (sym.size() && scope.scope_symbols.count(sym)) {
					return ParserError(get_position(*scope.scope_symbols[""].front().second()), 
						"This is an unnamed return value; however, the function also has a named return value");
				} else {
					if (scope.scope_symbols[""].size() > 1) {
						auto fst = scope.scope_symbols.find("");
						return ParserError(get_position(*fst->second[0].second()),
							"Multiple unnamed return values found; this is disallowed to avert bugs");
					}
					scope.scope_symbols[ret_val] = std::move(scope.scope_symbols[""]);
					scope.scope_symbols.erase("");
				}
			} else {
				scope.scope_symbols[ret_val] = std::move(scope.scope_symbols[sym]);
				scope.scope_symbols.erase(sym);
			}

			LET_ERR(bindings, resolve_local_bindings(scope));

			for (auto &b : bindings) {
				if (b.first != ret_val && b.second.size() > 1)
					return ParserError(get_position(b.second[1]), "Redefinition of '" + b.first + "' is not allowed");
			}

			if (bindings.count(ret_val) == 0) {
				return ParserError(get_position(*beg), "Function contains no forms");
			}

			// wrap up captures & argument
			if (scope.captured_syms.size()) {
				CGRef arg = GenericArgument::New();
				CGRef capture_root = GenericFirst::New(arg);
				arg_root->Resolve(GenericRest::New(arg));
				for (auto& csym : scope.captured_syms) {
					for(auto &ref : csym.second) ref->Resolve(GenericFirst::New(capture_root));
					capture_root = GenericRest::New(capture_root);
				}
			} else {
				arg_root->Resolve(GenericArgument::New());
			}

			PartialDefinition pd;

			if (!has_attribute(attributes, "Extend")) {
				pd.forms.emplace_back(Form::Fn(
					Evaluate::CallLib(
						":Fallback:No-Match", 
						GenericPair::New(
							Invariant::Constant::New(Type((sym_ns + sym).c_str())),
							arg_root))));
			}

			if (has_attribute(attributes, "Pattern")) {
				if (pd.forms.empty()) {
					return ParserError(get_position(attributes),
									   "Extend and Pattern attributes are mutually exclusive");
				}
				pd.forms.clear();
				pd.forms.emplace_back(Form::Fn(
					Invariant::GenericPropagateFailure::New()));
				pd.forms.back().attr = Attributes::Pattern;
			}

			for (auto& binding : bindings[ret_val]) {
				// dedupe graph
				ShareSubgraphs dedupe{ binding, recur };
				auto formGraph = dedupe.Go();

				for (auto ev : Qxx::FromGraph(formGraph).OfType<Nodes::Evaluate>()) {
					Lib::Reference* ref;
					if (ev->GetUp(0)->Cast(ref) && (ref == recur || ref->GetName() == "Recur")) {
						pd.recurData.emplace_back(Type(ev));
					}
				}

				Form f;
				f.mode = Form::Function;
				f.attr = Attributes::Extend;
				f.body = formGraph;
				pd.forms.emplace_back(f);
			}

			if (pd.forms.size() && has_attribute(attributes, "Override")) {
				if (has_attribute(attributes, "Extend")) {
					return ParserError(get_position(attributes),
									   "Extend and Override attributes are mutually exclusive");
				}
				pd.forms[0].attr = Attributes::AlwaysOverride;
			}


			LET_ERR(fn, pd.Complete(nullptr, sym));
			if (scope.captured_syms.size()) {
				CGRef curry_args = Invariant::Constant::New(false);
				for (auto csi = scope.captured_syms.rbegin();csi != scope.captured_syms.rend();++csi) {
					curry_args = GenericPair::New(Lib::Reference::New({ csi->first }), curry_args);
				}
				fn = fn_apply(":Curry", nullptr, Lib::Reference::New({ ":Curry" }), GenericPair::New(fn, curry_args));
				// closures need to be defined as values rather than multimorphs because
				// of the enclosing curry
				pd = PartialDefinition::Value(fn);
			}

			if (sym_ns.empty()) {
				fn->HasInvisibleConnections();
				recur->Resolve(fn);
			}

			return pd;
		}

		static Err<void> Reify(std::string ns, DefinitionMap& resolved, const resolver_map_t& defs) {
			for (auto& ds : defs) {
				PartialDefinition pd;
				for (auto& d : ds.second) {
					LET_ERR(resolution, d.second());
					pd.Append(resolution);
				}
				std::string qn = ds.first;
				if (qn.front() != ':') qn = ns + qn;
				resolved.emplace(qn, pd);
			}
			return { };
		}

		Err<void> GeneratePackage(DefinitionMap& parentDefs, std::string name, const CI& beg, const CI& end, parser_state_t state) {
			state.namespaces.push_front(name);

			resolver_map_t defs;
			capture_link scope{ defs, { }, nullptr };

			CHECK_ERR((generate_syms(beg, end, scope, state, {
			{
				tag::package,
				[&](const AstNode& n) {
					auto packname = read_string(n[0]);
					if (packname.front() != ':') packname = name + packname;
					return GeneratePackage(parentDefs, packname + ":", ++n.children.begin(),n.children.end(), state);
				} 
			}})));

			return Reify(name, parentDefs, defs);
		}


		Err<DefinitionMap> GenerateSymbols(const AstNode& p, 
							parser_state_t& state,
							std::function<Err<void>(BufferKey)> importHook,
							immediate_handler_t imm) {
			resolver_map_t defs;
			capture_link scope{ defs,{},nullptr };
			std::unordered_map<std::string, PartialDefinition> resolved;

			auto err = (generate_syms(p.children.begin(), p.children.end(), scope, state, {
				{ tag::package,[&](const AstNode& n) -> Err<void> {
					auto packName = read_string(n[0]);
					if (packName.front() != ':') packName = ":" + packName;
					return GeneratePackage(resolved, packName + ":", ++n.children.begin(), n.children.end(), state);
			} },
			{ tag::import,[&](const AstNode& n) -> Err<void> {
				if (n[0].strend) {
					return importHook(BufferKey{ "#CORE", "", n[0].get_string() + ".k" });
				} else if (n[0].strbeg == tag::lstring) {
					return importHook(BufferKey{ "#LOCAL", "", read_children(n[0]) });
				} else if (n[0].strbeg == tag::package) {
					std::string file = "main.k";

					if (n[0].children.size() > 2) {
						file = n[0][2].get_string();
					}

					return importHook(
						BufferKey{ 
							n[0][0].get_string(),
							n[0][1].get_string(),
							file
						}
					);
				}
				return { };
			} },
			{ tag::immediate,[&](const AstNode& n) -> Err<void> {
				expr immed{state,&scope};
				LET_ERR(e, immed.cons(n))
				imm("??", e);
				return {};
			} }
			}));

			if (err.err) return *err.err;

			CHECK_ERR(Reify(":", resolved, defs));
			return resolved;
		}

		static std::ostream& json_string(std::ostream& strm, const std::string& str) {
			strm << "\"";
			for (auto c : str) {
				switch (c) {
				case '\n':strm << "\\n"; break;
				case '\r':strm << "\\r"; break;
				case '\t':strm << "\\t"; break;
				case '\"':strm << "\\\""; break;
				case '\\': strm << "\\\\"; break;
				default: 
					if ((unsigned char)c < 0x20) {
						strm << std::hex << std::setw(4) << std::setfill('0') << (int)c;
					} else {
						strm.put(c);
					}
					break;
				}
			}
			strm << "\"";
			return strm;
		}

		void Repository2::ExportMetadata(std::ostream& json_doc) {
			json_doc << "{";
			bool firstSym = true;
			for (auto& sym : completeDefinitions) {

				std::stringstream json;
				json_string(json, sym.first) << ": {";

				bool has_docs = false;

				bool first = true;
				for (auto& md : sym.second.metadata) {
					if (md.second.size()) {
						has_docs = true;
						if (first) first = false;
						else json << ", ";

						json_string(json, md.first) << ": ";
						json_string(json, md.second);
					}
				}
				json << "}";

				if (has_docs) {
					if (!firstSym) json_doc << ",\n";
					else firstSym = false;

					json_doc << json.rdbuf();
				}
			}

			// dump special forms
			for (auto b : builtins) {
				auto md = builtin_metadata.equal_range(b.first);
				json_doc << ",\n";
				json_string(json_doc, ":" + b.first) << ": {";
				for (auto i = md.first; i != md.second; ++i) {
					if (i != md.first) json_doc << ", ";
					json_string(json_doc, i->second.first) << ":";
					json_string(json_doc, i->second.second);
				}
				json_doc << "}";

			}


			json_doc << "}\n";
		}

/*		void Repository::dump_kernel_functions(std::ostream& faux_kernel) {
			std::set<int> kernelChangesets;
			for (auto &cs : changesets) {
				if (cs.first == "#kernel") {
					kernelChangesets.emplace(cs.second.sequence);
				}
			}

			std::map<std::string, std::string> packageHierarchy;

			for (auto &sym : symbols) {
				for (auto kcs : kernelChangesets) {
					auto c = sym.second.find(kcs);
					if (c != sym.second.end()) {
						auto lastColon = sym.first.find_last_of(':');
						assert(lastColon != sym.first.npos && "All qualified names must have a colon");
						std::string inPackage{ sym.first.substr(1,lastColon) };
						std::string symbol{ sym.first.substr(lastColon + 1) };
						
						std::stringstream fauxDef;
						if (c->second.is_defn) {
							for (auto form : c->second.metadata) {
								std::string doc = form.second;
								for (auto &c : doc) if (c == '\n') c = ' ';
								fauxDef << symbol << "(" << form.first << ") { \n"
									<< "\t;; " << form.second << "\n\tRaise(\"no implementation\")\n}\n\n";
							}
						} else {
							fauxDef << "; constant " << symbol << "\n\n";
						}

						packageHierarchy[inPackage] += fauxDef.str();
					}
				}
			}

			faux_kernel << 
				"; Kronos kernel function stubs\n"
				"; This file is only intended to allow klangsrv and related tools to see the built-ins.\n\n";

			for (auto& pack : packageHierarchy) {
				std::string newline = "\n";
				auto sym = pack.first;
				if (sym.size()) {
					sym.pop_back();
					faux_kernel << "Package " << sym << " { ";
					newline = "\n\t";
				}

				std::stringstream content{ pack.second };
				for (std::string line;std::getline(content, line);) {
					faux_kernel << newline << line;
				}

				if (pack.first.size()) faux_kernel << "\n}\n\n";				
			}

			// dump special forms
			for (auto b : builtins) {
				auto range = builtin_metadata.equal_range(b.first);
				if (range.first == range.second) {
					faux_kernel << "; no documentation for " << b.first << "\n\n";
				}
				for (auto i = range.first;i != range.second;++i) {
					faux_kernel << b.first << i->second.first << " {\n\t;; "
						<< i->second.second
						<< "\n\tRaise(\"no implementation\")\n}\n\n";
				}
			}
		}*/
	}
}