#include "subrepl.h"
#include "TestInstrumentation.h"
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

int krepl_main(int argn, const char *carg[]);

void subrepl_state::reset() {
}


std::string subrepl_state::evaluate(std::string immediate) {
	char tmpfile[L_tmpnam];
	auto fname = tmpnam(tmpfile);
	std::ofstream dumpCode(fname);
	dumpCode << code;
	dumpCode.close();

	std::string escaped;
#ifndef NDEBUG
	std::clog << code;
#endif
	std::clog << "> " << immediate << "\n";

	std::stringstream output;
	auto oldCout = std::cout.rdbuf();
	auto oldCerr = std::cerr.rdbuf();
	try {
		std::cout.rdbuf(output.rdbuf());
		std::cerr.rdbuf(output.rdbuf());
		std::vector<const char*> cmdline{ "krepl" };

		cmdline.push_back("-i");
		cmdline.push_back(tmpfile);
		cmdline.push_back(immediate.c_str());

		
		auto resultCode = krepl_main((int)cmdline.size(), cmdline.data());
		if (resultCode != 0) {
			throw std::runtime_error("Execution of '" + immediate + "' failed: " + std::to_string(resultCode));
		}
	} catch (...) {
		std::cerr.rdbuf(oldCerr);
		std::cout.rdbuf(oldCout);
		std::cerr << code << "\n";
		std::cerr << output.str();
		throw;
	}
	std::cerr.rdbuf(oldCerr);
	std::cout.rdbuf(oldCout);

	auto result = output.str();
	std::clog << result;
	return result;
}