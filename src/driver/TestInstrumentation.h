#pragma once

#include <ostream>
#include <string>
#include "runtime/inout.h"

#ifndef _MSC_VER
#define MARK_USED __attribute__((used))
#else
#define MARK_USED
#pragma comment(linker, "/include:link_instruments")
#endif


bool AudioDiff(const std::string& result, const std::string& reference);

class InstrumentedIO : public Kronos::IO::IConfiguringHierarchy {
public:
	virtual bool DumpAudio(const char *fileName) = 0;
	static std::unique_ptr<InstrumentedIO> Create(int frames);
};