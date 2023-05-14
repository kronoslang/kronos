#pragma once
#include <vector>
#include <ostream>

namespace lithe {
	using cursor = const char*;

	struct error_report {
		virtual void write(std::ostream&) const = 0;
	};

	struct node {
		node(const char *strbeg = nullptr, const char *strend = nullptr) :strbeg(strbeg), strend(strend) {}
		const char *strbeg;
		const char *strend;

		const char* src_begin{ nullptr };
		const char* src_end{ nullptr };
#define LITHE_EXTENT_BEGIN(n, p) (n).src_begin = p;
#define LITHE_EXTENT_END(n, p) (n).src_end = p;

		std::vector<node> children;

		std::string get_string() const;
		int size() const;
		const node& get_child(int i) const;
		std::string get_header() const;
		bool is_error() const;
		bool is_fatal() const;
		void set_fatal();

		node& operator[](size_t i) { return children[i]; }
		const node& operator[](size_t i) const { return children[i]; }

		static node error(const error_report* er, const char *parse_pt, node child = {});
		void to_stream(std::ostream& s, int indent = 0) const;
	};

	static inline std::ostream& operator<<(std::ostream& s, const node& n) {
		n.to_stream(s, 0); return s;
	}
}