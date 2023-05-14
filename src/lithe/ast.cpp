#include "ast.h"
#include <sstream>

namespace lithe {
	static const char *error_tag = "error";
	static const char *fatal_tag = "fatal";

	std::string node::get_string() const {
		std::stringstream ss;
		to_stream(ss);
		return ss.str();
	}

	int node::size() const {
		return (int)children.size();
	}

	const node& node::get_child(int i) const {
		return children[i];
	}

	std::string node::get_header() const {
		if (!strbeg) return "";
		if (strend) return { strbeg, strend };
		else return strbeg;
	}

	bool node::is_error() const { return strbeg == error_tag || strbeg == fatal_tag; }
	bool node::is_fatal() const { return strbeg == fatal_tag; }

	node node::error(const error_report* er, const char* pos, node child) {
		node n;
		n.strbeg = error_tag;
		n.strend = (const char *)er;
		n.children.emplace_back(pos);
		if (child.strbeg || child.size()) n.children.emplace_back(std::move(child));
		return n;
	}

	void node::set_fatal() {
		strbeg = fatal_tag;
	}

	void node::to_stream(std::ostream& s, int indent) const {
		if (strbeg == error_tag || strbeg == fatal_tag) {
			for (int i = 0;i < indent;++i) s.put(' ');

			if (indent == 0) s << "Parse error: "; 
			else {
				if (children.size() > 1) s << "- ";
				else s << "Expected ";
			}
			auto er = (error_report*)strend;
			if (er) { er->write(s); s << "\n"; }
			
			if (children.size()) {
//				children[0].to_stream(s);
				if (children.size() > 1) {
					children[1].to_stream(s, indent + 1);
				}
			}
			return;
		}

		if (strbeg) {
			if (strend) s.write(strbeg, strend - strbeg);
			else s << strbeg;
		}
		if (children.size()) s << "[";
		for (size_t i(0);i < children.size();++i) {
			if (i) s << " ";
			children[i].to_stream(s);
		}
		if (children.size()) s << "]";
	}
}
