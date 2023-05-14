#pragma once

#include <deque>
#include <cassert>
#include <algorithm>
#include <ostream>

#ifndef NDEBUG
#include <sstream>
#endif

template <int N> struct bitchunk {
	std::uint8_t bits[(N + 7) / 8];
	bitchunk(std::uint64_t from) {
		for (auto &b : bits) {
			b = from & 0xff;
			from >>= 8;
		}
	}

	std::uint64_t widen() const {
		std::uint64_t smp(0);
		for (auto &b : bits) {
			smp <<= 8;
			smp |= b;
		}
		return smp;
	}
};

template <typename ITER> static void base64_encode(std::ostream& os, ITER begin, ITER end) {
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
			os << base64_chars[(buf[0] & 0xfc) >> 2];
			os << base64_chars[((buf[0] & 0x03) << 4) + ((buf[1] & 0xf0) >> 4)];
			os << base64_chars[((buf[1] & 0x0f) << 2) + ((buf[2] & 0xc0) >> 6)];
			os << base64_chars[((buf[2] & 0x3f))];
			bp = 0;
		}
	}

	if (bp) {
		for (int i(bp); i < 3; ++i) buf[i] = 0;
		os << base64_chars[(buf[0] & 0xfc) >> 2];
		if (bp >= 1) os << base64_chars[((buf[0] & 0x03) << 4) + ((buf[1] & 0xf0) >> 4)];
		if (bp >= 2) os << base64_chars[((buf[1] & 0x0f) << 2) + ((buf[2] & 0xc0) >> 6)];
		while (bp++ < 3) os << '=';
	}
}

static const std::uint8_t pr2six[256] =
{
	/* ASCII table */
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
	52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
	64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
	15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
	64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
	41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
	64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
};


class bitstream : std::deque<std::uint8_t> {
	int used_pos = 8;
	void push_chunk(std::uint8_t byte, int num_used) {
		assert(num_used >= 0 && num_used <= 8);
		if (used_pos == 8) {
			deque::push_back( byte & ((1 << num_used) - 1) );
			used_pos = num_used;
		} else {
			auto last_byte = back();
			last_byte |= (byte << used_pos) & 0xff;
			back() = last_byte;
			used_pos += num_used;
			if (used_pos > 8) {
				byte >>= used_pos;
				deque::push_back(byte & ((1 << (used_pos - 8)) - 1));
				used_pos -= 8;
			}
		}
	}
public:
	template <int N> void push_back(const bitchunk<N>& chunk) {
		for (int i = 0; i < N; i += 8) {
			push_chunk(chunk.bits[i / 8], std::min(8, N - i));
		}
	}

	template <int N> bitchunk<N> at_bit(int bitoffset) const {
		bitchunk<N> chunk(0);
		for (int i = 0;i < (N + 7) / 8; ++i) {
			chunk.bits[i] = operator[](i / 8);
		}
		return chunk;
	}

	size_t num_bits() const {
		return deque::size() * 8;
	}

	void pad() {
		while (size() % 3) deque::push_back(0);
	}

	void base64_encode(std::ostream& out) {
		::base64_encode(out, begin(), end());
#ifndef NDEBUG
		std::stringstream b64;
		::base64_encode(b64, begin(), end());
		bitstream reference;
		reference.base64_decode(b64.str());
		for (int i = 0;i < size();++i) {
			assert(at(i) == reference.at(i));
		}
#endif
	}

	void base64_decode(const std::string& from) {
		clear();
		size_t left = from.size();
		const unsigned char* in = (const unsigned char*)from.data();
		while(left > 4) {
			deque::push_back(pr2six[in[0]] << 2 | pr2six[in[1]] >> 4);
			deque::push_back(pr2six[in[1]] << 4 | pr2six[in[2]] >> 2);
			deque::push_back(pr2six[in[2]] << 6 | pr2six[in[3]]);
			in += 4;
			left -= 4;
		}

		/* Note: (nprbytes == 1) would be an error, so just ingore that case */
		if (left > 1) {
			deque::push_back(pr2six[in[0]] << 2 | pr2six[in[1]] >> 4);
		}
		if (left  > 2) {
			deque::push_back(pr2six[in[1]] << 4 | pr2six[in[2]] >> 2);
		}
		if (left > 3) {
			deque::push_back(pr2six[in[2]] << 6 | pr2six[in[3]]);
		}
		used_pos = 8;
	}
};
