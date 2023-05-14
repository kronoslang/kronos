#include "PlatformUtils.h"
#include "config/cxx.h"
#include "config/system.h"
#include <time.h>
#include <cassert>
#include <memory>
#ifndef WIN32
#include <sys/param.h>
#include <sys/types.h> 
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <libgen.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

using namespace std::string_literals;

#ifdef EMSCRIPTEN

std::string GetUserPath() {
	return "/.local/share/kronos";
}

std::string GetCachePath() {
	return "/.cache/kronos";
}

#else

std::string GetUserPath() {
	if (getenv("XDG_DATA_HOME")) return getenv("XDG_DATA_HOME") + "/kronos"s;
	passwd *pw = getpwuid(getuid());
	auto home = std::string((pw && pw->pw_dir) ? pw->pw_dir : "");
#ifdef __APPLE__
	return home + "/Library/Kronos";
#else 
	return home +"/.local/share/kronos";
#endif
}

std::string GetCachePath() {
	if (getenv("XDG_CACHE_HOME")) return getenv("XDG_CACHE_HOME") + "/kronos"s;
	passwd *pw = getpwuid(getuid());
	auto home = std::string((pw && pw->pw_dir) ? pw->pw_dir : "");
#ifdef __APPLE__
	return home + "/Library/Caches/Kronos";
#else
	return home + "/.cache/kronos";
#endif
}

#endif

std::string GetSharedPath() {
	return "/usr/local/share/kronos";
}

#ifdef HAVE_GETPID
#include <libproc.h>
#endif

#ifdef HAVE__NSGETEXECUTABLEPATH
#include <mach-o/dyld.h>
#endif

std::string GetProcessFileName( ) {
	char buffer[4096];
	#ifdef HAVE__NSGETEXECUTABLEPATH
	std::uint32_t bufSize(sizeof(buffer));
	if (_NSGetExecutablePath(buffer,&bufSize) == 0) {
		return buffer;
	}
	#endif
	#ifdef HAVE_READLINK
	if (readlink("/proc/self/exe",buffer,sizeof(buffer))) {
		return buffer;
	} 
	#endif
	#if defined(HAVE_GETPID) && defined(HAVE_PROC_PIDPATH) 
	pid_t pid = getpid();
	if (proc_pidpath(pid,buffer,sizeof(buffer)>0)) {
		return buffer;
	}
	#endif
	if (getenv("_")) return getenv("_");
	return "./";
}

time_t GetFileLastModified(const std::string& str) {
	struct stat st;
	stat(str.c_str(),&st);
	return st.st_mtime;
}

std::string GetParentPath(std::string filePath) {
    return dirname((char*)filePath.data());
}

std::string GetCanonicalAbsolutePath(std::string path) {
	char fullPath[PATH_MAX] = { 0 };
	auto rp = realpath(path.c_str(), fullPath);
	if (rp) path = rp;
	if (path.size() && path.back() == '/') path.pop_back();
	return path;
}

std::string GetProcessID() {
	return std::to_string(getpid());
}

bool IsStdoutTerminal() {
	return isatty(fileno(stdout)) != 0;
}


#else

#include <sys/types.h> 
#include <sys/stat.h>
#include <wchar.h>
#include <vector>
#include <cassert>         // assert
#include <iostream>         // std::wcerr
#include <streambuf>        // std::basic_streambuf
#include <algorithm>
#include <sstream>
#include <mutex>
#include <codecvt>
#include <direct.h>
#include <Windows.h>
#include <io.h>

#include "pad/cog/cog.h"

using namespace std;

#undef  UNICODE
#define UNICODE
#undef  STRICT
#define STRING

bool IsStdoutTerminal() {
	return _isatty(_fileno(stdout));
}

// Convert an UTF8 string to a wide Unicode String
std::string encode_utf8(const std::wstring &str) {
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
	return cvt.to_bytes(str);
}

// Convert an UTF8 string to a wide Unicode String
std::wstring utf8filename(const std::string &str) {
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
	return cvt.from_bytes(str);
}

std::wstring utf8filename(const char* str) {
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
	return cvt.from_bytes(str);
}

std::string GetCanonicalAbsolutePath(std::string path) {
	wchar_t full[_MAX_PATH ] = { 0 };
	for (auto& p : path) {
		if (p == '/') p = '\\'; 
	}
	auto wpath = utf8filename(path);
	if (_wfullpath(full, wpath.data(), _MAX_PATH) != nullptr) {
		path = encode_utf8(std::wstring(full));
	} 
	if (path.size() && path.back() == '\\') path.pop_back();
	return path;
}

std::string GetParentPath(std::string filePath) {
	wchar_t drv[_MAX_DRIVE];
	wchar_t dir[_MAX_DIR];
	_wsplitpath(utf8filename(filePath).c_str(), drv, dir, nullptr, nullptr);
	return encode_utf8(std::wstring(drv) + dir);
}

std::string GetProcessFileName( ) {
	wchar_t path[4096] = {0};
	if (GetModuleFileNameW(GetModuleHandleW(nullptr), path, 4096) != 0) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
		auto str = cvt.to_bytes(path);
		std::replace(str.begin( ), str.end( ), '\\', '/');
		return str;
	}
	return "./";
}

std::string GetProcessID() {
	return std::to_string(GetCurrentProcessId());
}

#include <ShlObj.h>

using COG::WinError;

std::string GetUserPath() {
	if (getenv("XDG_DATA_HOME")) return getenv("XDG_DATA_HOME") + "/kronos"s;
	PWSTR path{ nullptr };
	{
		WinError::Context("Locating UserData folder: ");
		WinError err = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &path);
	}
	auto pstr = encode_utf8(path);
	CoTaskMemFree(path);
	return pstr + "/Kronos";
}

std::string GetSharedPath() {
	PWSTR path{ nullptr };
	{
		WinError::Context("Locating ProgramData folder: ");
		WinError err = SHGetKnownFolderPath(FOLDERID_ProgramData, 0, NULL, &path);
		std::string ex;
		if (err.IsError(ex)) {
			std::cerr << "* Error while getting known folder ProgramData: " + ex << "\n";
			err.Clear();
			return "C:\\ProgramData\\Kronos";
		}
	}
	auto pstr = encode_utf8(path);
	CoTaskMemFree(path);
	return pstr + "\\Kronos";
}

std::string GetCachePath() {
	if (getenv("XDG_CACHE_HOME")) return getenv("XDG_CACHE_HOME") + "/kronos"s;
	return GetSharedPath() + "/Cache";
}

time_t GetFileLastModified(const std::string& str) {
	struct _stat stat;
	_wstat(utf8filename(str).c_str(),&stat);
	return stat.st_mtime;
}
#endif

std::string GetConfigPath() {
	if (getenv("XDG_CONFIG_HOME")) return getenv("XDG_CONFIG_HOME");
	return GetUserPath();
}