#include "JsonRPCEndpoint.h"
#include "common/PlatformUtils.h"
#include <sstream>

#ifdef WIN32
// Windows console encodes newlines as CRLF
#define CRLF "\n"
#else
#define CRLF "\r\n"
#endif

namespace JsonRPC {
	picojson::value Endpoint::operator()(const picojson::value& rpc) const {
		using namespace picojson;
		picojson::object resp;

		if (!rpc.is<object>()) {
			throw std::runtime_error("Could not parse RPC object " + rpc.serialize());
		} else {
			auto method = find(rpc.get("method").to_str());
			resp["jsonrpc"] = rpc.get("jsonrpc");
			if (rpc.contains("id")) resp["id"] = rpc.get("id");
			if (method != end()) {
				try {
					resp["result"] = method->second(rpc.get("params"));
					if (resp.count("id") > 0) return resp;
					else return picojson::value{};
				} catch (std::exception& e) {
					resp["error"] = object{
						{ "code", -32603.0 },
						{ "message", e.what() }
					};
				}
			} else {
				resp["error"] = object{
					{ "code", -32601.0 },
					{ "message", picojson::value(rpc.get("method").to_str() + " is not supported") }
				};
			}
		}
		return resp;
	}

	picojson::value Get(std::istream& s) {
		int len = 0;
		picojson::value obj;

        for (std::string header; true;) {
            if (!std::getline(s, header)) {
				return {};
            } else {
                if (header.empty()) break;
                while(isspace(header.back())) header.pop_back();
                if (header == "") break;
                sscanf(header.c_str(), "Content-Length: %i", &len);
            }
		}
        
		std::vector<char> json(len + 1);
		s.read(json.data(), len);
		auto beg = json.begin();
		picojson::parse(obj, beg, json.end());
		return obj;
	}

	void Put(std::ostream& s, const picojson::value& rpc) {
		std::stringstream payload;
		auto str = rpc.serialize();
#ifdef WIN32
		auto wide = utf8filename(str);
		str.clear();
		for (auto c : wide) {
			if (c < 128) str.push_back((char)c);
			else {
				str += "\\u";
				const char hexdigit[] = { '0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','f' };
				for (int digit = 3;digit >= 0;--digit) {
					str += hexdigit[(c >> (digit * 4)) & 0xf];
				}
			}
		}
#endif
		str.push_back('\n');
		payload << "Content-Length: " << str.size() << CRLF CRLF;
		payload << str;
		auto pstr = payload.str();
		s.rdbuf()->sputn(pstr.data(), pstr.size());
		s.rdbuf()->pubsync();
	}
}
