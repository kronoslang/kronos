#pragma once
#include <string>
#include <limits>
#include <cassert>

#ifdef WIN32
#define EXTERN_C_VISIBLE extern "C" __declspec(dllexport) 

// widen utf8 for non-standard msvc fstream and fopen overloads
std::wstring utf8filename(const std::string& fn);
std::wstring utf8filename(const char* fn);

#else
#define EXTERN_C_VISIBLE extern "C"

// filenames are natively utf8
static std::string utf8filename(const std::string& fn) { return fn; }
static const char* utf8filename(const char *fn) { return fn; }

#include "time.h"
#endif


template <typename TO, typename FROM> static TO check_cast(FROM f) {
#ifndef NDEBUG
	auto m = std::numeric_limits<TO>::min();
	auto x = std::numeric_limits<TO>::max();
    assert((f >= 0 || f >= m) && "sign lost in cast");
	assert((f < 0 || f <= x) && "truncated cast");
	assert(static_cast<FROM>(static_cast<TO>(f)) == f && "cast loses precision");
#endif
	return static_cast<TO>(f);
}

std::string GetProcessFileName( );
std::string GetProcessID( );
std::string GetMachineName( );
time_t GetFileLastModified(const std::string& filename);

std::string GetUserPath();
std::string GetSharedPath();
std::string GetCachePath();
std::string GetConfigPath();

std::string GetParentPath(std::string pathToFile);
std::string GetCanonicalAbsolutePath(std::string path);

std::string encode_utf8(const std::wstring& w);

bool IsStdoutTerminal();
