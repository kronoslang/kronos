#include <sstream>
#include <algorithm>
#include <regex>
#include <unordered_map>
#include <fstream>
#include <list>
#include <atomic>
#include <thread>
#include <iostream>
#include <mutex>

#include "CmdLineOpts.h"
#include "network/Sxx.h"
#include "network/websocket.h"
#include "config/system.h"

#include "kronos.h"
#include "ReplEnvironment.h"

#ifdef WIN32
#define EXPORT_SYMBOL __declspec(dllexport)
#else
#define EXPORT_SYMBOL
#endif

#define EXPAND_PARAMS                                                        \
  F(import, i, std::list<std::string>(), "<module>",                         \
    "Import source file <module>")                                           \
  F(import_path, ip, std::list<std::string>(),"<path>",						 \
    "Add paths to look for imports in")										 \
  F(port, p, std::string("15051"), "<protocol>",                             \
    "listen to TCP port <protocol> for http requests")                       \
  F(root, r, std::string(""), "<directory>", "serve files from <directory>") \
  F(help, h, false, "", "help; display this user guide")

namespace CL {
    using namespace CmdLine;
#define F(LONG, SHORT, DEFAULT, LABEL, DESCRIPTION) Option<decltype(DEFAULT)> LONG(DEFAULT, "--" #LONG, "-" #SHORT, LABEL, DESCRIPTION);
    EXPAND_PARAMS
#undef F
}

void FormatErrors(const std::string& xml, std::ostream& out,
                  Kronos::Context& cx);

std::unordered_map<std::string, std::string> percent_decode(
    const std::string& uri) {
  std::stringstream dec;
  std::regex keyval("([^=]+)=([\\s\\S]*)");
  std::unordered_map<std::string, std::string> kvmap;
  for (int i(0); i < uri.size(); ++i) {
    if (uri[i] == '%') {
      if (uri.size() > i + 2) {
        char hex[3] = {uri[i + 1], uri[i + 2], 0};
        dec << (char)strtol(hex, nullptr, 16);
        i += 2;
      } else
        break;
    } else if (uri[i] == '&') {
      std::smatch matches;
      std::string decoded = dec.str();
      if (std::regex_search(decoded, matches, keyval)) {
        kvmap[matches[1]] = matches[2];
      }
      dec.str("");
    } else {
      dec << uri[i];
    }
  }

  std::smatch matches;
  std::string decoded = dec.str();
  if (std::regex_search(decoded, matches, keyval)) {
    kvmap[matches[1]] = matches[2];
  }
  dec.clear();
  return kvmap;
}

void http_response(std::ostream& client, int code, std::string title,
                   std::string msg, std::string mime = "text/html") {
  client << "HTTP/1.1 " << code << " " << title << "\r\n"
         << "Content-Type: " << mime << "\r\n"
         << "Access-Control-Allow-Origin: *\r\n"
         << "Content-Length: " << msg.size() << "\r\n\r\n" << msg << "\r"
         << std::endl;
}

void transfer(std::ostream& out, std::istream& in, size_t count) {
  std::vector<char> buf(0x1000);
  while (count) {
    auto donow = std::min<size_t>(buf.size(), count);
    in.rdbuf()->sgetn(buf.data(), donow);
    out.rdbuf()->sputn(buf.data(), donow);
    count -= donow;
  }
}

void FormatErrorsEDN(const std::string& xml, std::ostream& out, Kronos::Context& cx);

namespace Kronos {
	std::unique_ptr<IO::IHierarchy> Hardware;

    class WebREPL {
		Kronos::Context cx;
		REPL::JiT::Compiler c;
		REPL::OstreamEnvironment env;
		std::stringstream out, err;
    public:
		std::mutex lock;
		WebREPL() : cx(CreateContext()), env(Hardware.get(), c, "(0 0)", out, err), c(cx) {
		}

		void FeedLine(const std::string& line) {
			env.Parse(line);
		}

		std::string FlushOut() {
			auto s = out.str();
			out.str("");
			return s;
		}
	};
}

std::unordered_map<std::string, Kronos::WebREPL> sessions;
std::mutex shared;
Sxx::Server listener;

int main(int argn, const char* carg[]) {
  using namespace Kronos;
  using namespace Sxx;

  try {
      std::list<const char*> args;
      Kronos::AddBackendCmdLineOpts(CmdLine::Registry());
      for (int i(1);i < argn;++i) args.emplace_back(carg[i]);
      CmdLine::Registry().Parse(args);
      
      if (CL::help()) {
          CL::Registry().ShowHelp(std::clog,
              "KREPL; Kronos " KRONOS_PACKAGE_VERSION
              " REPL \n(c) 2015 Vesa Norilo, University of Arts Helsinki\n\n");
          return 0;
      }
      
    if (CL::root().size() && CL::root().back() != '/') CL::root().push_back('/');
      
	Hardware = IO::CreateCompositeIO();

    listener.BlockingTCP(CL::port(), [&](Socket s) {

      std::cout << "[" << s.GetName() << "] " << s.GetProperties() << "\n";

	  SocketBuffer sbuf(s);
      std::iostream clientStream(&sbuf);

      std::regex methodParser("([A-Z]+)\\s+(\\S+)\\s+HTTP/1.1");
      std::regex headerParser("(\\S+):\\s+(.*)");
      std::regex urlParser("/([^/\\s]*)/(.*)");

      std::unordered_map<std::string, std::string> mimeType = {
          {".html", "text/html"},
          {".htm", "text/html"},
          {".css", "text/css"},
          {".jpg", "image/jpeg"},
          {".jpeg", "image/jpeg"},
          {".png", "image/png"},
          {".js", "application/javascript"},
          {".xml", "application/xml"}};

      enum ReqType { UNKNOWN, GET, POST } reqType;

      std::unordered_map<std::string, ReqType> httpMethods = {{"GET", GET},
                                                              {"POST", POST}};

      std::string requestUri, sessionId, paramString;
      bool closeConnection = false;

      std::unordered_map<std::string, std::string> httpHeaders;

      // read the http request
      while (clientStream.good() && clientStream.eof() == false && listener.IsRunning()) {
        std::string reqline;
        std::getline(clientStream, reqline, '\n');

        std::smatch matches;
        if (std::regex_search(reqline, matches, methodParser)) {
          auto m = httpMethods.find(matches[1]);
          if (m == httpMethods.end())
            reqType = UNKNOWN;
          else
            reqType = m->second;
          std::string uri = matches[2];
          if (std::regex_search(uri, matches, urlParser)) {
            sessionId = matches[1];
            requestUri = matches[2];
          } else {
            sessionId.clear();
            requestUri.clear();
          }
        } else if (std::regex_search(reqline, matches, headerParser)) {
          std::string header = matches[1];
          for (auto& c : header) c = tolower(c);
          httpHeaders[header] = matches[2];
        } else if (reqline == "\r") {
			WebREPL* repl_inst = nullptr; {
				std::lock_guard<std::mutex> lg(shared);
				try {
					repl_inst = &sessions[sessionId];
				} catch (std::exception& e) {
					std::cerr << "* " << e.what();
					abort();
				}
			}

			if (upgrade_websocket(httpHeaders, clientStream)) {
                if (requestUri == "display") {
                    
                }
			} else if (reqType == GET) {
            auto paramStart = requestUri.find_last_of('?');
            if (paramStart != requestUri.npos) {
              paramString = requestUri.substr(paramStart + 1);
              requestUri = requestUri.substr(0, paramStart);
            }
          } else if (reqType == POST) {
            auto cli = httpHeaders.find("content-length");
            std::stringstream params;
            if (cli != httpHeaders.end()) {
              int sz = strtol(cli->second.c_str(), nullptr, 10);
              transfer(params, clientStream, sz);
            } else if (httpHeaders["transfer-encoding"].find("chunked") !=
                       std::string::npos) {
              while (true) {
                std::string szs;
                std::getline(clientStream, szs);
                int sz = strtol(szs.c_str(), nullptr, 16);
                if (sz == 0) break;
                transfer(params, clientStream, sz);
              }
            }
            paramString = params.str();
          }

          if (reqType == GET || reqType == POST) {
            if (sessionId.empty()) {
              http_response(clientStream, 404, "No session",
                            "Please identify your session; session-id is the "
                            "first segment of the URI path");
            } else if (requestUri == "ping") {
              http_response(clientStream, 200, "Ok", "Pong");
            } else if (requestUri == "repl") {
				std::lock_guard<std::mutex> lg(repl_inst->lock);
                repl_inst->FeedLine(percent_decode(paramString)["program"]);
                http_response(clientStream, 200, "Ok", repl_inst->FlushOut());
            } else {
              auto extp = requestUri.find_last_of('.');
              std::string ext =
                  extp == std::string::npos ? "" : requestUri.substr(extp);

              std::ifstream file(CL::root() + requestUri,
                                 std::ios::binary | std::ios::ate);

              auto f = mimeType.find(ext);
              std::string mime = (f == mimeType.end()) ? "application/unknown" : f->second;

              if (file.is_open()) {
                std::vector<char> fileBuf(0x10000);
                size_t len = file.tellg();
                file.seekg(0, file.beg);
                clientStream
                    << "HTTP/1.1 200 OK\r\n"
                       "Content-Type: " << mime
                    << "\r\n"
                       "Connection: Keep-Alive\r\n"
                       "Server: kreplsrv/" KRONOS_PACKAGE_VERSION
                       "\r\n"
                       "Content-Length: " << len
                    << "\r\n\r\n";  

                while (len) {
                  size_t block = std::min(len, fileBuf.size());
                  file.read(fileBuf.data(), block);
                  clientStream.write(fileBuf.data(), block);
                  len -= block;
                }
              } else {
                std::stringstream notfound;
                notfound << "'" << requestUri << "' not available";
                http_response(clientStream, 404, "Not found", notfound.str());
              }
            }
          } else {
            http_response(clientStream, 501, "Not implemented", "HTTP method not supported");
          }

          clientStream.flush();
          httpHeaders.clear();
          requestUri.clear();
          sessionId.clear();
          paramString.clear();
          reqType = UNKNOWN;
          if (closeConnection) {
            s.Close();
            return;
          }
        }
      }
    });

    if (listener.IsRunning() == false) {
      throw std::runtime_error(
          "Network error while trying to listen on port '" + CL::port() + "'");
    }
    
    listener.BlockingShutdown();
  } catch (Kronos::IError& e) {
    std::cerr << "* Compiler Error: " << e.GetErrorMessage() << " *"
              << std::endl;
    return -2;
  } catch (std::range_error& e) {
    std::cerr << "* " << e.what() << " *" << std::endl;
    std::cerr << "Try '" << carg[0] << " -h' for a list of parameters\n";
    return -3;
  } catch (std::exception& e) {
    std::cerr << "* Runtime error: " << e.what() << " *" << std::endl;
    return -1;
  }
  return 0;
}
