#include <fstream>
#include "attachment.h"

static const char* B64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const int B64index[256] =
{
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  62, 63, 62, 62, 63,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0,  0,  0,  0,  0,  0,
	0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 0,  0,  0,  0,  63,
	0,  26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51
};

const std::vector<unsigned char> b64decode(const void* data, const size_t& len) {
	if (len == 0) return {};

	unsigned char* p = (unsigned char*)data;
	size_t j = 0,
		pad1 = len % 4 || p[len - 1] == '=',
		pad2 = pad1 && (len % 4 > 2 || p[len - 2] != '=');
	const size_t last = (len - pad1) / 4 << 2;
	std::vector<unsigned char> result(last / 4 * 3 + pad1 + pad2, '\0');
	unsigned char* str = (unsigned char*)result.data();

	for (size_t i = 0; i < last; i += 4) {
		int n = B64index[p[i]] << 18 | B64index[p[i + 1]] << 12 | B64index[p[i + 2]] << 6 | B64index[p[i + 3]];
		str[j++] = n >> 16;
		str[j++] = n >> 8 & 0xFF;
		str[j++] = n & 0xFF;
	}
	if (pad1) {
		int n = B64index[p[last]] << 18 | B64index[p[last + 1]] << 12;
		str[j++] = n >> 16;
		if (pad2) {
			n |= B64index[p[last + 2]] << 6;
			str[j++] = n >> 8 & 0xFF;
		}
	}
	return result;
}

std::vector<unsigned char> b64decode(const std::string& str64) {
	return b64decode(str64.c_str(), str64.size());
}

const std::string b64encode(const void* data, const size_t& len) {
	std::string str((len + 2) / 3 * 4, '=');
	unsigned char* p = (unsigned char*)data;
	size_t j = 0, pad = len % 3;
	const size_t last = len - pad;

	for (size_t i = 0; i < last; i += 3) {
		int n = int(p[i]) << 16 | int(p[i + 1]) << 8 | p[i + 2];
		str[j++] = B64chars[n >> 18];
		str[j++] = B64chars[n >> 12 & 0x3F];
		str[j++] = B64chars[n >> 6 & 0x3F];
		str[j++] = B64chars[n & 0x3F];
	}
	if (pad)  /// set padding
	{
		int n = --pad ? int(p[last]) << 8 | p[last + 1] : p[last];
		str[j++] = B64chars[pad ? n >> 10 & 0x3F : n >> 2];
		str[j++] = B64chars[pad ? n >> 4 & 0x03F : n << 4 & 0x3F];
		str[j++] = pad ? B64chars[n << 2 & 0x3F] : '=';
	}
#ifndef NDEBUG
	auto test = b64decode(str);
	assert(test.size() == len);
	for (int i = 0; i < len; ++i) {
		assert(test[i] == p[i]);
	}
#endif
	return str;
}

std::string b64encode(const std::string& str) {
	return b64encode(str.c_str(), str.size());
}

std::string attach(picojson::value& link, const std::string& path, const std::string& ext, const std::string& mime) {
	std::ifstream file{ path, std::ios_base::binary | std::ios_base::ate };
	if (file.is_open() == false) throw std::runtime_error("Could not load '" + path + "'");
	std::vector<char> data((size_t)file.tellg());
	file.seekg(0, file.beg);
	file.read(data.data(), data.size());

	auto b64 = b64encode(data.data(), data.size());
	auto uid = Packages::fnv1a(b64.data(), b64.size()).to_str() + ext;

	if (mime.find("VIP") != mime.npos) {
		uid = "vip-" + uid;
	}

	if (!link.contains("~attachments")) {
		link.get<picojson::object>()["~attachments"] = picojson::object{};
	}
	picojson::object& attachments{
		link.get<picojson::object>()["~attachments"].get<picojson::object>()
	};

	auto fileName = path;
	if (auto folderPos = fileName.find_last_of("/\\")) {
		fileName = fileName.substr(folderPos + 1);
	}

	attachments[uid] = picojson::object{
		{ "data", b64 },
		{ "mime", mime },
		{ "name", fileName }
	};

	return uid;
}
