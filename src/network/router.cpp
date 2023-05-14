#include "router.h"
#include "websocket.h"

namespace Sxx {
	Responder Route(std::unordered_map<std::string, Responder> routes) {
		return [routes = std::move(routes)](Sxx::Socket& sock, const std::string& uri, const http::Request& request, http::Response& resp) {
			auto segment = uri.substr(0, uri.find_first_of('/', 1));
			auto subRoute = routes.find(segment);
			if (subRoute != routes.end()) {
				std::string nuri = uri;
				if (segment.size() != uri.size()) {
					nuri = nuri.substr(segment.size() + 1);
				} 
				subRoute->second(sock, nuri, request, resp);
			} 
		};
	}

	void Serve(const std::string& port, bool acceptNonLocal, const Responder& r) {
		Sxx::Server server(acceptNonLocal);
		server.BlockingTCP(port, [&](Socket s) {
            SocketBuffer socketBuf(s);
            std::iostream clientStream(&socketBuf);
			auto request = http::Parse(clientStream);
			http::Response response;

			auto uri = request.Uri.empty() ? "" : request.Uri.substr(1);
			request.Peer = s.GetName();

			r(s, http::UrlDecode(uri), request, response);
			clientStream << response;
		});

		if (server.IsRunning() == false) {
			throw std::runtime_error(
				"Network error while trying to listen on port '" + port + "'");
		}

		server.BlockingShutdown();
	}

	namespace Responders {
		Responder Static(const std::string& mime, const std::string& body) {
			return [mime, body](Sxx::Socket&, const std::string&, const http::Request& req, http::Response& resp) {
				resp.ResultCode = http::Response::OK;
				resp.Headers.emplace("Content-Type", mime);
				resp.Body = { body.data(), body.data() + body.size() };
			};
		}

		struct WebsocketStream : public IWebsocketStream {
			std::iostream& sock;
			const http::Request& req;
			WebsocketStream(const http::Request& req, std::iostream& s) :sock(s),req(req) {}
			
			const char *Read(std::vector<char>& read) override {
				return read_websocket(sock, read);
			}

			void Write(const void* write, size_t numBytes) override {
				write_websocket(sock, (const char*)write, numBytes);
			}

			bool IsGood() override {
				return sock.good() && !sock.eof();
			}

			const http::Request& GetHttpRequest() const {
				return req;
			}
		};

		Responder Websocket(std::function<void(IWebsocketStream&)> client) {
			return[client = std::move(client)](Sxx::Socket& s, const std::string& uri, const http::Request& req, http::Response& resp) {
				SocketBuffer sbuf(s);
				std::iostream socketStream(&sbuf);
				if (upgrade_websocket(req.Headers, socketStream)) {
					WebsocketStream wss{ req, socketStream };
					client(wss);
				}
			};
		}

	}
}
