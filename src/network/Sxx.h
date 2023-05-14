#pragma once

#include <iosfwd>
#include <streambuf>
#include <vector>
#include <functional>
#include <thread>
#include <string>
#include <memory>
#include <tuple>
#include <mutex>

#ifdef WIN32
#ifdef SXX_EXPORTS
#define SXX_API __declspec(dllexport)
#elseif defined(SXX_IMPORTS)
#define SXX_API __declspec(dllimport)
#else
#define SXX_API
#endif
#else
#define SXX_API
#endif

namespace Sxx {

	template <typename ITER> class iter_range : public std::tuple<ITER, ITER> {
	public:
		typedef ITER iterator;
		typedef const ITER const_iterator;
		iter_range(ITER begin, ITER end):std::tuple<ITER,ITER>(begin, end) { }
		ITER begin() { return std::get<0>(*this); }
		ITER end() { return std::get<1>(*this); }
		const ITER cbegin() const { return std::get<0>(*this); }
		const ITER cend() const { return std::get<1>(*this); }
		template <typename DST> operator iter_range<DST>() {
			return make_range(DST(begin()), DST(end()));
		}
		template <typename DST> operator iter_range<DST>() const {
			return make_range(DST(cbegin()), DST(cend()));
		}
	};

	template <typename ITER> iter_range<ITER> make_range(ITER b, ITER e) {
		return iter_range<ITER>(b, e);
	}

	class Socket;

    class Server {
		volatile bool stopSignal;
		std::thread server;
		bool acceptNetwork;
	public:
		SXX_API Server(Server &&s):stopSignal(std::move(s.stopSignal)), server(std::move(s.server)) { }
		SXX_API Server(bool acceptNonLocal = false);
		SXX_API ~Server() { BlockingShutdown(); }
		SXX_API Server& operator=(Server&& from);
		SXX_API Server& operator=(const Server&) = delete;
        SXX_API void BlockingTCP(const std::string&, std::function<void(Socket)>);
        SXX_API void BlockingUDP(const std::string&, std::function<void(Socket)>);
        SXX_API void TCP(const std::string& protocol, std::function<void(Socket)>);
		SXX_API void UDP(const std::string& protocol, std::function<void(Socket)>);
		SXX_API void Shutdown();
		SXX_API void BlockingShutdown();
		SXX_API bool IsRunning(int = 0);
	};

	namespace TCP {
		SXX_API Socket Connect(const std::string& hostAddress, const std::string& protocol);
	}

	namespace UDP {
		SXX_API Socket Connect(const std::string& hostAddress, const std::string& protocol);
	}

	class Socket {
		friend class Server;
		friend Socket TCP::Connect(const std::string&, const std::string&);
		friend Socket UDP::Connect(const std::string&, const std::string&);
		struct Pimpl;
		Pimpl *pimpl;
	public:
		enum Type {
			TCP = 1,
			UDP = 2,
			IPv4 = 4,
			IPv6 = 8,
			Input = 16,
			Output = 32
		};
	private:
		Type type;
		Socket(Type t, Pimpl* p);
	public:
		SXX_API Socket():pimpl(nullptr) { }
		Socket(const Socket&) = delete;
		Socket& operator=(const Socket&) = delete;
		SXX_API Socket(Socket&&);
		SXX_API Socket& operator=(Socket&&);
		SXX_API ~Socket();

		SXX_API bool ReadAvailable(int blockForMilliseconds = 0);
		SXX_API int Read(void* buffer, size_t maxBytes);
		SXX_API int Write(const void *buffer, size_t bytes);

		SXX_API iter_range<const char*> Read(std::vector<char>& useBuffer);
		SXX_API int Write(const std::vector<char>& v);
		int Write(iter_range<const char*> r) { return Write(r.begin(), r.end() - r.begin()); }

		SXX_API void Close();

		operator bool() const { return pimpl != nullptr; }
		Type GetType() const { return type; }

		bool IsIPv4() const { return (type & IPv4) != 0; }
		bool IsIPv6() const { return (type & IPv6) != 0; }
		bool CanRead() const { return (type & Input) != 0; }
		bool CanWrite() const { return (type & Output) != 0; }
		bool IsStream() const { return (type & TCP) != 0; }
		bool IsDatagram() const { return (type & UDP) != 0; }

		SXX_API std::string GetName();

		enum State {
			Ok = 1,
			Closed = 0,
			ConnectionRefused = -9999,
			ConnectionClosed,
			NotConnected,
			AccessDenied,
			UnknownHostname,
			AddressInUse,
			SocketBusy,
			DatagramOverflow,
			HostUnreachable,
			Waiting,
			ProtocolNotSupported,
			OtherError
		};

		struct Exception : public std::runtime_error {
			State s;
			Exception(int s, const char *what):std::runtime_error(what),s((State)s) { }			
		};

		struct Properties {
			bool ipv4, ipv6, can_read, can_write, tcp, udp;
		};

		Properties GetProperties() const {
			return Properties{ IsIPv4(), IsIPv6(), CanRead(), CanWrite(), IsStream(), IsDatagram() };
		}
	};

	SXX_API std::ostream& operator<<(std::ostream&, Socket::Properties props);

	class SocketBuffer : public std::streambuf {
		Socket& sock;
		std::vector<char> buffer;
		std::vector<char> write_buffer;
		int write_pos;
		bool allowUnderflow = true;
		SXX_API int underflow();
		SXX_API int overflow(int);
		SXX_API int sync();
		SXX_API std::streamsize showmanyc();
		SXX_API std::streamsize xsputn(const char* s, std::streamsize n);
		SXX_API int write(const void *data, int n);
		int sendloop_unsafe(const void *data, int n);
		int sync_unsafe();
		std::mutex readLock, writeLock;
	public:
		SocketBuffer(Socket& s, size_t bufferSize = 1024, size_t writeBuffer = 1024):sock(s), buffer(s.CanRead()?bufferSize:0), write_buffer(s.CanWrite()?writeBuffer:0), write_pos(0) { }
		~SocketBuffer() { try { sync(); } catch (...) { }; }
		SXX_API void diags(std::ostream&);
	};
}
