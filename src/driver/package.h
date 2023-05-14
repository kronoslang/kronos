#pragma once

#include "driver/picojson.h"

#include <string>
#include <unordered_set>
#include <mutex>

namespace Packages {
	class CloudClient {
		std::string fileSystemLocation;
		std::unordered_set<std::string> stringPool;
	protected:
		mutable picojson::object cacheData;
		mutable std::recursive_mutex lock;
		const char* Remember(std::string str);
		virtual void Obtain(std::string package, std::string version, std::string file, std::string dest) = 0;
		virtual std::string GetCommitHash(const std::string & pack, const std::string & version) const = 0;
		void AddPackageVersion(const std::string& package, const std::string& version, const std::string& hash) const;
		const std::string& GetFileSystemLocation() const { return fileSystemLocation; }
	public:
		CloudClient();
        virtual ~CloudClient() { }
		static const char* ResolverCallback(const char *package, const char *file, const char *version, void *self);
		const char* Resolve(std::string package, std::string file, std::string version);
		std::string GetLocalFilePath(std::string package, std::string version) const;

		const picojson::object& GetCache() const {
			return cacheData;
		}

		static bool DoesFileExist(const std::string& path);
	};

	class BitBucketCloud : public CloudClient {
		std::string GetCommitHash(const std::string & pack, const std::string & version) const override;
		void Obtain(std::string package, std::string version, std::string file, std::string dstPath) override;
	public:
		BitBucketCloud() { };
	};

	class GitHubApi : public CloudClient {
		void Obtain(std::string package, std::string version, std::string file, std::string dstPath) override;
	public:
		GitHubApi() { };
		std::string GetCommitHash(const std::string & pack, const std::string & version) const;
	};

	void MakeMultiLevelPath(const std::string& path);

	struct uint128 {
		uint32_t dw[4] = { 0 }; // little endian
		
		uint128(uint64_t low, uint64_t high = 0) {
			dw[0] = low & 0xffffffff;
			dw[1] = low >> 32;
			dw[2] = high & 0xffffffff;
			dw[3] = high >> 32;
		}

		uint128(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
			dw[0] = a;dw[1] = b;dw[2] = c;dw[3] = d;
		}

		uint64_t low() const {
			uint64_t l = dw[1];
			l <<= 32;
			l += dw[0];
			return l;
		}

		uint64_t high() const {
			uint64_t h = dw[3];
			h <<= 32;
			h += dw[2];
			return h;
		}

		bool is_zero() const { for (auto d : dw) if (d) return false; return true; }
		operator std::string() const;
		std::string to_str() const { return *this; }
		std::string digest() const;
	};

	uint128 operator*(uint128 a, uint128 b);
	uint128 operator^(uint128 a, uint128 b);
	bool operator==(uint128 a, uint128 b);
	bool operator<(uint128 a, uint128 b);
	std::ostream& operator<<(std::ostream&, const uint128&);

	uint128 fnv1a(const void* data = nullptr, size_t numBytes = 0);
	uint128 fnv1a(uint128& state, const void* data, size_t numBytes);

	using DefaultClient = GitHubApi;
}

struct WebResponse {
	int code;
	std::string uri;
	std::vector<char> data;
	operator picojson::value() const;
	bool Ok() const {
		return (code / 100) == 2;
	}
	picojson::value JSON() const;
	std::string Text() const { return std::string(data.data(), data.data() + data.size()); }
};

WebResponse WebRequest(std::string method, std::string server, std::string page, 
					   const void* body = nullptr, size_t bodySize = 0, const std::unordered_set<std::string>& extraHeaders = {});
