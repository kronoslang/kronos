#pragma once

#include "lithe.h"
#include "variant.h"

#include <cassert>
#include <memory>
#include <string>
#include <functional>
#include <unordered_map>
#include <array>

namespace lithe {
	using std::function;
	using std::move;

	namespace detail {
		template <typename... XS> struct static_list {};

		template <typename X, typename... XS> struct static_list<X, XS...> {
			X first;
			static_list<XS...> rest;
		};

		static static_list<> make_list() {
			return { };
		};

		template <typename X, typename... XS>
		static auto make_list(X x, XS... xs) {
			return static_list<X,XS...>{ x, make_list(xs...) };
		}
	}

	struct parse_error {
		std::string description;
		const char *parse_point;
	};

	struct ignore_result {};

	struct parse_point {
		cursor& current;
		cursor limit;
	};

	namespace producers {
		template <typename TResult>
		struct lexer {
			using success_t = TResult;
			using output_t = sum_type<success_t, parse_error>;

			rule parser;
			function<success_t(node)> reader;

			output_t read(parse_point pt) const {
				auto tmp = (*parser)(pt.current, pt.limit);
				if (tmp.is_error()) return parse_error{ tmp.get_string(), pt.current };
				else return reader(tmp);
			};

			rules::dispatch_set dispatch_entries() const {
				return parser->dispatch_entries();
			}

			bool is_optional() const {
				return parser->is_optional();
			}
		};

		static lexer<ignore_result> skip(rule r) {
			return { r, [](node) { return ignore_result{}; } };
		}

		template <typename TCvt>
		static auto convert(rule r, TCvt cvt) {
			return lexer<decltype(cvt(node{}))>{r, cvt};
		}

		template <typename TResult, typename TConstructor, typename... TAdapters>
		struct sequence {
			using success_t = TResult;
			using output_t = sum_type<success_t, parse_error>;
			using seq_t = detail::static_list<TAdapters...>;
			using cc_t = TConstructor; // function<success_t(typename TAdapters::success_t...)>;

			cc_t cc;
			seq_t adapters;

			rules::dispatch_set sequence_entries(detail::static_list<>) const {
				return {};
			}

			template <typename TList> rules::dispatch_set sequence_entries(const TList& list) const {
				auto ds = list.first.dispatch_entries();
				if (list.first.is_optional()) {
					auto ds2 = sequence_entries(list.rest);
					ds.insert(ds2.begin(), ds2.end());
				}
				return ds;
			}

			bool sequence_optional(detail::static_list<>) const {
				return true;
			}

			template <typename TList> bool sequence_optional(const TList& list) const {
				return list.first.is_optional() && sequence_optional(list.rest);
			}

			rules::dispatch_set dispatch_entries() const {
				return sequence_entries(adapters);
			}

			bool is_optional() const {
				return sequence_optional(adapters);
			}

			template <typename TSeq, typename... TArgs>
			output_t doseq(parse_point pt, const TSeq& seq, TArgs&&... args) const {
				using res_t = typename detail::distinct_sum_type<success_t, parse_error>::type;
				return seq.first.read(pt).dispatch([&](auto&& reified) -> output_t {
					return relay(pt, seq, move(reified), move(args)...);
				});
			}

			template <typename TSeq, typename TArg, typename... TArgs>
			output_t relay(parse_point pt, const TSeq& seq, TArg&& arg, TArgs&&... args) const {
				return doseq(pt, seq.rest, std::forward<TArgs>(args)..., move(arg));
			}

			template <typename TSeq, typename... TArgs>
			output_t  relay(parse_point, const TSeq&, parse_error err, TArgs&&...) const {
				return err;
			}

			template <typename TSeq, typename... TArgs>
			output_t  relay(parse_point pt, const TSeq& seq, ignore_result, TArgs&&... args) const {
				return doseq(pt, seq.rest, std::forward<TArgs>(args)...);
			}

			template <typename... TArgs>
			output_t  doseq(parse_point pt, detail::static_list<>, TArgs&&... args) const {
				return cc(std::forward<TArgs>(args)...);
			}

			output_t read(parse_point pt) const {
				return doseq(pt, adapters);
			}
		};

		template <typename TColl, typename TInserter, typename TValue>
		struct loop_base {
			using success_t = TColl;
			using output_t = sum_type<success_t, parse_error>;
			rule begin_rule;
			TInserter insert_fn;
			TValue insert_adapter;
			rule end_rule;
			rule ignore_rule;

			loop_base(rule br, TInserter inf, TValue insa, rule er, rule ir)
				:begin_rule(br), insert_fn(inf), insert_adapter(insa), end_rule(er), ignore_rule(ir) {}


			rules::dispatch_set dispatch_entries() const {
				auto ds = begin_rule->dispatch_entries();
				if (begin_rule->is_optional()) {
					auto ds2 = insert_adapter.dispatch_entries();
					ds.insert(ds2.begin(), ds2.end());
					ds2 = end_rule->dispatch_entries();
					ds.insert(ds2.begin(), ds2.end());
				}
				return ds;
			}

			bool is_optional() const {
				return begin_rule->is_optional() && end_rule->is_optional();
			}
		};

		template <typename TColl, typename TInserter, typename TValue, bool TContinuation> struct loop_adapter;

		template <typename TColl, typename TInserter, typename TValue>
		struct loop_adapter<TColl, TInserter, TValue, true> : public loop_base<TColl, TInserter, TValue> {
			loop_adapter(rule br, TInserter inf, TValue insa, rule er, rule ir)
				:loop_base(br, inf, insa, er, ir) {}

			output_t read(parse_point pt) const {
				auto tmp = (*begin_rule)(pt.current, pt.limit);
				if (tmp.is_error()) return parse_error{ tmp.get_string(), pt.current };

				success_t coll;
				TValue ins = insert_adapter;

				ins.cc = [this, &coll](auto&&... args) {
					insert_fn(coll, std::forward<decltype(args)>(args)...);
					return ignore_result{};
				};

				for (bool first = true;;first = false) {
					auto old = pt.current;
					tmp = (*end_rule)(pt.current, pt.limit);
					if (!tmp.is_error()) return coll;

					pt.current = old;

					if (!first && ignore_rule) {
						tmp = (*ignore_rule)(pt.current, pt.limit);
						if (tmp.is_error()) return parse_error{ tmp.get_string(), pt.current };
					}

					auto err = ins.read(pt);
					if (err.is<parse_error>()) return err.get<parse_error>();
				}
			}
		};

		template <typename TColl, typename TInserter, typename TValue>
		struct loop_adapter<TColl, TInserter, TValue, false> : public loop_base<TColl, TInserter, TValue> {
			loop_adapter(rule br, TInserter inf, TValue insa, rule er, rule ir)
				:loop_base(br, inf, insa, er, ir) {}

			template <typename T>
			parse_error insert(TColl& coll, T&& elem) const {
				insert_fn(coll, std::move(elem));
				return { "", nullptr };
			}

			parse_error insert(TColl&, parse_error pe) const {
				return pe;
			}

			output_t read(parse_point pt) const {
				auto tmp = (*begin_rule)(pt.current, pt.limit);
				if (tmp.is_error()) return parse_error{ tmp.get_string(), pt.current };

				success_t coll;

				for (bool first = true;;first = false) {
					auto old = pt.current;
					tmp = (*end_rule)(pt.current, pt.limit);
					if (!tmp.is_error()) return coll;

					pt.current = old;

					if (!first && ignore_rule) {
						tmp = (*ignore_rule)(pt.current, pt.limit);
						if (tmp.is_error()) return parse_error{ tmp.get_string(), pt.current };
					}

					auto err = insert_adapter.read(pt).dispatch([&](auto&& reified) -> parse_error {
						return insert(coll, std::move(reified));
					});

					if (err.description.size()) return err;					
				}
			}
		};

		template <typename... TResults> struct any_of2 {
			using success_t = sum_type<TResults...>;
			using output_t = sum_type<success_t, parse_error>;
			using dispatch_t = function<output_t(parse_point)>;
			using table_t = std::array<std::vector<dispatch_t>, 256>;

			std::shared_ptr<table_t> table = std::make_shared<table_t>();
			bool optional = true;

			void populate_table(detail::static_list<>) { }

			template <typename TList> void populate_table(const TList& tl) {
				dispatch_t dispatch = [rule = tl.first](parse_point pt) -> output_t {
					auto tmp = rule.read(pt);
					if (tmp.is<parse_error>()) {
						return std::move(tmp.get<parse_error>());
					} else {
						using rule_t = decltype(rule);
						return (success_t)move(tmp.get<typename rule_t::success_t>());
					}
				};

				optional = optional && tl.first.is_optional();

				for (auto de : tl.first.dispatch_entries()) {
					(*table)[de].emplace_back(dispatch);
				}

				populate_table(tl.rest);
			}

			template <typename... TAlts>
			any_of2(TAlts... alts) {
				populate_table(detail::make_list(alts...));
			}

			bool is_optional() const {
				return optional;
			}

			rules::dispatch_set dispatch_entries() const {
				rules::dispatch_set ds;
				for (int = 0;i < 256;++i) {
					if (!(*table)[i].empty()) ds.emplace(i);
				}
				return ds;
			}

			output_t read(parse_point pt) const {
				output_t tmp = parse_error{};
				const table_t& dt(*table);
				unsigned char dchar = pt.current[0];
				for (auto &o : dt[dchar]) {
					auto old = pt.current;
					tmp = o(pt);
					if (!tmp.is<parse_error>()) break;
					pt.current = old;
				}
				return std::move(tmp);
			}
		};

		template <typename... TAlts> struct any_of {
			detail::static_list<TAlts...> options;
			using success_t = sum_type<typename TAlts::success_t...>;
			using output_t = sum_type<success_t, parse_error>;

			any_of(TAlts... alts) :options(detail::make_list(alts...)) {}

			output_t doseq(detail::static_list<>, parse_point pt, const parse_error& pe) const {
				return pe;
			}

			template <typename TList>
			output_t doseq(const TList& tl, parse_point pt, const parse_error& pe) const {
				auto old = pt.current;
				auto tmp = tl.first.read(pt);
				if (tmp.is<parse_error>()) {
					pt.current = old;
					return doseq(tl.rest, pt, tmp.get<parse_error>());
				} else {
					using first_t = decltype(tl.first);
					return (success_t)move(tmp.get<typename first_t::success_t>());
				}
			}

			output_t read(parse_point pt) const {
				return doseq(options, pt, {});
			}
		};

		template <typename TResult> struct recursive {
			using success_t = TResult;
			using output_t = sum_type<success_t, parse_error>;

			using fn_t = function<output_t(parse_point)>;

			std::shared_ptr<fn_t> indirect = std::make_shared<fn_t>();

			template <typename TRule>
			void assign(TRule r) {
				*indirect = [r = move(r)](parse_point pt) -> output_t {
					auto tmp = r.read(pt);
					if (tmp.is<parse_error>()) {
						return tmp.get<parse_error>();
					} else {
						return (success_t)std::move(tmp.get<typename TRule::success_t>());
					}
				};
			}

			output_t read(parse_point pt) const {
				assert(indirect && " incomplete recursive definition");
				return (*indirect)(pt);
			}

			bool is_optional() const {
				return true;
			}

			rules::dispatch_set dispatch_entries() const {
				rules::dispatch_set ds;
				for (int i = 0;i < 256;++i) {
					ds.emplace(i);
				}
				return ds;
			}
		};
	}

	namespace detail {
		template <typename T>
		struct fn_traits : public fn_traits<decltype(&T::operator())> {};

		template <typename T>
		struct fn_traits<const T> : public fn_traits<T> {};

		template <typename TClass, typename TReturn, typename... TArgs>
		struct fn_traits<TReturn(TClass::*)(TArgs...) const> {
			using arg_t = std::tuple<TArgs...>;
			using fn_t = function<TReturn(TArgs...)>;
		};

		template <typename TReturn, typename... TArgs>
		struct fn_traits<TReturn(*)(TArgs...)> {
			using arg_t = std::tuple<TArgs...>;
			using fn_t = function<TReturn(TArgs...)>;
		};

		template <typename TConstructor, typename TTuple, typename... TFiltered> struct faux_constructor;

		template <typename TConstructor, typename TArg, typename... TArgs, typename... TFiltered>
		struct faux_constructor<TConstructor, static_list<TArg, TArgs...>, TFiltered...> {
			using next_t = faux_constructor<TConstructor, static_list<TArgs...>, TFiltered..., TArg>;
			using success_t = typename next_t::success_t;
			using fn_t = typename next_t::fn_t;
		};

		template <typename TConstructor, typename... TArgs, typename... TFiltered>
		struct faux_constructor<TConstructor, static_list<ignore_result, TArgs...>, TFiltered...> {
			using next_t = faux_constructor<TConstructor, static_list<TArgs...>, TFiltered...>;
			using success_t = typename next_t::success_t;
			using fn_t = typename next_t::fn_t;
		};

		template <typename TConstructor, typename... TFiltered>
		struct faux_constructor<TConstructor, static_list<>, TFiltered...> {
			using success_t = decltype(std::declval<TConstructor>()(std::declval<TFiltered>()...));
			using fn_t = function<success_t(TFiltered...)>;
		};
	}

	namespace producers {
		template <typename TConstructor, typename... TRules>
		static auto constructor(TConstructor cons, TRules... rules) {
			using success_t = typename detail::faux_constructor<TConstructor, detail::static_list<typename TRules::success_t...>>::success_t;
			return sequence<success_t, TConstructor, TRules...>{ cons, rules... };
		}

		template <typename... TRules>
		static auto continuation(TRules... rules) {
			auto dummy_cons = [](auto...) {return ignore_result{}; };
			using fn_t = typename detail::faux_constructor<decltype(dummy_cons), detail::static_list<typename TRules::success_t...>>::fn_t;
			return sequence<ignore_result, fn_t, TRules...>{ {}, rules... };
		}

		template <typename TRule>
		static auto reader(const TRule& r, std::string txt) {
			auto beg = txt.data();
			return r.read(parse_point{ beg, beg + txt.size() });
		}

		template <class TCollection, typename TInsertFn, typename TValue>
		static auto coll_of(rule begin, TInsertFn insert_fn, TValue value_adapter, rule between, rule end) {
			return loop_adapter<TCollection, TInsertFn, TValue, false>{
				begin,
				insert_fn,
				value_adapter,
				end,
				between
			};
		}

		template <class TCollection, typename TInsertFn, typename Tcc, typename... TValueRules>
		static auto coll_of(rule begin, TInsertFn insert_fn, sequence<ignore_result, Tcc, TValueRules...> value_emplacer, rule between, rule end) {
			return loop_adapter<TCollection, TInsertFn, sequence<ignore_result, Tcc, TValueRules...>, true> {
				begin,
				insert_fn,
				value_emplacer,
				end,
				between
			};
		}

		template <typename TKey, typename TValue, typename... TValueRules>
		static auto map_of(rule begin, rule between, rule end, TValueRules... rules) {
			using map_t = std::unordered_map<TKey, TValue>;
			return coll_of<map_t>(begin, [](map_t& m, TKey&& key, TValue&& value) {
				m.emplace(std::move(key), std::move(value));
				return ignore_result{};
			}, continuation(rules...), between, end);
		}

		template <typename... TAlts>
		static auto any(TAlts... ta) {
			return any_of2<typename TAlts::success_t...>(move(ta)...);
		}

		template <typename TSumType>
		static auto variant(std::unordered_map<const char*, function<TSumType(node)>> dispatch) {
			auto converter = [d = move(dispatch)](node n) -> TSumType {
				return d[n.strbeg](n);
			};
		}

		template <typename TColl>
		auto back_emplacer() {
			using val_t = typename TColl::value_type;
			return [](TColl& coll, val_t&& val) {
				coll.emplace_back(std::move(val));
				return ignore_result{};
			};
		}

		template <typename TColl>
		auto emplacer() {
			using val_t = typename TColl::value_type;
			return [](TColl& coll, val_t&& val) {
				coll.emplace(std::move(val));
				return ignore_result{};
			};
		}

		template <class TAssoc>
		auto assoc_emplacer() {
			using key_t = typename TAssoc::key_type;
			using val_t = typename TAssoc::mapped_type;
			return [](TAssoc& coll, key_t&& key, val_t&& val) {
				coll.emplace(std::move(key), std::move(val));
				return ignore_result{};
			};
		}
	}

	static std::ostream& operator<<(std::ostream& s, const parse_error& fe) {
		s << fe.description << std::endl;
		return s;
	}
}