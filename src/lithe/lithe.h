#pragma once

#include <memory>
#include <functional>
#include <unordered_set>

#include "ast.h"

namespace lithe {
	using std::shared_ptr;
	using cursor = const char*;

#ifndef NDEBUG
	extern bool trace;
#endif

	namespace rules {
		struct interface;
		using dispatch_set = std::unordered_set<unsigned char>;

		struct or_;
		struct seq_;
		struct char_class;

		struct interface : public error_report {
            virtual ~interface() {};
			virtual dispatch_set dispatch_entries() const = 0;
			virtual node operator()(cursor& current, cursor limit) const = 0;
			virtual const or_* as_or() const { return nullptr; }
			virtual const seq_* as_seq() const { return nullptr; }
			virtual const char* get_tag() const { return nullptr; }
			virtual const char_class* as_char_class() const { return nullptr; }
			virtual bool is_optional() const { return false; }
			virtual shared_ptr<const interface> ignore(shared_ptr<const interface>) const;
			inline node parse(const std::string& str) const {
				cursor beg; cursor end;
				beg = str.data(); end = str.data() + str.size();
				auto result = operator()(beg, end);
				if (result.is_error()) {
					if (result.children.empty()) result.children.push_back(node(beg));
					else result[0].strbeg = beg;
				}
				return result;
			}
		};

		struct recursive : public interface {
			virtual void assign(shared_ptr<const interface> from) = 0;
		};
	}

	using rule = shared_ptr<const rules::interface>;
	using recursive_rule = shared_ptr<rules::recursive>;

	rule characters(const char *name, const char *characters, bool invert = false, unsigned minimum = 1, unsigned maximum = 0);
	rule characters(const char *name, int(*pred)(int), bool invert = false, unsigned minimum = 1, unsigned maximum = 0);
	rule recursive(const char *tag);
	rule E(const char *tag, rule content);
	rule IE(const char *tag, rule content);
	rule T(const char *literal_text);
	rule I(rule ignore);
	rule L(const char* label, rule content);
	static inline rule I(const char *ignore) { return I(T(ignore)); }
	rule O(rule r);
	rule repeat(rule element, int minimum = 1);
	rule for_(rule element, rule inbetween, rule end);
	rule require(const char *explain, rule required);
	rule custom(const char *name, rule upstream, std::function<node(node)>);
	rule end();
	rule bad(const char *error, rule);

	enum class callback_stage {
		preprocess,
		postprocess
	};

	rule callback(const char *name, rule upstream, std::function<void(callback_stage, cursor&, cursor)>);
	rule peek(rule p);
	recursive_rule recursive();

	rule operator<<(rule a, rule b);
	rule operator| (rule a, rule b);
}
