#include "lithe.h"
#include <cassert>
#include <vector>
#include <array>
#include <cstring>

#ifndef NDEBUG
#include <iostream>
#include <string>
static thread_local std::string trace_indent;
struct indent {
	indent() { trace_indent.push_back(' '); }
	~indent() { trace_indent.pop_back(); }
	static const std::string& current() { return trace_indent; }
};
namespace lithe {
	bool trace = false;
}
#define LITHE_TRACE
#endif

namespace lithe {
	
	using std::vector;
	using std::make_shared;

	namespace rules {
		struct or_ : public interface {
			vector<rule> options;
			std::array<vector<const interface*>, 256> table;

			or_(const or_&) = delete;

			const or_* as_or() const override { 
				return this; 
			}

			dispatch_set dispatch_entries()  const override {
				dispatch_set accum;
				for (auto &o : options) {
					auto sub = o->dispatch_entries();
					accum.insert(sub.begin(), sub.end());
				}
				return accum;
			}

			node operator()(cursor& current, cursor limit)  const override {
				cursor start = current;
				node tmp;
				LITHE_EXTENT_BEGIN(tmp, current)
				for (auto o : table[(unsigned char)*current]) {
					tmp = (*o)(current, limit);
					if (!tmp.is_error() || tmp.is_fatal()) {
						LITHE_EXTENT_END(tmp,current);
						return tmp;
					}
					current = start;
				}
				return node::error(this, current, tmp);
			}

			or_(rule a, rule b) {
				auto aor = a->as_or();
				auto bor = b->as_or();
				if (aor) {
					options = aor->options;
					table = aor->table;

				} else {
					options.emplace_back(a);
					auto de = a->dispatch_entries();
					for (unsigned char c : de) {
						table[c].emplace_back(a.get());
					}
				}

				if (bor) {
					for (int i = 0;i < 256;++i) {
						table[i].insert(table[i].end(), bor->table[i].begin(), bor->table[i].end());
					}
					options.insert(options.end(), bor->options.begin(), bor->options.end());
				} else {
					options.emplace_back(b);
					auto de = b->dispatch_entries();
					for (unsigned char c : de) {
						table[c].emplace_back(b.get());
					}
				}
			}

			or_(vector<rule> opts):options(std::move(opts)) {
				for (auto &o : options) {
					for (unsigned char c : o->dispatch_entries()) {
						table[c].emplace_back(o.get());
					}
				}
			}


			void write(std::ostream& os) const override {
				os << "either ";
				for (size_t i = 0;i < options.size();++i) {
					if (i) {
						if (i < options.size() - 1) {
							os << ", ";
						} else {
							os << " or ";
						}
					}
					options[i]->write(os);
				}
			}
		};

		static void flatten_to(vector<node>& dest, const node& src) {
			if (src.strbeg) {
				dest.emplace_back(src);
			} else {
				dest.reserve(dest.size() + src.children.size());
				for (auto &c : src.children) {
					flatten_to(dest, c);
				}
			}
		}

		struct seq_ : public interface {
			vector<rule> sequence;
			const seq_* as_seq()  const override { return this; }

			dispatch_set dispatch_entries()  const override {
				dispatch_set result;
				for (auto &s : sequence) {
					auto sub = s->dispatch_entries();
					result.insert(sub.begin(), sub.end());
					if (s->is_optional() == false) break;

				}
				return result;
			}

			bool is_optional() const override {
				for (auto &s : sequence) if (!s->is_optional()) return false;
				return true;
			}

			node complex_seq(cursor& current, cursor limit, node result, size_t i) const {
				for (;i < sequence.size();++i) {
					node tmp = (*sequence[i])(current, limit);
					if (tmp.is_error()) {
						if (tmp.is_fatal()) return tmp;
						else return node::error(this, current, tmp);
					}
					flatten_to(result.children, tmp);
				}
				LITHE_EXTENT_END(result, current);
				return result;
			}

			node operator()(cursor& current, cursor limit)  const override {
				assert(!sequence.empty());
				auto beg = current;
				size_t i = 0;
				node str;
				while (i < sequence.size()) {
					str = (*sequence[i++])(current, limit);
					if (str.is_error()) {
						if (str.is_fatal()) {
							return str;
						} else {
							return node::error(this, current, str);
						}
					}
					if (str.strbeg || str.children.size()) break;
				}

				if (!str.children.empty()) {
					node result;
					LITHE_EXTENT_BEGIN(result, beg)
					flatten_to(result.children, str);
					return complex_seq(current, limit, std::move(result), i);
				}

				while (i < sequence.size()) {
					node tmp = (*sequence[i++])(current, limit);
					if (tmp.is_error()) {
						if (tmp.is_fatal()) return tmp;
						else return node::error(this, current, tmp);
					}

					if (tmp.children.empty()) {
						if (tmp.strbeg == tmp.strend) continue;
						if (tmp.strbeg == str.strend) {
							str.strend = tmp.strend;
							continue;
						}
					} 

					node result;
					LITHE_EXTENT_BEGIN(result, beg)
					flatten_to(result.children, str);
					flatten_to(result.children, tmp);
					return complex_seq(current, limit, std::move(result), i);
				}

				return str;
			}

			seq_(rule a, rule b) {
				auto aseq = a->as_seq();
				auto bseq = b->as_seq();
				if (aseq) {
					sequence = aseq->sequence;
				} else {
					sequence.emplace_back(a);
				}

				if (bseq) {
					sequence.insert(sequence.end(), bseq->sequence.begin(), bseq->sequence.end());
				} else {
					sequence.emplace_back(b);
				}
			}

			void write(std::ostream& os)  const override {
				//os << "sequence of ";
				for (size_t i = 0;i < sequence.size();++i) {
					if (i) {
						if (i < sequence.size() - 1) {
							os << ", ";
						} else {
							os << " and ";
						}
					}
					sequence[i]->write(os);
				}
			}
		};

		struct term : public interface {
			const char *tag;
			rule content;
			bool insert;
			
			term(const char *t, rule content, bool insert_tag) :tag(t),content(content),insert(insert_tag) {}

			dispatch_set dispatch_entries()  const override {
				return content->dispatch_entries();
			}

			node operator()(cursor& current, cursor limit)  const override {
#ifdef LITHE_TRACE
				if (trace) {
					std::clog << indent::current() << "[" << tag << "] ";
					for (cursor i = current; i < limit; ++i) {
						if (*i == '\n') break;
						if (i > current + 10) break;
						std::clog.put(*i);
					}
					std::clog << std::endl;
				}
				indent bump;
#endif
				auto start = current;
				auto c = (*content)(current, limit);
				if (c.is_error()) {
#ifdef LITHE_TRACE
					if (trace) {
						std::clog << indent::current() << "* Mismatch [" << tag << "]\n";
					}
#endif
					if (c.is_fatal()) {
						return c;
					}
					current = start;
					return node::error(this, current, c);
				}
#ifdef LITHE_TRACE
				if (trace) {
					std::clog << indent::current() << "% Matched [" << tag << "]!\n";
				}
#endif
				assert(c.is_error() || (c.src_begin && c.src_end));
				if (!insert) return c;
				if (c.strbeg) {
					node tmp;
					LITHE_EXTENT_BEGIN(tmp, c.src_begin)
					LITHE_EXTENT_END(tmp,c.src_end);
					tmp.strbeg = tag;
					tmp.children.emplace_back(std::move(c));
					return tmp;
				} else {
					c.strbeg = tag;
					return c;
				}
			}

			void write(std::ostream& os) const override {
				os << "[" << tag << "]";
			}
		};

		struct literal : public interface {
			const char *str;
		public:
			literal(const char *s) : str(s) {}
			
			dispatch_set dispatch_entries()  const override {
				return { (unsigned char)*str };
			}

			node operator()(cursor& current, cursor limit)  const override {
				node tmp;
				tmp.strbeg = current;
				LITHE_EXTENT_BEGIN(tmp, current);
				for (auto p = str; *p; ++current, ++p) {
					if (current >= limit || *p != *current) {
						return node::error(this, current);
					}
				}

				tmp.strend = current;
				LITHE_EXTENT_END(tmp, current);
				return tmp;
			}

			void write(std::ostream& os) const override {
				os << "\'" << str << "\'";
			}
		};

		struct wrapper : public interface {
			rule r;
            const char *name;
			wrapper(rule r, const char *name = nullptr) :r(r),name(name) {}

			bool is_optional() const override {
				return r->is_optional();
			}

			dispatch_set dispatch_entries()  const override {
				return r->dispatch_entries();
			}

			node operator()(cursor& current, cursor limit)  const override {
				return (*r)(current, limit);
			}

			void write(std::ostream& os) const override {
				if (name) os << name;
				else r->write(os);
			}
		};

		struct require : public wrapper {
			const char *reason;
			require(const char *expl, rule r) :wrapper(r),reason(expl) {}

			node operator()(cursor& current, cursor limit)  const override {
				auto begin = current;
				auto tmp = wrapper::operator()(current, limit);
				if (tmp.is_error()) {
					if (tmp.is_fatal()) return tmp;
					auto err = node::error(this, current, tmp);
					current = begin;
					err.set_fatal();
					return err;
				}
				return tmp;
			}

			void write(std::ostream& os) const override {
				os << reason;
			}
		};

		struct bail : public wrapper {
			const char *reason;
			bail(const char *expl, rule r) :wrapper(r), reason(expl) {}

			node operator()(cursor& current, cursor limit)  const override {
				auto tmp = wrapper::operator()(current, limit);
				if (tmp.is_error()) {
					return {};
				}
				tmp = node::error(this, current);
				tmp.set_fatal();
				return tmp;
			}

			void write(std::ostream& os) const override {
				os << reason;
			}
		};

		struct repeat : public interface {
			rule r;
			int minimum;
			repeat(rule r, int min) :r(r), minimum(min) {}

			bool is_optional() const override {
				return minimum == 0 || r->is_optional();
			}

			dispatch_set dispatch_entries()  const override {
				return r->dispatch_entries();
			}

			node operator()(cursor& current, cursor limit)  const override {
				node result;
				LITHE_EXTENT_BEGIN(result, current)
				for (int matches = 0;;++matches) {
					cursor pos = current;
					auto tmp = (*r)(current, limit);
					if (tmp.is_error()) {
						if (tmp.is_fatal()) return tmp;
						current = pos;
						if (matches < minimum) return node::error(this, current, tmp);
						else break;
					}
					result.children.emplace_back(tmp);
				}
				LITHE_EXTENT_END(result,current);
				return result;
			}

			void write(std::ostream& os)  const override {
				switch (minimum) {
				case 0: os << "zero"; break;
				case 1: os << "one"; break;
				case 2: os << "two"; break;
				case 3: os << "three"; break;
				case 4: os << "four"; break;
				default: os << minimum; break;
				}
				os << " or more of ";
				r->write(os);
			}
		};

		struct for_ :public interface {
			rule body, end, iterator;
			for_(rule b, rule e, rule i) :body(b), end(e), iterator(i) { }

			dispatch_set dispatch_entries() const override {
				auto es = end->dispatch_entries();
				auto bs = body->dispatch_entries();
				bs.insert(es.begin(), es.end());
				return bs;
			}

			node operator()(cursor& current, cursor limit) const override {
				node result;
				LITHE_EXTENT_BEGIN(result, current)
				for (bool first = true;;first = false) {
					cursor at = current;
					node tmp = (*end)(current, limit);
					if (tmp.is_error()) {
						if (tmp.is_fatal()) return tmp;
						current = at;
					} else {
						flatten_to(result.children, tmp);
						LITHE_EXTENT_END(result,current);
						return result;
					}

					if (!first && iterator) {
						tmp = (*iterator)(current, limit);
						if (tmp.is_error()) {
#ifdef LITHE_TRACE
							if (trace) {
								std::clog << indent::current() << "missing iterator: " << std::string(at, current).substr(0,10) << "\n";
							}
#endif

							tmp.set_fatal();
							return tmp;
						}
						flatten_to(result.children, tmp);
					}

					at = current;
					tmp = (*body)(current, limit);

					if (tmp.is_error()) {
						tmp.set_fatal();
						return tmp;
					}
#ifdef LITHE_TRACE
					if (trace) {
						std::clog << indent::current() << "* Matched '" << std::string(at, current).substr(0, 10) << "...'\n";
					}
#endif

					flatten_to(result.children, tmp);
				}
			}

			void write(std::ostream& os) const override {
				body->write(os);
				if (iterator) {
					os << " alternating with ";
					iterator->write(os);
				}
				os << " ending in ";
				end->write(os);				
			}
		};

		struct optional : public wrapper {
			std::array<char, 256> can_work;

			optional(rule r) :wrapper(r) {
				memset(can_work.data(), 0, sizeof(can_work));
				for (auto c : r->dispatch_entries()) {
					can_work[c] = 1;
				}
			}

			bool is_optional() const override {
				return true;
			}


			node operator()(cursor& current, cursor limit)  const override {
				if (!can_work[(unsigned char)current[0]]) {
					node tmp;
					LITHE_EXTENT_BEGIN(tmp, current)
					LITHE_EXTENT_END(tmp, current);
					return tmp;
				}
				cursor start = current;
				node tmp = wrapper::operator()(current, limit);
				if (tmp.is_error() && !tmp.is_fatal()) {
					current = start;
					tmp = {};
				}
				return tmp;
			}

			void write(std::ostream& os)  const override {
				os << "(optional ";
				wrapper::write(os);
				os << ")";
			}
		};

		struct char_class : public interface {
			std::array<bool,256> in_class;
			const char *name;

			const char_class* as_char_class() const override {
				return this;
			}

			bool is_optional() const override {
				return minimum < 1;
			}

			char_class(const char *name):name(name) {}

			size_t minimum, maximum;
			
			char_class(const char *name, const char *str, bool invert, unsigned minimum, unsigned maximum)
				:name(name),minimum(minimum),maximum(maximum) 
			{
				for (auto &ic : in_class) ic = invert;
				while (*str) {
					in_class[(unsigned char)*str++] = !invert;
				}
			}

			char_class(const char *name, int(*pred)(int), bool invert, unsigned minimum, unsigned maximum)
				:name(name),minimum(minimum),maximum(maximum)
			{
				for (int i = 0;i < 256;++i) {
					in_class[i] = invert != (pred(i) != 0);
				}
			}

			dispatch_set dispatch_entries()  const override {
				dispatch_set s;
				for (int i = 0;i < 256;++i) {
					if (minimum < 1 || in_class[i]) s.emplace(i);
				}
				return s;
			}

			node operator()(cursor& current, cursor limit)  const override {
				node tmp;
				tmp.strbeg = current;
				LITHE_EXTENT_BEGIN(tmp, current)

				if (maximum != 0) {
					auto avail = limit - current;
					if (avail > maximum) avail = maximum;
					limit = current + avail;
				}

				while (current < limit && in_class[(unsigned char)*current]) {
					++current;
				}
				if (current >= tmp.strbeg + minimum) {
					tmp.strend = current;
					LITHE_EXTENT_END(tmp,current);
					return tmp;
				} else {
					return node::error(this, current);
				}
			}

			void write(std::ostream& s)  const override { s << name; }
		};

		struct ignore : public wrapper {
			ignore(rule r):wrapper(r) {}

			node operator()(cursor& current, cursor limit)  const override {
				auto b = current;
				auto tmp = wrapper::operator()(current, limit);
				if (tmp.is_error()) return tmp;
				node i;
				LITHE_EXTENT_BEGIN(i, b)
				LITHE_EXTENT_END(i,current);
				return i;
			}
		};

		struct indirect : public recursive {
			rule r;
			mutable bool cyclic_dispatch = false;
		public:
			dispatch_set dispatch_entries()  const override {
				if (r && !cyclic_dispatch) {
					cyclic_dispatch = true;
					auto result = r->dispatch_entries();
					cyclic_dispatch = false;
					return result;
				} else {
					// don't know the rule yet, so dispatch everything to this
					dispatch_set s;
					for (int i = 0;i < 256;++i) s.emplace(i);
					return s;
				}
			}

			node operator()(cursor& current, cursor limit)  const override {
				assert(r && "grammar has an undefined recursive rule");
				return (*r)(current, limit);
			}

			void write(std::ostream& s)  const override {
				assert(r && "grammar has an undefined recursive rule");
				r->write(s);
			}

			void assign(rule from) override {
				assert(!r && "recursive rule already defined");
				std::swap(r, from);
			}
		};

		struct custom : public wrapper {
			std::function<node(node)> process;
			custom(const char *name, rule r, std::function<node(node)> process):wrapper(r, name),process(process) {
			}

			node operator()(cursor& cur, cursor end) const override {
				auto b = cur;
				auto tmp = wrapper::operator()(cur, end);
				if (tmp.is_error()) return tmp;
				assert(tmp.src_begin && tmp.src_end);
				auto tmp2 = process(std::move(tmp));
				assert(tmp2.is_error() || (tmp2.src_begin && tmp2.src_end));
				return tmp2;
			}
		};

		struct callback : public wrapper {
			std::function<void(callback_stage, cursor&, cursor)> hook;
			callback(const char *name, rule r, std::function<void(callback_stage, cursor&, cursor)> cb) :wrapper(r, name), hook(cb) {}

			node operator()(cursor& begin, cursor end) const override {
				hook(callback_stage::preprocess, begin, end);
				auto tmp = wrapper::operator()(begin, end);
				hook(callback_stage::postprocess, begin, end);
				return tmp;
			}
		};

		struct end_anchor : public interface {
			dispatch_set dispatch_entries() const override {
				return { 0 };
			}

			node operator()(cursor& begin, cursor end) const override {
				if (begin == end) {
					node e;
					LITHE_EXTENT_BEGIN(e, end)
					LITHE_EXTENT_END(e, end)
					return e;
				}
				return node::error(this, begin);
			}

			void write(std::ostream& s) const override {
				s << "end";
			}
		};

		rule interface::ignore(rule self) const {
			assert(self.get() == this);
			return make_shared<const rules::ignore>(self);
		}
	}


	rule operator|(rule a, rule b) {
		auto acc = a->as_char_class();
		auto bcc = b->as_char_class();

		if (acc && bcc && acc->minimum == bcc->minimum &&
			acc->maximum == bcc->maximum) {
			auto cc = make_shared<rules::char_class>("merged");
			for (int i = 0;i < 256;++i) {
				cc->in_class[i] = acc->in_class[i] || bcc->in_class[i];
			}
			cc->minimum = acc->minimum;
			cc->maximum = acc->maximum;
			return cc;
		}

		auto as = a->as_seq();
		auto bs = b->as_seq();
		
		if (as && bs) {
			if (as->sequence.front() == bs->sequence.front()) {
				auto asc = make_shared<rules::seq_>(*as);
				auto bsc = make_shared<rules::seq_>(*bs);
				asc->sequence.erase(asc->sequence.begin());
				bsc->sequence.erase(bsc->sequence.begin());

				if (asc->sequence.empty()) {
					return bsc;
				}
				
				if (bsc->sequence.empty()) {
					return asc;
				}

				return make_shared<rules::seq_>(as->sequence.front(), asc | bsc);
			}
		}

		return make_shared<rules::or_>(a, b);	
	}

	rule operator<<(rule a, rule b) {
		return make_shared<rules::seq_>(a, b);
	}

	rule E(const char *tag, rule content) {
		if (auto or_node = content->as_or()) {
			auto opts = or_node->options;
			for (auto &o : opts) {
				o = make_shared<rules::term>(tag, o, true);
			}
			return make_shared<rules::or_>(opts);
		} else {
			return make_shared<rules::term>(tag, content, true);
		}
	}

	rule IE(const char *tag, rule content) {
		return make_shared<rules::term>(tag, content, false);
	}

	rule T(const char *text) {
		return make_shared<rules::literal>(text);
	}

	rule I(rule r) {
		return r->ignore(r);
	}

	rule L(const char* label, rule r) {
		return make_shared<rules::wrapper>(r, label);
	}

	rule O(rule r) {
		return make_shared<rules::optional>(r);
	}

	rule require(const char* explain, rule r) {
		return make_shared<rules::require>(explain, r);
	}

	rule bad(const char *explain, rule r) {
		return make_shared<rules::bail>(explain, r);
	}

	recursive_rule recursive() {
		return make_shared<rules::indirect>();
	}

	rule repeat(rule r, int minimum) {
		return make_shared<rules::repeat>(r, minimum);
	}

	rule for_(rule body, rule iterator, rule end) {
		return make_shared<rules::for_>(body, end, iterator);
	}

	rule characters(const char *name, const char *chars, bool invert, unsigned int minimum, unsigned int maximum) {
		return make_shared<rules::char_class>(name, chars, invert, minimum, maximum);
	}

	rule characters(const char *name, int(*pred)(int), bool invert, unsigned int minimum, unsigned int maximum) {
		return make_shared<rules::char_class>(name, pred, invert, minimum, maximum);
	}

	rule custom(const char *name, rule child, std::function<node(node)> postprocess) {
		return make_shared<rules::custom>(name, child, postprocess);
	}

	rule callback(const char *name, rule child, std::function<void(callback_stage, cursor&, cursor)> preprocess) {
		return make_shared<rules::callback>(name, child, preprocess);
	}

	rule end() {
		return make_shared<rules::end_anchor>();
	}
}
