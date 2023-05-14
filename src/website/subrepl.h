#pragma once

#include <string>
#include <vector>

struct subrepl_state {
	std::string code;
	std::vector<std::string> audio;
	std::vector<std::string> eval;
	void reset();
	std::string evaluate(std::string immediate);
	subrepl_state() { reset(); }
};