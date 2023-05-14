#pragma once
// AC97 control interface related addresses:
#define AC97_IN_FIFO_ADR     		 0x0000  (playback channel)
#define AC97_OUT_FIFO_ADR            0x0004  (recording channel)
#define AC97_FIFO_STATUS_ADR         0x0008
#define AC97_FIFO_CTRL_ADR           0x000C
#define AC97_CTRL_ADR   	         0x0010
#define AC97_READ_ADR   	         0x0014
#define AC97_WRITE_ADR  	         0x0018
// Specific registers to initialize the DAC and ADC sampling rates
#define AC97_SAMPLERATE_DAC_ADR		0x002C			 
#define AC97_SAMPLERATE_ADC_ADR		0x0032			 
//
// Stream controller registers
#define STREAMCONTROLLER_BASE     0x01000000 // DSP control register 
#define STREAMCONTROLLER_DSP_CNTR 0x01000000 // DSP control register 
#define ADCTOKENADDR_LSPART		  0x01000001
#define ADCTOKENADDR_MSPART		  0x01000002
#define DACTOKENADDR_LSPART		  0x01000003
#define DACTOKENADDR_MSPART		  0x01000004
#define SCHEDULER_BASE            0x01000080 // Scheduler base address
//
//
// Event controller registers:
#define EVENTCONTROLLER_CNTR      0x06000000 // Control register
#define EVENTCONTROLLER_ADDRLS    0x06000001 // Least significant part (16-bit) of indirect address
#define EVENTCONTROLLER_ADDRMS    0x06000002 // Most significant part (16-bit) of indirect address
#define EVENTCONTROLLER_DATA      0x06000003 // Virtual data register (write initiates transaction to indirect address, with post-increment on address register)
// LUT memory base address
#define LUTBASEADDRESS            0x09000000 // LUT memory base address
// DDR base address
#define DDRBASEADDRESS            0x0A000000 // DDR memory base address

struct SerialPort {
	HANDLE port;
	DCB config;
	SerialPort(int com) {
		wchar_t buf[16];
		wsprintf(buf, L"COM%i", com);
		port = CreateFile(buf, GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING, 0, 0);
		if (port == INVALID_HANDLE_VALUE)     throw std::runtime_error("Couldn't open COM port");
		memset(&config, 0, sizeof(config));
		config.DCBlength = sizeof(config);
		if (GetCommState(port, &config) == 0) throw std::runtime_error("Couldn't read COM port mode");
		config.BaudRate = CBR_38400;
		config.StopBits = ONESTOPBIT;
		config.ByteSize = DATABITS_8;
		config.Parity = ODDPARITY;
		config.fBinary = true;
		config.fParity = true;
		config.fDtrControl = DTR_CONTROL_ENABLE;
		config.fRtsControl = RTS_CONTROL_ENABLE;
		if (SetCommState(port, &config) == 0) throw std::runtime_error("Couldn't set COM port mode");
	}

	int Write(std::vector<char>& buffer) {
		DWORD didWrite(0);
		WriteFile(port, buffer.data(), (DWORD)buffer.size(), &didWrite, NULL);
		return didWrite;
	}

	int Read(std::vector<char>& buffer) {
		DWORD didRead(0);
		ReadFile(port, buffer.data(), (DWORD)buffer.size(), &didRead, NULL);
		return didRead;
	}

	void Encode(DWORD dw, std::vector<char>& buffer) {
		buffer.push_back((dw >> 0) & 0xff);
		buffer.push_back((dw >> 8) & 0xff);
		buffer.push_back((dw >> 16) & 0xff);
		buffer.push_back((dw >> 24) & 0xff);
	}

	void Encode(WORD w, std::vector<char>& buffer) {
		buffer.push_back((w >> 0) & 0xff);
		buffer.push_back((w >> 8) & 0xff);
	}

	void Encode(DWORD address, WORD data, std::vector<char>& buffer) {
		Encode(address, buffer);
		WORD burstSize = 1;
		buffer.push_back((burstSize >> 8) & 0xff);
		buffer.push_back((burstSize >> 0) & 0xff);
		Encode(data, buffer);
	}

	void Encode(DWORD address, DWORD data, std::vector<char>& buffer) {
		Encode(address, WORD((data >> 0) & 0xffff), buffer);
		Encode(address + 1, WORD((data >> 16) & 0xffff), buffer);
	}

	void SendToWaveCore(DWORD address, WORD data) {
		std::vector<char> buffer;
		Encode(address, data, buffer);
		Write(buffer);
	}

	void SendToWaveCore(DWORD address, const void* blob, WORD bytes) {
		std::vector<char> buffer;

		auto push = [&buffer](auto D) {
			for (int i(0);i < sizeof(D);++i) {
				buffer.push_back((D >> (i * 8)) & 0xff);
			}
		};

		WORD *wdata = (WORD*)blob;
		for (int i(0);i < bytes / 2;i++) {
			push(address + i);
			buffer.push_back(0); buffer.push_back(1);
			push(wdata[i]);
		}

		Write(buffer);

		return;

		// burst transfer

		// disable processing
//		push(STREAMCONTROLLER_DSP_CNTR);
//		push(WORD(0));

		// send burst start address
		push(address);

		// send burst size in words
		WORD burstSize = bytes / sizeof(WORD);
		buffer.push_back(burstSize >> 8);
		buffer.push_back(burstSize & 0xff);
		
		// send dword payload
		assert(bytes % 4 == 0 && "Blob transfer interface requires DWORD format");
		for (int i(0);i < bytes / 4;i++) {
			push(*(((DWORD*)blob) + i));
		}

		// enable processing
//		push(STREAMCONTROLLER_DSP_CNTR);
//		push(WORD(1));

		Write(buffer);
	}

	void SendToWaveCore(DWORD address, float data) {
		union cast {
			float d;
			DWORD w;
		} cast;
		cast.d = data;
		SendToWaveCore(address, WORD((cast.w >> 0) & 0xffff));
		SendToWaveCore(address + 1, WORD((cast.w >> 16) & 0xffff));
	}

	~SerialPort() {
		if (port != INVALID_HANDLE_VALUE) CloseHandle(port);
	}
};
