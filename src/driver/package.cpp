#include "package.h"
#include "common/PlatformUtils.h"
#include "config/system.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <memory>
#include <thread>
#include <regex>

#define KRONOS_USER_AGENT "Kronos WebRequest/" KRONOS_PACKAGE_VERSION

#ifdef WIN32
#include <io.h>
#include <Windows.h>
#include <WinInet.h>
#include <direct.h>
#include <stdexcept>
#pragma comment(lib, "wininet")

namespace {
	struct FilesystemLock {
		HANDLE handle;
		FilesystemLock(const char *path, int timeOut100ms) {
			handle = ::CreateFile(utf8filename(path).c_str(), GENERIC_WRITE | DELETE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
			while (timeOut100ms--) {
				if (::LockFile(handle, 0, 0, 4, 0)) {
					FILE_DISPOSITION_INFO fdi;
					fdi.DeleteFileW = TRUE;
					SetFileInformationByHandle(handle, FileDispositionInfo, &fdi, sizeof(FILE_DISPOSITION_INFO));
					return;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			}
			throw std::runtime_error(std::string("Failed to obtain lock: ") + path);
		}

		~FilesystemLock() {
			UnlockFile(handle, 0, 0, 4, 0);
			::CloseHandle(handle);
		}
	};

	void CloseHandleProc(char *ptr) {
		if (ptr) InternetCloseHandle((HINTERNET)ptr);
	}

	using WIHandle = std::unique_ptr<char, void(*)(char*)>;

	WIHandle Wrap(HINTERNET inet) {
		if (inet == INVALID_HANDLE_VALUE) return WIHandle{ nullptr, CloseHandleProc };
		return WIHandle{ (char*)inet, CloseHandleProc };
	};
}

WebResponse WebRequest(std::string method, std::string server, std::string page, const void* body, size_t bodySize, const std::unordered_set<std::string>& extraHeaders) {
	BOOL trueVar = TRUE;
	DWORD trueSz = sizeof(trueVar);

	INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
	DWORD secureFlag = 0;

	const std::string httpScheme = "http://";
	const std::string httpsScheme = "https://";

	if (server.compare(0, httpScheme.size(), httpScheme) == 0) {
		server = server.substr(httpScheme.size());
	} else if (server.compare(0, httpsScheme.size(), httpsScheme) == 0) {
		port = INTERNET_DEFAULT_HTTPS_PORT;
		secureFlag = INTERNET_FLAG_SECURE;
		server = server.substr(httpsScheme.size());
	} else {
		throw std::runtime_error("http or https scheme is required");
	}

	if (server.find_last_of(':') != server.npos) {
		auto portString = server.substr(server.find_last_of(':') + 1);
		if (!sscanf(portString.data(), "%hu", &port)) {
			throw std::invalid_argument("Invalid port: " + portString);
		}
		server = server.substr(0, server.find_last_of(':'));
	}

	auto wserver = utf8filename(server.c_str());
	auto wpage = utf8filename(page.c_str());

	// initialize WinInet
	auto winInet = Wrap(InternetOpen(TEXT(KRONOS_USER_AGENT " (WinInet)"), INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0));
	if (!winInet) throw std::runtime_error("Could not initialize WinInet");

	auto connection = Wrap(InternetConnect(winInet.get(), wserver.c_str(), port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, NULL));
	if (!connection) throw std::runtime_error("Could not connect to " + server);
	auto request = Wrap(HttpOpenRequest(connection.get(), utf8filename(method).c_str(), wpage.c_str(), NULL, NULL, NULL, secureFlag, NULL));
	if (!request) throw std::runtime_error("Could not request " + page);
	if (!HttpAddRequestHeaders(request.get(), L"Accept-Encoding: identity", -1L, HTTP_ADDREQ_FLAG_REPLACE))
		throw std::runtime_error("Could not specify encoding");

	for (auto &h : extraHeaders) {
		if (h.size()) {
			if (!HttpAddRequestHeaders(request.get(), utf8filename(h).c_str(), -1L, HTTP_ADDREQ_FLAG_REPLACE))
				throw std::runtime_error("Could not add header '" + h + "'");
		}
	}

	if (HttpSendRequest(request.get(), NULL, 0, (LPVOID)body, (DWORD)bodySize)) {
		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		HttpQueryInfo(request.get(), HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &statusCode, &statusSize, NULL);

		std::vector<char> result;
		char buffer[4096] = { 0 };
		DWORD didRead = 0;
		for (;;) {
			if (InternetReadFile(request.get(), buffer, 4096, &didRead) && didRead == 0) break;
			size_t pos = result.size();
			result.resize(pos + didRead);
			memcpy(result.data() + pos, buffer, didRead);
		}
		return {
			(int)statusCode,
			server + page,
			std::move(result)
		};
	} 
	throw std::runtime_error(server + page + " not available");
}
#else
#define _stat stat
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
static void _mkdir(const char *path) {
    mkdir(path, 0777);
}

namespace {
	struct FilesystemLock {
		int handle;
		std::string path;
		FilesystemLock(const char *path, int timeout):path(path) {
			do {
				handle = open(path, O_CREAT | O_EXCL, S_IRWXU);
				if (handle != -1) return;
				if (errno != EEXIST) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
			} while (--timeout > 0);
			throw std::runtime_error("Could not write package manager lock file '" + std::string(path) + "'");
		}

		~FilesystemLock() {
			if (handle != -1) {
				unlink(path.c_str());
				close(handle);
			}
		}
	};
}
#endif

#if CURL_FOUND
#include <curl/curl.h>
namespace {
	void CleanupCURL(CURL* curl) {
		curl_easy_cleanup(curl);
	}

	std::unique_ptr<CURL, void(*)(CURL*)> Wrap(CURL* ptr) {
		return { ptr, CleanupCURL };
	}

	static size_t curl_write(void* data, size_t size, size_t nmemb, void* context) {
		auto blob = (std::vector<char>*)context;
		auto pos = blob->size();
		blob->resize(pos + nmemb * size);
		memcpy(blob->data() + pos, data, nmemb * size);
		return size * nmemb;
	}

	struct curl_body {
		const char *data;
		size_t limit;
	};

	static size_t curl_read(void *dest, size_t size, size_t nmemb, void* userp) {
		auto body = (curl_body*)userp;
		auto todo = std::min(body->limit, size * nmemb);
		if (todo) {
			memcpy(dest, body->data, todo);
			body->data += todo;
			body->limit -= todo;
		}
		return todo;
	}
}
#define CHECK(expr) { CURLcode err = expr; if (err != CURLE_OK) throw std::runtime_error("libCURL error " __FILE__ ":" + std::to_string(__LINE__) + " " + curl_easy_strerror(err)); }
WebResponse WebRequest(std::string method, std::string server, std::string page, const void* body, size_t bodySize, const std::unordered_set<std::string>& extraHeaders) {
	static bool curl_global = false;
    if (!curl_global) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl_global = true;
    }
    auto curl = Wrap(curl_easy_init());
    if (curl) {
//		curl_easy_setopt(curl.get(), CURLOPT_VERBOSE, 1L);
        CHECK(curl_easy_setopt(curl.get(), CURLOPT_URL, (server + "/" + page).c_str()));
        CHECK(curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, curl_write));
		std::vector<char> result;
		CHECK(curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &result));
		CHECK(curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, KRONOS_USER_AGENT " (libCURL)"));
		CHECK(curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1));

		curl_body cbody{ (const char*)body, bodySize };
		if (bodySize) {
			CHECK(curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, curl_read));
			CHECK(curl_easy_setopt(curl.get(), CURLOPT_READDATA, &cbody));
			CHECK(curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, (long)bodySize));
		}

		curl_slist *chunk = nullptr;
		for (auto &h : extraHeaders) {
			if (h.size()) chunk = curl_slist_append(chunk, h.c_str());
		}
		if (chunk) CHECK(curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, chunk));
			
		int one = 1;
			
		if (method == "POST") {
			CHECK(curl_easy_setopt(curl.get(), CURLOPT_POST, &one));
		} else if (method == "PUT") {
			CHECK(curl_easy_setopt(curl.get(), CURLOPT_PUT, &one));
		}

        CHECK(curl_easy_perform(curl.get()));

        long http_code = 0;
        CHECK(curl_easy_getinfo(curl.get(), CURLINFO_HTTP_CODE, &http_code));
		curl_slist_free_all(chunk);
		return {
			(int)http_code,
			server + page,
			std::move(result)
		};
    } else {
        throw std::runtime_error("Could not initialize libcurl");
    }
}
#endif

namespace Packages {
	const char* CloudClient::ResolverCallback(const char *pack, const char* file, const char *ver, void *self) {
		return ((CloudClient*)self)->Resolve(pack, file, ver);
	}

	CloudClient::CloudClient() {
		using namespace std::string_literals;
		fileSystemLocation = GetCachePath() + "/";

		std::ifstream cacheFile{ fileSystemLocation + "cache.json" };
		if (cacheFile.is_open()) {
			picojson::value parsed;
			picojson::parse(parsed, cacheFile);
			if (parsed.is<picojson::object>()) cacheData = parsed.get<picojson::object>();
		}
	}

	void CloudClient::AddPackageVersion(const std::string& package, const std::string& version, const std::string& hash) const {
		std::lock_guard<std::recursive_mutex> lg{ lock };
		if (!cacheData[package].is<picojson::object>()) {
			cacheData[package] = picojson::object{ };
		}
		
		auto &versions = cacheData[package].get<picojson::object>();

		if (versions[version] != hash) {
			versions[version] = hash;
			std::ofstream cacheFile{ fileSystemLocation + "cache.json" };
			if (cacheFile.is_open()) {
				cacheFile << picojson::value{ cacheData }.serialize();
			}
		}
	}
    
	const char *CloudClient::Remember(std::string str) {
		std::lock_guard<std::recursive_mutex> lg(lock);
		return stringPool.emplace(std::move(str)).first->c_str();
	}

	void MakeMultiLevelPath(const std::string& path) {
		struct _stat buf;
		if (_stat(path.c_str(), &buf)) {
			for (size_t pos = 0; (pos = path.find('/', pos + 1)) != path.npos;) {
				_mkdir(path.substr(0, pos).c_str());
			}
		}
	}

	static const std::string api = "https://api.bitbucket.org";

	bool CloudClient::DoesFileExist(const std::string& file_in) {
		struct _stat buf;
		return (_stat(file_in.c_str(), &buf) == 0);
	}

	const char* CloudClient::Resolve(std::string pack, std::string file, std::string version) {
		try {
			auto path = GetLocalFilePath(pack, version);
			auto file_in = path + file;
			if (!DoesFileExist(file_in)) {
				std::clog << "* Downloading [" << pack << " " << version << "]:" << file << " ...";
				try {
					Obtain(pack, version, file, file_in);
#ifdef WIN32
					_wchmod(utf8filename(file_in.c_str()).c_str(), _S_IREAD);
#else 
					chmod(file_in.c_str(), S_IRUSR | S_IRGRP);
#endif
					std::clog << "Ok\n";
				} catch (std::exception& e) {
					std::clog << "Failed: " << e.what() << "\n";
				}
			}
			return Remember(file_in);
		} catch (std::exception& e) {
			std::cerr << "! " << e.what() << " !\n";
			return "";
		}
	}

	std::string CloudClient::GetLocalFilePath(std::string pack, std::string version) const {
		static std::regex semver{"[0-9]+\\.[0-9]+(\\.[0-9]+)?"};
		if (std::regex_match(version, semver)) {
			return fileSystemLocation + pack + "/" + version + "/";
		} else {
			if (version.size() && version.back() == '~') {
				// legacy: tilde indicates moving ref
				version.pop_back();
				version = version + "-" + GetCommitHash(pack, version);
			}
			return fileSystemLocation + pack + "/" + version + "/";
		}
	}
    
	std::string BitBucketCloud::GetCommitHash(const std::string& pack, const std::string& version) const {
		auto tag = "/2.0/repositories/" + pack + "/refs/tags/" + version;
		picojson::value parsed = WebRequest("GET", api, tag);
		return parsed.get("target").get("hash").to_str();
	}

	void BitBucketCloud::Obtain(std::string pack, std::string version, std::string file, std::string dest) {
		if (version.back() == '~') version.pop_back();

		auto sha = GetCommitHash(pack, version);
		auto src = "/2.0/repositories/" + pack + "/src/" + sha + "/" + file;
		auto content = WebRequest("GET", api, src);

		if (content.Ok()) {
			AddPackageVersion(pack, version, "bb-" + sha);

			std::lock_guard<std::recursive_mutex> lg(lock);

			MakeMultiLevelPath(dest);
			FilesystemLock lock((GetFileSystemLocation() + ".lock").c_str(), 20);

			std::ofstream write(utf8filename(dest), std::ios_base::binary);
			write.write(content.data.data(), content.data.size());
		} else {
			throw std::runtime_error("Server responded with " + content.uri + " -> " + std::to_string(content.code) + " " + content.Text());
		}
	}

	std::string GitHubApi::GetCommitHash(const std::string& pack, const std::string& version) const {
		std::string sha = "";
		std::lock_guard<std::recursive_mutex> lg{ lock };
		if (cacheData[pack].contains(version) && cacheData[pack].get(version).to_str().substr(0, 3) == "gh-") {
			sha = cacheData[pack].get(version).to_str().substr(3);
		} else {
			for (std::string kind : { "/tags", "/branches" }) {
				picojson::value parsed = WebRequest("GET", "https://api.github.com", "/repos/" + pack + kind);

				if (parsed.is<picojson::array>()) {
					auto tags = parsed.get<picojson::array>();
					for (auto &t : tags) {
						if (t.contains("name") && t.contains("commit") && t.get("commit").contains("sha")) {
							auto thisSha = t.get("commit").get("sha").to_str();
							if (kind == "/tags") {
								AddPackageVersion(pack, t.get("name").to_str(), "gh-" + thisSha);
							}
							if (t.get("name").to_str() == version) {
								sha = thisSha;
							}
						}
					}
				}
			}
		}
		if (sha.empty()) {
			throw std::runtime_error("Specified tag not found");
		}
		return sha;
	}

	void GitHubApi::Obtain(std::string pack, std::string version, std::string file, std::string dest) {
		if (version.empty()) {
			throw std::runtime_error("Bad package version '" + version + "' for " + pack);
		}
		if (version.back() == '~') version.pop_back();
		//auto sha = GetCommitHash(pack, version);
		auto rawUrl = "/" + pack + "/" + version + "/" + file;
		auto content = WebRequest("GET", "https://raw.githubusercontent.com", rawUrl);
		if (content.Ok()) {
			std::lock_guard<std::recursive_mutex> lg(lock);

			MakeMultiLevelPath(dest);
			FilesystemLock lock((GetFileSystemLocation() + ".lock").c_str(), 20);

			std::ofstream write(utf8filename(dest), std::ios_base::binary);
			write.write(content.data.data(), content.data.size());
		} else {
			throw std::runtime_error("server responded with " + std::to_string(content.code) + " " + content.Text());
		}
	}

	static inline uint64_t mul_128hi(uint64_t x, uint64_t y) {
		const uint64_t m32 = 0xffffffff;
		uint64_t x0 = x & m32, y0 = y & m32, x1 = x >> 32, y1 = y >> 32;
		auto z0 = x0 * y0;
		auto z1a = x1 * y0;
		auto z1b = x0 * y1;
		auto z2 = x1 * y1;
		
		auto z1al = z1a & m32, z1bl = z1b & m32;
		auto z1l = z1al + z1bl + (z0 >> 32);

		auto z1h = (z1a >> 32) + (z1b >> 32) + (z1l >> 32);
		z2 += z1h;

		return z2;
	}

	uint128 operator*(uint128 a, uint128 b) {
		auto low = a.low() * b.low();
		auto high = mul_128hi(a.low(), b.low());
		high += a.low() * b.high();
		high += a.high() * b.low();

		return { low, high };
	}

	uint128 operator^(uint128 a, uint128 b) {
		for (int i = 0;i < 4;++i) {
			a.dw[i] ^= b.dw[i];
		}
		return a;
	}

	bool operator==(uint128 a, uint128 b) {
		for (int i = 0;i < 4;++i) if (a.dw[i] != b.dw[i]) return false;
		return true;
	}

	bool operator<(uint128 a, uint128 b) {
		for (int i = 3;i >= 0;--i) if (a.dw[i] < b.dw[i]) return true;
		return false;
	}

	std::string uint128::digest() const {
		uint32_t scramble = 0;
		for (auto& w : dw) {
			scramble ^= w;
		}
		const char b64[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
		std::string d = "";
		for (int i = 0; i < 5; i++) {
			d.push_back(b64[scramble & 0x3f]);
			scramble >>= 6;
		}
		return d;
	}

	static size_t writeHex(char * const buf, const uint128& u128) {
		const char hexdigit[] = {
			'0','1','2','3','4','5','6','7','8','9','0','a','b','c','d','e','f'
		};
		char *write = buf;
		for (int i = 3;i >= 0;--i) {
			for (int j = 7;j >= 0;--j) {
				*write++ = hexdigit[(u128.dw[i] >> (j * 4)) & 0xf];
			}
		}
		return write - buf;
	}

	uint128::operator std::string() const {
		char hex[32];
		auto sz = writeHex(hex, *this);
		return { hex, hex + sz };
	}

	std::ostream& operator<<(std::ostream& os, const uint128& u128) {
		char hex[32];
		auto sz = writeHex(hex, u128);
		os.write(hex, sz);
		return os;
	}

	uint128 fnv1a(uint128& hash, const void* data, size_t bytes) {
		const char *byte = (const char *)data;
		uint128 fnv_prime{ 0x3b + (1 << 8), 1 << (88 - 64) };
		for (size_t i = 0;i < bytes;++i) {
			hash = hash ^ byte[i];
			hash = hash * fnv_prime;
		}
		return hash;
	}

	uint128 fnv1a(const void* data, size_t bytes) {
		uint128 hash = {
			0x62b821756295c58d, 
			0x6c62272e07bb0142 
		};
		return fnv1a(hash, data, bytes);
	}
}

picojson::value WebResponse::JSON() const {
	auto b = data.begin(), e = data.end();
	picojson::value v;
	auto err = picojson::parse(v, b, e);
	if (err.size()) throw std::runtime_error(uri + ":" + err);
	return v;
}

WebResponse::operator picojson::value() const {
	if (code / 100 != 2) throw std::runtime_error(uri + " responded: " + std::to_string(code) + " " + Text());
	return JSON();
}
