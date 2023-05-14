int main(int, const char*[]);

#include <fcntl.h>
#include <io.h>

#include <iostream>
#include <vector>
#include <codecvt>
#include <mutex>
#include <cassert>         // assert
#include <iostream>         // std::wcerr
#include <streambuf>        // std::basic_streambuf
#include <algorithm>
#include <Windows.h>

using namespace std;

#define WIN_TEXT_MODE	_O_U8TEXT

template <class CVT> class buffer_convert : public std::streambuf, std::recursive_mutex {
	CVT cvt;
	std::wstreambuf& backing_stream;
	std::vector<char> buffer;
	using mutex_t = std::recursive_mutex;
public:
	buffer_convert(std::wstreambuf& backing, int buffer_size = 1024) :backing_stream(backing), buffer(buffer_size) {
		setp(buffer.data(), buffer.data() + buffer.size());
	}

	streamsize showmanyc() {
		std::lock_guard<mutex_t> l(*this);
		return backing_stream.in_avail();
	}

	int underflow() {
		std::lock_guard<mutex_t> l(*this);

		int oldmode = 0;

		wchar_t transcode[1024];
		const wchar_t *u16b = transcode;
		const wchar_t *u16e = transcode + backing_stream.sgetn(transcode, std::min(1024ll, backing_stream.in_avail()));

		if (u16e == u16b) {
			int ch = backing_stream.sbumpc();
			if (ch == wstreambuf::traits_type::eof()) {
				return traits_type::eof();
			}
			transcode[0] = ch;
			u16e = transcode + 1;
		}

		char *u8e(nullptr);
		const wchar_t* u16m(nullptr);

		std::mbstate_t read{ 0 };

		switch (cvt.out(read, u16b, u16e, u16m, buffer.data(), buffer.data() + buffer.size(), u8e)) {
		case std::codecvt_base::error: return traits_type::eof();
		default:
			for (; u16e > u16m; --u16e) backing_stream.sputbackc(u16e[-1]);
			setg(buffer.data(), buffer.data(), u8e);
			return traits_type::to_char_type(buffer.front());
			break;
		}
	}

	int sync() {
		std::lock_guard<mutex_t> l(*this);
		wchar_t transcode[1024];
		wchar_t* u16b(transcode);
		const char *u8(nullptr);

		for (u8 = buffer.data(); u8 < pptr();) {
			const char *u8nxt(pptr());
			wchar_t* u16e(nullptr);
			std::mbstate_t write{ 0 };
			switch (cvt.in(write, u8, u8nxt, u8nxt, u16b, transcode + 1024, u16e)) {
			case std::codecvt_base::error:
				return traits_type::eof();
			case std::codecvt_base::partial:
			default:
				auto todo = u16e - u16b;
				while (todo) {
					// cast: return value is small
					auto did = (int)backing_stream.sputn(u16b, todo);
					if (did == 0) {
						return traits_type::eof();
					}
					todo -= did;
					u16b += did;
				}
				break;
			}

			if (u8nxt == u8) break;
			else u8 = u8nxt;
		}

		backing_stream.pubsync();

		// reschedule any code points that were part of a partial character
		size_t unsent = pptr() - u8;
		memcpy(buffer.data(), u8, unsent);
		setp(buffer.data(), buffer.data() + unsent, buffer.data() + buffer.size());
		return ~traits_type::eof();
	}

	int overflow(int ch) {
		if (ch == traits_type::eof()) return ch;
		if (sync() == traits_type::eof()) {
			return traits_type::eof();
		}

		std::lock_guard<mutex_t> l(*this);
		return sputc(ch);
	}

	streamsize xsputn(const char *buf, streamsize n) {
		std::lock_guard<mutex_t> l(*this);

		streamsize total = n;
		while (n > 0) {
			auto avail = std::min<streamsize>(epptr() - pptr(), n);
			
			while (avail) {
				int copyChunk = (int)avail;
				if (avail > std::numeric_limits<int>::max()) copyChunk = std::numeric_limits<int>::max();
				memcpy(pptr(), buf, copyChunk);
				avail -= copyChunk;
				pbump(copyChunk); buf += copyChunk; n -= copyChunk;
			}
			if (sync() == traits_type::eof()) {
				break;
			}
		}
		return total - n;
	}
};

void set_win32_console_utf8() {
	// Forward narrow character output to the wide streams:
	static buffer_convert<std::codecvt_utf8_utf16<wchar_t>> cout_cvt(*wcout.rdbuf());
	static buffer_convert<std::codecvt_utf8_utf16<wchar_t>> cerr_cvt(*wcerr.rdbuf());
	static buffer_convert<std::codecvt_utf8_utf16<wchar_t>> clog_cvt(*wclog.rdbuf());
	static buffer_convert<std::codecvt_utf8_utf16<wchar_t>> cin_cvt(*wcin.rdbuf());

	// windows be sane
	_setmode(_fileno(stdin), WIN_TEXT_MODE);
	_setmode(_fileno(stdout), WIN_TEXT_MODE);
	_setmode(_fileno(stderr), WIN_TEXT_MODE);

	cout.rdbuf(&cout_cvt);
	cerr.rdbuf(&cerr_cvt);
	clog.rdbuf(&clog_cvt);
	cin.rdbuf(&cin_cvt);
}

int wmain(int argc, const wchar_t* argv[]) {
	using namespace std;

#ifdef NDEBUG
	set_win32_console_utf8();
#endif

	auto chnd = GetStdHandle(STD_OUTPUT_HANDLE);
	if (chnd != INVALID_HANDLE_VALUE) {
		DWORD mode = 0;
		GetConsoleMode(chnd, &mode);
		mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		SetConsoleMode(chnd, mode);
	}

	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
	vector<const char*> nargv(argc);
	vector<string> nargs(argc);

	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);

	for (int i = 0; i < argc; ++i) {
		nargs[i] = cvt.to_bytes(argv[i]);
		nargv[i] = nargs[i].c_str();
	}

	auto result = main(argc, nargv.data());
	cout.flush(); cerr.flush(); clog.flush();
	return result;
}