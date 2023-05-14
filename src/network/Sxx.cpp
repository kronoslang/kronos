#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <IPHlpApi.h>
#include <mutex>

static void InitSockets() {	
	static std::mutex initLock;
	initLock.lock();
	static bool WinsockInitialized = false;
	if (WinsockInitialized == false) {
		WSADATA data;
		if (!WinsockInitialized) WSAStartup(MAKEWORD(2, 2), &data);
		WinsockInitialized = true;
	}
	initLock.unlock();
}

static int poll(pollfd* fds, int fdcount, int timeout) {
	return WSAPoll(fds, fdcount, timeout);
}


typedef int socklen_t;
#pragma comment (lib, "Ws2_32.lib")
#else
#ifdef __APPLE__
#include <netinet/in.h>
#include <sys/sockio.h>
#include <sys/ioctl.h>   
#endif
#include <arpa/inet.h>
#include <sys/ioctl.h>   
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>

#define SOCKET_ERROR -1
#define INVALID_SOCKET -1
typedef int SOCKET;
static int closesocket(SOCKET s) { return close(s); }
#define ioctlsocket ioctl
static void InitSockets() {
    signal(SIGPIPE, SIG_IGN);
}

static int WSAGetLastError() {
	return errno;
}
#endif

#include <memory>
#include <algorithm>
#include <list>
#include <thread>
#include <iostream>
#include "Sxx.h"

namespace Sxx {

	int CastSize(size_t st) {
		if (st > size_t(std::numeric_limits<int>::max())) throw Socket::Exception(Socket::OtherError, "Transaction size overflows 32-bit integer");
		else return static_cast<int>(st);
	}

	template <typename T> T ValidateSocketOp(T code) {
		if (code == SOCKET_ERROR) {
			std::string error;
#ifdef WIN32
			char wmsg[256];
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), wmsg, sizeof(wmsg), NULL);
			error = wmsg;
			Socket::State code = Socket::OtherError;
			switch (WSAGetLastError()) {
			case WSA_NOT_ENOUGH_MEMORY: throw std::bad_alloc();
			case WSAEACCES: code = Socket::AccessDenied; break;
			case WSAEALREADY:
			case WSAEISCONN:
			case WSAEINPROGRESS: code = Socket::SocketBusy; break;
			case WSAENOBUFS:
			case WSAEMSGSIZE: code = Socket::DatagramOverflow; break;
			case WSAEADDRINUSE: code = Socket::AddressInUse; break;
			case WSAEHOSTUNREACH:
			case WSAENETUNREACH: code = Socket::HostUnreachable; break;
			case WSAECONNABORTED:
			case WSAECONNRESET:
			case WSAEHOSTDOWN:
			case WSAEDISCON:
			case WSAENETRESET: code = Socket::ConnectionClosed; break;
			case WSAECONNREFUSED: code = Socket::ConnectionRefused; break;
			case WSAESHUTDOWN:
			case WSAENOTCONN: code = Socket::NotConnected; break;
			case WSATRY_AGAIN:
			case WSAELOOP: code = Socket::UnknownHostname; break;
			default: return code;
			}
#else
			error = strerror(errno);
			switch (errno) {
			case EPERM: code = Socket::AccessDenied; break;
			case ENOMEM: throw std::bad_alloc();
			case EDESTADDRREQ:
			case EFAULT: code = Socket::UnknownHostname; break;
			case ENETDOWN:
			case EPIPE: code = Socket::ConnectionClosed; break;
			case EALREADY:
			case EADDRINUSE: code = Socket::AddressInUse; break;
			case EINPROGRESS: code = Socket::SocketBusy; break;
			case EMSGSIZE: code = Socket::DatagramOverflow; break;
			case EHOSTUNREACH:
			case ENETUNREACH: code = Socket::HostUnreachable; break;
			}
#endif
			throw Socket::Exception(code, error.c_str());
		}
		return code;
	}

	struct Socket::Pimpl {
		SOCKET s;
		Pimpl(const Pimpl& p) = delete;
		Pimpl& operator=(const Pimpl& p) = delete;
		Pimpl(SOCKET c) :s(c) {}
		~Pimpl() {
			ValidateSocketOp(closesocket(s));
#ifdef WIN32
			ValidateSocketOp(shutdown(s, SD_BOTH));
#endif
		}

		int Read(void* buffer, int maxBytes) {
			for (;;) {
				auto didRead = recv(s, (char*)buffer, maxBytes, 0);
				ValidateSocketOp(didRead);
				return didRead;
			}
		}

		int Write(const void *buffer, int bytes) {
			if (bytes < 1) return 0;
			return send(s, (const char*)buffer, bytes, 0);
		}

		int Avail() {
			unsigned long bytes;
			ValidateSocketOp(ioctlsocket(s, FIONREAD, &bytes));
			return bytes;
		}

		bool ReadAvailable(int block) {
            pollfd fds;
            fds.fd = s;
            fds.events = POLLIN;
			return ValidateSocketOp(poll(&fds,1,block)) > 0;
		}

		std::string GetName() {
			sockaddr_storage sa;
			socklen_t sa_len = sizeof(sa);
			ValidateSocketOp(getpeername(s, (sockaddr*)&sa, &sa_len));
			char host[1024];
			char service[1024];
			ValidateSocketOp(getnameinfo((sockaddr*)&sa, sa_len, host, 1024, service, 1024, 0));
			return std::string(host) + ":" + std::string(service);
		}
	};

	Socket::Socket(Socket::Type t, Socket::Pimpl* p) :pimpl(p), type(t) {}
	Socket::~Socket() { delete pimpl; }
	void Socket::Close() { delete pimpl; pimpl = nullptr; type = (Type)0; }
	Socket::Socket(Socket&& from) : pimpl(from.pimpl), type(from.type) { from.pimpl = nullptr; }
	Socket& Socket::operator=(Socket&& from) { std::swap(pimpl, from.pimpl); type = from.type; return *this; }
	int Socket::Read(void *buffer, size_t maxBytes) { auto num = pimpl ? pimpl->Read(buffer, CastSize(maxBytes)) : 0; return num; }
	int Socket::Write(const void *buffer, size_t bytes) { auto num = pimpl ? pimpl->Write(buffer, CastSize(bytes)) : 0; return num; }
	bool Socket::ReadAvailable(int millisecondsToBlock) { return pimpl && pimpl->ReadAvailable(millisecondsToBlock); }

	std::string Socket::GetName() { return pimpl ? pimpl->GetName() : "not connected"; }

	std::ostream& operator<<(std::ostream& p, Socket::Properties props) {
		p << (props.tcp ? "TCP" : "UDP") << "/" << (props.ipv6 ? "IP6" : "IPv6") << " [";
		if (props.can_read) p << "r";
		if (props.can_write) p << "w";
		p << "]";
		return p;
	}

	iter_range<const char*> Socket::Read(std::vector<char>& dst) {
		return make_range<const char*>(dst.data(), dst.data() + Read(dst.data(), dst.size()));
	}

	int Socket::Write(const std::vector<char>& v) {
		return Write(v.data(), v.size());
	}

	static bool SetSocketBlockingEnabled(SOCKET fd, bool blocking) {
		if (fd < 0) return false;

#ifdef WIN32
		unsigned long mode = blocking ? 0 : 1;
		return (ioctlsocket(fd, FIONBIO, &mode) == 0) ? true : false;
#else
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags < 0) return false;
		flags = blocking ? (flags&~O_NONBLOCK) : (flags | O_NONBLOCK);
		return (fcntl(fd, F_SETFL, flags) == 0) ? true : false;
#endif
	}

	static SOCKET Connect(const std::string& remoteAddress, const std::string& protocol, bool tcp) {
		InitSockets();

		sockaddr_storage servaddr;
		memset(&servaddr, 0, sizeof(servaddr));
		addrinfo hints, *address;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = tcp ? SOCK_STREAM : SOCK_DGRAM;
		hints.ai_protocol = tcp ? IPPROTO_TCP : IPPROTO_UDP;
		getaddrinfo(remoteAddress.c_str(), protocol.c_str(), NULL, &address);

		SOCKET sock = INVALID_SOCKET;
		for (addrinfo *a = address; a != NULL; a = a->ai_next) {
			sock = socket(address->ai_family, a->ai_socktype, tcp ? a->ai_protocol : IPPROTO_UDP);
			if (sock != INVALID_SOCKET) {
				if (connect(sock, a->ai_addr, CastSize(a->ai_addrlen)) == 0) {
					break;
				}
				closesocket(sock);
				sock = INVALID_SOCKET;
			}
		}

		freeaddrinfo(address);
		return sock;
	}

	addrinfo* GetAddressInfo(const std::string& port, int protocol, bool datagram) {
		addrinfo hints, *servinfo;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = datagram ? SOCK_DGRAM : SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		hints.ai_protocol = protocol;

		if (getaddrinfo(NULL, port.c_str(), &hints, &servinfo)) {
			throw std::runtime_error("Couldn't set up server addrinfo");
		} else return servinfo;
	}

	namespace TCP {

		SXX_API Socket Connect(const std::string& remoteAddress, const std::string& protocol) {
			auto sock = ValidateSocketOp(Sxx::Connect(remoteAddress, protocol, true));
			socklen_t sockaddrlen(sizeof(sockaddr_storage));
			sockaddr_storage sockname;
			getsockname(sock, (sockaddr*)&sockname, &sockaddrlen);
			return Socket(Socket::Type(Socket::TCP + Socket::Input + Socket::Output + (sockname.ss_family == AF_INET6 ? Socket::IPv6 : Socket::IPv4)), new Socket::Pimpl(sock));
		}
	}

	namespace UDP {
		SXX_API Socket Connect(const std::string& remoteAddress, const std::string& protocol) {
			auto sock = ValidateSocketOp(Sxx::Connect(remoteAddress, protocol, false));
			socklen_t sockaddrlen(sizeof(sockaddr_storage));
			sockaddr_storage sockname;
			getsockname(sock, (sockaddr*)&sockname, &sockaddrlen);
			return Socket(Socket::Type(Socket::UDP + Socket::Output + (sockname.ss_family == AF_INET6 ? Socket::IPv6 : Socket::IPv4)), new Socket::Pimpl(sock));
		}
	}

	std::streamsize SocketBuffer::showmanyc() {
		std::lock_guard<std::mutex> lg{ readLock };
		return sock.ReadAvailable(0) ? 1 : 0;
	}

	void SocketBuffer::diags(std::ostream& s) {
		std::lock_guard<std::mutex> lg{ writeLock };
		for (auto& b : write_buffer) b = 0;
		s << "[SOCKET] " << write_pos << "/" << write_buffer.size() << " : " << allowUnderflow << "\n";
	}

	int SocketBuffer::underflow() {
		std::lock_guard<std::mutex> lg{ readLock };
		if (allowUnderflow == false) {
			return traits_type::eof();
		} else if (sock.IsDatagram()) allowUnderflow = false;

		if (gptr() < egptr()) return traits_type::to_int_type(*gptr());

		int didRead = sock.Read(buffer.data(), buffer.size());
		setg(buffer.data(), buffer.data(), buffer.data() + didRead);

		if (didRead < 1) {
			return traits_type::eof();
		}
		return traits_type::to_int_type(buffer.front());
	}

	int SocketBuffer::sendloop_unsafe(const void *data, int n) {
		const char *data_ptr = (const char *)data;
		int didWrite = 0;
		while (n > 0) {
			int sent = sock.Write(data_ptr, n);
			if (sent < 1) break;
			didWrite += sent;
			data_ptr += sent;
			n -= sent;
			if (sock.IsDatagram()) break;
		}
		return didWrite;
	}

	int SocketBuffer::sync_unsafe() {
		allowUnderflow = true;
		if (sendloop_unsafe(write_buffer.data(), write_pos) == write_pos) {
			write_pos = 0;
			return 0;
		} else {
			write_pos = 0;
			return -1;
		}
	}

	int SocketBuffer::sync() {
		std::lock_guard<std::mutex> lg{ writeLock };
		return sync_unsafe();
	}

	int SocketBuffer::write(const void *data, int n) {
		std::unique_lock<std::mutex> lg{ writeLock };
		if (write_buffer.size() == 0) {
			return sendloop_unsafe(data, n);
		}

		if (n >= (int)write_buffer.size()) {
			// heuristic to send unbuffered
			sync_unsafe();
			return sendloop_unsafe(data, n);
		}

		const char *data_ptr = (const char *)data;
		int didWrite = 0;
		while (n > 0) {
			int avail = std::min<int>((int)write_buffer.size() - write_pos, n);
			memcpy(write_buffer.data() + write_pos, data_ptr, avail);
			didWrite += avail;
			write_pos += avail;
			data_ptr += avail;
			n -= avail;
			if (write_pos >= (int)write_buffer.size()) {
				if (sock.IsDatagram()) break;
				if (sync_unsafe() < 0) break;
			}
		}
		return didWrite;
	}

	int SocketBuffer::overflow(int c) {
		if (c != traits_type::eof()) {
			auto wr = traits_type::to_char_type(c);
			write(&wr, sizeof(wr));
		}
		return c;
	}

	std::streamsize SocketBuffer::xsputn(const char *data, std::streamsize n) {
		return write(data, int(n));
	}

	Server::Server(bool acceptNonLocal):acceptNetwork(acceptNonLocal) {
		InitSockets();
	}

	void Server::BlockingTCP(const std::string& protocol, std::function<void(Socket)> connectionHandler) {
        addrinfo* servinfo = GetAddressInfo(protocol, IPPROTO_TCP, false);
        std::vector<std::thread> servers;

		stopSignal = false;

        for (addrinfo *p = servinfo; p != NULL; p = p->ai_next) {
            SOCKET listenSock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (listenSock == SOCKET_ERROR) continue;

            if (bind(listenSock, p->ai_addr, CastSize(p->ai_addrlen)) == SOCKET_ERROR) {
                closesocket(listenSock);
                continue;
            } else {
                if (listen(listenSock, 100) == SOCKET_ERROR) {
                    closesocket(listenSock);
                    continue;
                }

                servers.emplace_back([this, listenSock, &connectionHandler]() {
                    std::vector<std::thread> connections;
                    while (this->IsRunning(0)) {
                        pollfd fds;
                        fds.fd = listenSock;
                        fds.events = POLLIN;
                        
                        int result = poll(&fds,1,100);
                        if (result < 0) {
                            fprintf(stderr, "** Select error %i\n",result);
                        } else if (result > 0) {
							sockaddr_storage storage;
							socklen_t sockaddrlen = sizeof(sockaddr_storage);
							SOCKET client = accept(listenSock, (sockaddr*)&storage, &sockaddrlen);
                            if (client != SOCKET_ERROR) {
                                int sockFlags = Socket::TCP | Socket::Input | Socket::Output | (storage.ss_family == AF_INET6 ? Socket::IPv6 : Socket::IPv4);

								if (!acceptNetwork) {
									sockaddr_in remote;
									socklen_t len = sizeof(remote);
									getpeername(client, (sockaddr*)&remote, &len);
									char rname[64] = { 0 }, lname[64] = { 0 };
									inet_ntop(storage.ss_family, &remote, rname, 64);
									inet_ntop(storage.ss_family, &storage, lname, 64);
									if (strcmp(rname, lname)) {
										closesocket(client);
									}
								}

                                connections.emplace_back([=,&connectionHandler]() {
                                    try {
                                        connectionHandler(Socket((Socket::Type)sockFlags, new Socket::Pimpl(client)));
                                    } catch (std::exception& e) {
                                        fprintf(stderr, "** Unhandled exception in TCP Connection Handler: %s", e.what());
                                        abort();
                                    }
                                });
                            } 
                        }
                    }
                    closesocket(listenSock);
                    while (connections.size()) {
                        if (connections.back().joinable()) connections.back().join();
                        connections.pop_back();
                    }
                });
            }
        }
        while (servers.size()) {
            if (servers.back().joinable()) servers.back().join();
            servers.pop_back();
        }
        freeaddrinfo(servinfo);
        this->Shutdown();
	}
    
	void Server::BlockingUDP(const std::string& protocol, std::function<void(Socket)> connectionHandler) {

		addrinfo* servinfo = GetAddressInfo(protocol, IPPROTO_UDP, true);

        std::vector<std::thread> waitThreads;

        for (addrinfo *p = servinfo; p != NULL; p = p->ai_next) {
            SOCKET listenSock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (listenSock == SOCKET_ERROR) continue;

            if (bind(listenSock, p->ai_addr, CastSize(p->ai_addrlen)) != SOCKET_ERROR) {
                waitThreads.emplace_back([=,&connectionHandler]() {
                    socklen_t sockaddrlen = sizeof(sockaddr_storage);
                    sockaddr_storage storage;
                    getsockname(listenSock, (sockaddr*)&storage, &sockaddrlen);
                    int sockFlags = Socket::UDP | Socket::Input | (storage.ss_family == AF_INET6 ? Socket::IPv6 : Socket::IPv4);
                    while (this->IsRunning(0)) {
                        connectionHandler(Socket((Socket::Type)sockFlags, new Socket::Pimpl(listenSock)));
                    }
                });
                continue;
            }
        }

        freeaddrinfo(servinfo);
        if (waitThreads.empty()) throw std::runtime_error("Couldn't listen on any UDP sockets");

        while (waitThreads.size()) {
            waitThreads.back().join();
            waitThreads.pop_back();
        }
	}
 
    void Server::TCP(const std::string& protocol, std::function<void(Socket)> handler) {
        Shutdown();
        stopSignal = false;
        server = std::thread([this,&protocol](std::function<void(Socket)> h){
            BlockingTCP(protocol, std::move(h));
        }, std::move(handler));
    }
    
    void Server::UDP(const std::string& protocol, std::function<void(Socket)> handler) {
        Shutdown();
        stopSignal = false;
        server = std::thread([this,&protocol](std::function<void(Socket)> h){
            BlockingUDP(protocol, std::move(h));
        }, std::move(handler));
    }
    
	void Server::Shutdown() {
		stopSignal = true;
	}

	void Server::BlockingShutdown() {
		Shutdown(); if (server.joinable()) server.join();
	}

	bool Server::IsRunning(int timeout) {
		if (stopSignal) return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
		return !stopSignal;
	}

	Server& Server::operator=(Server&& from) {
		stopSignal = from.stopSignal;
		server = std::move(from.server);
		return *this;
	}
}
