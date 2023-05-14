#include "lithe.h"
#include "adapter.h"
#include <cctype>
#include <string>
#include <iostream>
#include <unordered_map>


int main() {
	std::string xml = "<document name='pertti'><element>foo</element><child><subchild/></child></document>";

	using namespace lithe;

	const char *attrlist = "attributes";

	auto xml_node = recursive();
	auto alnum = characters("alphanumeric", isalnum);
	auto whitespace = I(characters("whitespace", isspace));
	auto attribute = alnum << O(I("=") << require(I("'") << characters("not quote", "'", true) << I("'")));
	auto tag_header = alnum << E(attrlist, repeat(whitespace << attribute, 0));
	auto close_tag = I("</") << alnum << I(">");
	auto empty_tag = E("empty_tag", tag_header << I("/>"));

	auto open_tag = E("open_tag", custom("tags must match", tag_header << I(">") 
									  << repeat(xml_node, 0) << close_tag, 
	[](node element) {
		auto closing = element.children.back();
		element.children.pop_back();
		if (closing.get_string() == element[0].get_string()) {
			// tags match
			return element;
		}
		return node::error(nullptr, closing.strbeg);
	}));

	auto element = E("node", custom("canonicalize", I("<") << require(
		open_tag 
		| empty_tag), [](node element) {
		auto tmp = element[0].children;
		element.children = tmp;
		return element;
	}));

	auto text = characters("xml text", "<>", true);

	xml_node->assign(element | text);

	auto walker = [attrlist](auto recur, node xml) -> void {
		if (xml.is_error()) {
			xml.to_stream(std::cout);
			return;
		}

		if (xml.children.empty()) {
			std::cout << xml.get_string();
			return;
		}
		auto tag = xml[0];
		std::cout << "<" << tag.get_string();
		if (xml.size() == 1) {
			 std::cout << "/>";
		} else {
			int beg = 1;
			if (xml[1].strbeg == attrlist) {
				for (auto& a : xml[1].children) {
					std::cout << " " << a[0].get_string() << "='" << a[1].get_string() << "'";
				}
				++beg;
			}
			std::cout << ">";
			while (beg < xml.children.size()) {
				recur(recur, xml[beg++]);
			}
			std::cout << "</" << tag.get_string() << ">";
		}
	};
	walker(walker, (xml_node << end())->parse(xml));

	
}