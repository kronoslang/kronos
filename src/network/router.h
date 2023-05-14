#pragma once

#include "Sxx.h"
#include "http.h"
#include <unordered_map>

namespace Sxx {

	using Responder = std::function<void(Sxx::Socket&, const std::string&, const http::Request&, http::Response&)>;

	Responder Route(std::unordered_map<std::string, Responder>);
	
	namespace Responders {
		Responder Static(const std::string& mime, const std::string& body);

		class IWebsocketStream {
		public:
			virtual const char* Read(std::vector<char>&) = 0;
			virtual void Write(const void*, size_t sz) = 0;
			virtual bool IsGood() = 0;
			virtual const http::Request& GetHttpRequest() const = 0;
		};

		Responder Websocket(std::function<void(IWebsocketStream&)>);
	}

	void Serve(const std::string& port, bool acceptNonLocal, const Responder&);
}