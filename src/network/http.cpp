#include "http.h"
#include "lithe/lithe.h"
#include <sstream>

namespace Sxx {
	namespace http {
		static std::string uriPunct = "-._~:/?#[]@!$&'()*+,;=%";

		int isurlc(int c) {
			return (isalnum(c) || uriPunct.find(c) != uriPunct.npos) ? 1 : 0;
		}

		lithe::rule RequestGrammar() {
			using namespace lithe;

			auto GET = T("GET");
			auto PUT = T("PUT");
			auto POST = T("POST");

			auto ln = I("\r\n");

			auto uri = characters("uri", isurlc);
			auto protocol = (GET | PUT | POST) << I(" ") << uri << I(T(" HTTP/") << characters("not newline", "\r", true));
			auto hkey = characters("kebab-case", [](int c) { return (isalnum(c) || c == '-') ? 1 : 0;});
			auto hval = characters("value", "\r", true);
			auto header = E("header", hkey << require("value required for header", I(": ") << hval));

			rule httpHeaders;
			return protocol << ln << repeat(header << ln, 0);
		}

		Request Parse(std::istream& s) {
			std::stringstream req;
			for (std::string line; std::getline(s, line);) {
				if (line == "\r") break;
				req << line << "\n";
			}

			auto reqs = req.str();
			static lithe::rule grammar = RequestGrammar();
			auto headers = grammar->parse(reqs);

			Request reqData;
			if (headers.children.size() > 1) {
				reqData.Method = headers[0].get_string();
				reqData.Uri = headers[1].get_string();
				for (int i = 2;i < headers.children.size();++i) {
					std::string key = headers[i][0].get_string();
					for (auto &c : key) c = tolower(c);
					reqData.Headers.emplace( key, headers[i][1].get_string());
				}
			}

			if (reqData.Headers["transfer-encoding"] == "chunked") {
				for (std::string chunkLen;std::getline(s, chunkLen);) {
					size_t chunkBytes = strtoull(chunkLen.c_str(), nullptr, 10);
					if (!chunkBytes) break;
					auto pos = reqData.Body.size();
					reqData.Body.resize(pos + chunkBytes);
					s.read(reqData.Body.data() + pos, chunkBytes);
					std::getline(s, chunkLen);
				}
			} else if (reqData.Headers.count("content-length")) {
				auto bytes = strtoll(reqData.Headers["content-length"].c_str(), nullptr, 10);
				reqData.Body.resize(bytes);
				s.read(reqData.Body.data(), bytes);
			}

			return reqData;
		}

		std::string UrlDecode(std::string url) {
			int wr = 0;
			for (int i = 0;i < url.size();++i, ++wr) {
				if (url[i] == '%') {
					if (url.size() <= i + 2) throw std::invalid_argument("invalid url");
					char hex[3] = { url[i + 1], url[i + 2], 0 };
					url[wr] = (char)strtol(hex, nullptr, 16);
					i += 2;
				} else if (wr != i) {
					url[wr] = url[i];
				}
			}
			url.resize(wr);
			return url;
		}

		std::string UrlEncode(std::string txt) {
			std::string url;
			url.reserve(url.size());
			for (auto c : txt) {
				if (c != '%' && isurlc(c)) url.push_back(c);
				else {
					static char hex[16] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };
					url.push_back('%');
					url.push_back(hex[(c >> 4) & 0xf]);
					url.push_back(hex[c & 0xf]);
				}
			}
			return url;
		}
	}
}

namespace std {
	std::ostream& operator << (std::ostream& os, const Sxx::http::Request&) {
		return os;
	}

	std::ostream& operator << (std::ostream& os, const Sxx::http::Response& r) {
		os << "HTTP/1.1 " << r.ResultCode << " " << r.ReasonPhrase() << "\r\n";
		for (auto &h : r.Headers) {
			os << h.first << ": " << h.second << "\r\n";
		}
		os << "\r\n";
		os.write(r.Body.data(), r.Body.size());
		return os;
	}
}
