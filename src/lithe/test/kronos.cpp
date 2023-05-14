#include "lithe.h"
#include "grammar/kronos.h"
#include <fstream>
#include <iostream>
#include <sstream>

int main() {
	std::ifstream test("/Users/vnorilo/code/kronos/library/VM.k");
	std::stringstream src;
	src << test.rdbuf();

	using namespace lithe;

	std::string code = src.str();

	auto parser = grammar::kronos::parser();
	auto tokens = parser->parse(code);
	tokens.to_stream(std::cout);

}