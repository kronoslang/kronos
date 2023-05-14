#include "websocket.h"
#include "config/system.h"
#include <array>
#include <cassert>
#include <cctype>
#include <limits>

#ifdef _WIN32
#include <WinSock2.h>
#pragma comment (lib, "Ws2_32.lib")
#else
#include <netinet/in.h>
#if !defined(HAVE_HTONLL) && !defined(htonll)
#define htonll(x) ((1==htonl(1)) ? (x) : ((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#endif
#if !defined(HAVE_NTOHLL) && !defined(ntohll)
#define ntohll(x) ((1==ntohl(1)) ? (x) : ((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
#endif
#endif

static unsigned CircularShift(int bits, unsigned word) {
	return ((word << bits) & 0xFFFFFFFF) | ((word & 0xFFFFFFFF) >> (32 - bits));
}

static void sha1_block(std::array<std::uint32_t, 5>& H, const std::array<std::uint8_t, 64>& block) {
	const unsigned K[] = {               // Constants defined for SHA-1
		0x5A827999,
		0x6ED9EBA1,
		0x8F1BBCDC,
		0xCA62C1D6
	};
	int         t;                          // Loop counter
	unsigned    temp;                       // Temporary word value
	unsigned    W[80];                      // Word sequence
	unsigned    A, B, C, D, E;              // Word buffers

	for (t = 0; t < 16; t++) {
		W[t] = ((unsigned)block[t * 4]) << 24;
		W[t] |= ((unsigned)block[t * 4 + 1]) << 16;
		W[t] |= ((unsigned)block[t * 4 + 2]) << 8;
		W[t] |= ((unsigned)block[t * 4 + 3]);
	}

	for (t = 16; t < 80; t++) {
		W[t] = CircularShift(1, W[t - 3] ^ W[t - 8] ^ W[t - 14] ^ W[t - 16]);
	}

	A = H[0];
	B = H[1];
	C = H[2];
	D = H[3];
	E = H[4];

	for (t = 0; t < 20; t++) {
		temp = CircularShift(5, A) + ((B & C) | ((~B) & D)) + E + W[t] + K[0];
		temp &= 0xFFFFFFFF;
		E = D;
		D = C;
		C = CircularShift(30, B);
		B = A;
		A = temp;
	}

	for (t = 20; t < 40; t++) {
		temp = CircularShift(5, A) + (B ^ C ^ D) + E + W[t] + K[1];
		temp &= 0xFFFFFFFF;
		E = D;
		D = C;
		C = CircularShift(30, B);
		B = A;
		A = temp;
	}

	for (t = 40; t < 60; t++) {
		temp = CircularShift(5, A) +
			((B & C) | (B & D) | (C & D)) + E + W[t] + K[2];
		temp &= 0xFFFFFFFF;
		E = D;
		D = C;
		C = CircularShift(30, B);
		B = A;
		A = temp;
	}

	for (t = 60; t < 80; t++) {
		temp = CircularShift(5, A) + (B ^ C ^ D) + E + W[t] + K[3];
		temp &= 0xFFFFFFFF;
		E = D;
		D = C;
		C = CircularShift(30, B);
		B = A;
		A = temp;
	}

	H[0] = (H[0] + A) & 0xFFFFFFFF;
	H[1] = (H[1] + B) & 0xFFFFFFFF;
	H[2] = (H[2] + C) & 0xFFFFFFFF;
	H[3] = (H[3] + D) & 0xFFFFFFFF;
	H[4] = (H[4] + E) & 0xFFFFFFFF;
}

template <typename ITER> std::array<std::uint32_t, 5> sha1(ITER begin, ITER end) {
	std::array<std::uint8_t, 64> block;
	
    std::array<std::uint32_t, 5> digest = {{
        0x67452301,
        0xEFCDAB89,
		0x98BADCFE,
		0x10325476,
		0xC3D2E1F0
    }};

	std::uint64_t len(0);

	int block_pos = 0;
	while (begin != end) {
		block[block_pos++] = std::uint8_t(*begin++ & 0xff);
		len+=8;
		if (block_pos >= block.size()) {
			sha1_block(digest, block);
			block_pos = 0;
		}
	}

	if (block_pos > 55) {
		block[block_pos++] = 0x80;
		while (block_pos < 64) block[block_pos++] = 0;
		sha1_block(digest, block);
		block_pos = 0;
		while (block_pos < 56) block[block_pos++] = 0;
	} else {
		block[block_pos++] = 0x80;
		while (block_pos < 56) block[block_pos++] = 0;
	}

	for (int bi = 0; bi < 8; bi++) {
		block[56 + bi] = (len >> (56 - bi * 8)) & 0xff;
	}

	sha1_block(digest, block);

	return digest;
}

static inline bool is_base64(unsigned char c) {
	return (isalnum(c) || (c == '+') || (c == '/'));
}

template <typename ITER> static void base64_encode(std::ostream& out, ITER begin, ITER end) {
	static const std::string base64_chars =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789+/";

	static_assert(sizeof(decltype(*begin)) == 1, "assuming byte data for now");

	char buf[3] = { 0 };
	int bp = 0;

	while (begin != end) {
		buf[bp++] = *begin++;
		if (bp == 3) {
			out << base64_chars[(buf[0] & 0xfc) >> 2];
			out << base64_chars[((buf[0] & 0x03) << 4) + ((buf[1] & 0xf0) >> 4)];
			out << base64_chars[((buf[1] & 0x0f) << 2) + ((buf[2] & 0xc0) >> 6)];
			out << base64_chars[((buf[2] & 0x3f))];
			bp = 0;
		}
	}

	if (bp) {
		for (int i(bp); i < 3; ++i) buf[i] = 0;
		out << base64_chars[(buf[0] & 0xfc) >> 2];
		if (bp>=1) out << base64_chars[((buf[0] & 0x03) << 4) + ((buf[1] & 0xf0) >> 4)];
		if (bp>=2) out << base64_chars[((buf[1] & 0x0f) << 2) + ((buf[2] & 0xc0) >> 6)];
		while (bp++ < 3) out << '=';
	}
}

static std::string to_lower(std::string s) {
	for (auto&c : s) c = std::tolower(c);
	return s;
}

void write_websocket(std::ostream& socketStream, const char *data, size_t size) {
	socketStream.rdbuf()->sputc('\x81');
	if (size < 126) {
		socketStream.rdbuf()->sputc(size & 0x7f);
	} else {
		if (size < 65536) {
			assert(size <= std::numeric_limits<std::uint16_t>::max());
			std::uint16_t sz = (std::uint16_t)size;
			sz = htons(sz);
			socketStream.rdbuf()->sputc(126);
			socketStream.write((char*)&sz, 2);
		} else {
			std::uint64_t sz = size;
			sz = htonll(sz);
			socketStream.rdbuf()->sputc(127);
			socketStream.write((char*)&sz, 8);
		}
	}
	socketStream.write(data, size);
	socketStream.flush();
}

const char* read_websocket(std::iostream& socketStream, std::vector<char>& buffer) {
	static std::vector<char> previous;
	unsigned char type;
	size_t writePos = 0;
	bool FIN;
	do {
		if (socketStream.rdbuf()->sgetn((char*)&type, 1) != 1) {
			throw std::runtime_error("WebSocket closed abruptly");
		};
		FIN = (type & 0x80) != 0;

		unsigned char szb = 0;
		uint64_t sz(0);
		if (socketStream.rdbuf()->sgetn((char*)&szb, 1) != 1) {
			throw std::runtime_error("WebSocket closed abruptly");
		}
		bool hasMask = (szb & 0x80) == 0x80;
		switch (szb & 0x7f) {
		case 126:
			{
				unsigned short szw;
				socketStream.read((char *)&szw, 2);
				sz = ntohs(szw);
				break;
			}
		case 127:
			{
				socketStream.read((char*)&sz, 8);
				sz = ntohll(sz);
				break;
			}
		default: sz = szb & 0x7f;
		}

		// opcode
		switch (type & 0x0f) {
		case 0x01: case 0x02: break;
		case 0x08:
			{
				char close[] = { (char)0x88, '\0'};
				socketStream.write(close, 2);
				return buffer.data();
				break;
			}
		case 0x09:
			{
				// ping
				assert(sz < 126);
				unsigned char pong[132] = { (unsigned char)((type & 0x0f) | 0x90), (unsigned char)sz, 0 };
				if (hasMask) sz += 4;
				socketStream.read((char*)pong + 2, sz);
				socketStream.write((char*)pong, sz + 2);
				FIN = false;
				continue;
			}
		case 0x0a:
			{
				// pong
				char ping[132];
				if (hasMask) sz += 4;
				socketStream.read(ping, sz);
				FIN = false;
				continue;
			}
		default:
			throw std::runtime_error("Undefined WebSocket opcode");
		}

		char masks[4] = { 0 };
		if (hasMask) {
			socketStream.read(masks, 4);
		}

		if (buffer.size() < writePos + sz) buffer.resize(writePos + sz);

		auto toDo = sz;
		char* writePtr = buffer.data() + writePos;
		while (toDo) {
			auto didRead = socketStream.rdbuf()->sgetn(writePtr, toDo);
			if (!didRead) {
				throw std::runtime_error("Websocket closed abruptly");
			}
			assert(didRead <= toDo);
			writePtr += didRead;
			toDo -= didRead;
		}

		if (hasMask) {
			for (int i(0); i < sz; ++i) {
				buffer[writePos + i] ^= masks[i & 3];
			}
		} 
		writePos += sz;
	} while (!FIN);

	previous = buffer;
	previous.resize(writePos);

	return buffer.data() + writePos;
}

bool upgrade_websocket(const std::unordered_map<std::string, std::string>& headers, std::ostream& response) {
	auto hdr = [&headers](const std::string& key) -> std::string {
		assert(to_lower(key) == key);
		auto f = headers.find(key);
		if (f == headers.end()) return std::string();
		else return f->second;
	};

	auto con = to_lower(hdr("connection"));
	if (to_lower(hdr("upgrade")) != "websocket" || con.find("upgrade") == con.npos) return false;

	response <<
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: WebSocket\r\n"
		"Connection: Upgrade\r\n";

	std::string accept_key = hdr("sec-websocket-key") + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

	auto sha1_key = sha1(accept_key.cbegin(), accept_key.cend());

	union endian {
		char bytes[4] = { 1,2,3,4 };
		std::uint32_t dword;
	} et;


	if (et.dword == 0x04030201) {
		for (auto& s : sha1_key) {
			et.dword = s;
			std::swap(et.bytes[0], et.bytes[3]);
			std::swap(et.bytes[1], et.bytes[2]);
			s = et.dword;
		}
	} else assert(et.dword == 0x01020304);

	response << "Sec-WebSocket-Accept: ";
	base64_encode(response, (const std::uint8_t*)sha1_key.data(), (const std::uint8_t*)(sha1_key.data()+sha1_key.size()));

	response << "\r\n\r" << std::endl;
	return true;
}
