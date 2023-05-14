#include "PAF.h"
#include "PropertySet.h"
#include <sndfile.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <clocale>
#include <string.h>

namespace PAF {
	class SndFileReader : public IAudioFileReader, public PropertySetImpl {
		SNDFILE *sf;
		SF_INFO info;

		int count = 0;
		void Retain( ) { count++; }
		void Release( ) {
			if (--count == 0) delete this;
		}

	public:

		using PropertySetImpl::Get;
		using PropertySetImpl::Set;
		using PropertySetImpl::GetMode;

		SndFileReader(const char *path) {
			info.format = 0;
			if ((sf = sf_open(path, SFM_READ, &info)) != nullptr) {
				CreateR(PAF::SampleRate, info.samplerate);
				CreateR(PAF::NumChannels, info.channels);
				CreateR(PAF::NumSampleFrames, info.frames);
				if ((info.format & SF_FORMAT_PCM_S8) != 0 ||
					(info.format & SF_FORMAT_PCM_U8) != 0) {
					CreateR(PAF::BitDepth, 8);
				} else if ((info.format & SF_FORMAT_PCM_16) != 0) {
					CreateR(PAF::BitDepth, 16);
				} else if ((info.format & SF_FORMAT_PCM_24) != 0) {
					CreateR(PAF::BitDepth, 24);
				} else if ((info.format & SF_FORMAT_PCM_32) != 0) {
					CreateR(PAF::BitDepth, 32);
				} else if ((info.format & SF_FORMAT_FLOAT) != 0) {
					CreateR(PAF::BitDepth, 32);
				} else if ((info.format & SF_FORMAT_DOUBLE) != 0) {
					CreateR(PAF::BitDepth, 64);
				}
			} else {
				throw std::runtime_error(sf_strerror(sf));
			}
		}

		void Close( ) {
			if (sf) {
				sf_close(sf);
				sf = nullptr;
			}
		}

		~SndFileReader( ) {
			if (sf) sf_close(sf);
		}

		void StreamWithDelegate(PAF::IAudioFileReaderDelegate& d, int sz) {
			std::vector<float> buf(sz);
			int didread(0);
			while ((didread = operator()(buf.data( ), sz))) {
				if (d(buf.data( ), didread) == false) break;
			}
		}

		int operator()(float *buf, int sz) {
			if (sf) {
				return (int)sf_read_float(sf, buf, sz);
			} else return 0;
		}
	};

    IAudioFileReader* IAudioFileReader::Construct(const char *path) {
    	return new SndFileReader(path);
    }
    
    IShared::~IShared() { }
    
	class SndFileWriter : public IAudioFileWriter, public PropertySetImpl {
		SNDFILE *sf = nullptr;
		SF_INFO info;

		int count = 0;
		void Retain( ) { count++; }
		void Release( ) {
			if (--count == 0) delete this;
		}

		std::string url;

	public:

		using PropertySetImpl::Get;
		using PropertySetImpl::Set;
		using PropertySetImpl::GetMode;

		SndFileWriter(const SndFileWriter&) = delete;
		void operator=(SndFileWriter) = delete;

		SndFileWriter(const char *path) :url(path) { 
			memset(&info, 0, sizeof(info));
			auto extpos = url.find_last_of('.');
			if (extpos != std::string::npos) {
				auto ext = url.substr(extpos);
				for (auto&c : ext) c = tolower(c);

				CreateRW(PAF::NumChannels, 1);
				CreateRW(PAF::SampleRate, 44100);

				static const std::unordered_map<std::string, int> sf_formats = {
					{".wav", SF_FORMAT_WAVEX},
					{".bwf", SF_FORMAT_WAVEX},
					{".aif", SF_FORMAT_AIFF},
					{".aiff", SF_FORMAT_AIFF},
					{".raw", SF_FORMAT_RAW},
					{".pcm", SF_FORMAT_RAW},
					{".w64", SF_FORMAT_W64},
					{".sd2", SF_FORMAT_SD2},
					{".flac", SF_FORMAT_FLAC},
					{".caf", SF_FORMAT_CAF},
					{".ogg", SF_FORMAT_OGG | SF_FORMAT_VORBIS },
					{".rf64", SF_FORMAT_RF64}
				};

				auto fmt = sf_formats.find(ext);
				if (fmt == sf_formats.end( )) info.format = SF_FORMAT_RAW;
				else info.format = fmt->second;

				switch (info.format) {
				case SF_FORMAT_OGG:
					CreateRW(PAF::Quality, 70);
					break;
				case SF_FORMAT_FLAC:
					CreateRW(PAF::Quality, 90);
					break;
				default:
					CreateRW(PAF::BitDepth, 16);
				}
				
				CreateRW(PAF::SampleRate, 44100);
			} 
		}

		void Close( ) {
			if (sf) {
				sf_close(sf);
				sf = nullptr;
			}
		}

		~SndFileWriter( ) {
			if (sf) sf_close(sf);
		}

		void StreamWithDelegate(PAF::IAudioFileWriterDelegate& d, int sz) {
			std::vector<float> buf(sz);
			int didread(0);
			while ((didread = operator()(buf.data( ), sz))) {
				if (d(buf.data( ), didread) == false) break;
			}
		}

		int operator()(const float *buf, int sz) {
			if (sf == nullptr) {
				info.format &= SF_FORMAT_TYPEMASK;
				if (Has(PAF::BitDepth)) {
					switch (Get(PAF::BitDepth)) {
					case 64: info.format |= SF_FORMAT_DOUBLE; break;
					case 32: info.format |= SF_FORMAT_FLOAT; break;
					case 24: info.format |= SF_FORMAT_PCM_24; break;
					default: case 16: info.format |= SF_FORMAT_PCM_16; break;
					case 8: info.format |= SF_FORMAT_PCM_U8; break;
					}
				}

				info.channels = Get(PAF::NumChannels);
				info.samplerate = Get(PAF::SampleRate);

				if (sf_format_check(&info) == 0) throw std::runtime_error("Unsupported format");

				sf = sf_open(url.c_str( ), SFM_WRITE, &info);				

				if (sf == nullptr) throw std::runtime_error("Couldn't open sound file");

				if (Has(PAF::Quality)) {
					double vbr = Get(PAF::Quality) * 0.01;
					if (vbr < 0.0) vbr = 0.0;
					if (vbr > 1.0) vbr = 1.0;
					sf_command(sf, SFC_SET_VBR_ENCODING_QUALITY, &vbr, sizeof(double));
				}

				sf_command(sf, SFC_SET_CLIPPING, nullptr, SF_TRUE);
			}

			if (sf) {
				auto result = sf_write_float(sf, buf, sz);
				if (result == 0) {
					int code = sf_error(sf);
					if (code) {
						throw std::runtime_error(sf_error_number(code));
					}
				}
				return result;
			} else return 0;
		}
	};
	
	IAudioFileWriter* IAudioFileWriter::Construct(const char *path) {
		return new SndFileWriter(path);
    }

    // todo
    std::vector<std::string> GetReadableFormats() { return {}; }
    std::vector<std::string> GetWritableFormats() { return {}; }
}
