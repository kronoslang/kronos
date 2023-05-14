#include "CmdLineOpts.h"
#include "runtime/inout.h"

int krepl_main(CmdLine::IRegistry& optionParser, Kronos::IO::IConfiguringHierarchy* io, int argn, const char *carg[]);

int main(int argn, const char *carg[]) {
	return krepl_main(CmdLine::Registry(), nullptr, argn, carg);
}
