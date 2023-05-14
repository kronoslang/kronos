#include "PAF.h"
#include "PropertySet.h"
#include "mmfspackle.h"

#include <Propvarutil.h>
#include <wmcodecdsp.h>
#include <aviriff.h>
#include <codecvt>
#include <string>
#include <fstream>
#include <cassert>

#pragma warning(disable: 4250)

namespace {
	std::wstring_convert<std::codecvt_utf8_utf16<TCHAR>> utf8cvt;

	template <typename T> using COM = MF::COM<T>;

	std::string GetWinError(HRESULT hr) { 
		_com_error error(hr);
		std::string tmp(utf8cvt.to_bytes(error.ErrorMessage()));
		return tmp;
	}

	std::vector<std::string> GetInputCodecs() {
		std::vector<std::string> inputCodecs;

		static const int MAX_KEY_LENGTH = 255;
		static const int  MAX_VALUE_NAME = 16383;
		HKEY byteStreams;

		if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\Microsoft\\Windows Media Foundation\\ByteStreamHandlers",
			0, KEY_READ,
			&byteStreams) == ERROR_SUCCESS) {

			WCHAR    achKey[MAX_KEY_LENGTH];   // buffer for subkey name
			DWORD    cbName;                   // size of name string 
			WCHAR    achClass[MAX_PATH] = L"";  // buffer for class name 
			DWORD    cchClassName = MAX_PATH;  // size of class string 
			DWORD    cSubKeys = 0;               // number of subkeys 
			DWORD    cbMaxSubKey;              // longest subkey size 
			DWORD    cchMaxClass;              // longest class string 
			DWORD    cValues;              // number of values for key 
			DWORD    cchMaxValue;          // longest value name 
			DWORD    cbMaxValueData;       // longest value data 
			DWORD    cbSecurityDescriptor; // size of security descriptor 
			FILETIME ftLastWriteTime;      // last write time 

			DWORD i, retCode;

			DWORD cchValue = MAX_VALUE_NAME;

			// Get the class name and the value count. 
			retCode = RegQueryInfoKeyW(
				byteStreams,                    // key handle 
				achClass,                // buffer for class name 
				&cchClassName,           // size of class string 
				NULL,                    // reserved 
				&cSubKeys,               // number of subkeys 
				&cbMaxSubKey,            // longest subkey size 
				&cchMaxClass,            // longest class string 
				&cValues,                // number of values for this key 
				&cchMaxValue,            // longest value name 
				&cbMaxValueData,         // longest value data 
				&cbSecurityDescriptor,   // security descriptor 
				&ftLastWriteTime);       // last write time 

			// Enumerate the subkeys, until RegEnumKeyEx fails.

			if (cSubKeys) {
				for (i = 0; i < cSubKeys; i++) {
					cbName = MAX_KEY_LENGTH;
					retCode = RegEnumKeyExW(byteStreams, i,
						achKey,
						&cbName,
						NULL,
						NULL,
						NULL,
						&ftLastWriteTime);
					if (retCode == ERROR_SUCCESS) {
						if (achKey[0] == '.') {
							std::wstring_convert<std::codecvt_utf8_utf16<TCHAR>> cvt;
							inputCodecs.push_back(cvt.to_bytes(achKey+1));
						}
					}
				}
			}
		}
		RegCloseKey(byteStreams);
		return inputCodecs;
	}

	class MMFReader : public PAF::IAudioFileReader, public PAF::PropertySetImpl {

		using PAF::PropertySetImpl::Get;
		using PAF::PropertySetImpl::Set;
		using PAF::PropertySetImpl::GetMode;

		MF::SourceReader mediaSourceReader;

		int count = 0;
		void Retain() { count++; }
		void Release() {
			if (--count == 0) delete this;
		}

	public:
		MMFReader(const MMFReader&) = delete;
		MMFReader& operator=(const MMFReader&) = delete;

		MMFReader(const wchar_t* path):mediaSourceReader(path) {
			mediaSourceReader.SetStreamSelection(MF::SourceReader::All, false);
			mediaSourceReader.SetStreamSelection(MF::SourceReader::FirstAudio, true);

			auto mInfo = mediaSourceReader.GetNativeMediaType(MF::SourceReader::FirstAudio, 0);

			auto majorType = mInfo.GetMajorType( );

			if (majorType != MFMediaType_Audio)
				throw std::runtime_error("Media source is not an audio source");

			MF::MediaType partialType;
			partialType.SetMajorType(MFMediaType_Audio);
			partialType.SetSubType(MFAudioFormat_PCM);

			int bits = mInfo.GetBitsPerSample( );
			if (bits <= 16) bits = 16;
			else bits = 24;

			partialType.SetBitsPerSample(bits);

			if (!mediaSourceReader.SetCurrentMediaType(MF::SourceReader::FirstAudio, partialType))
				throw std::runtime_error("Couldn't set media type");

			mInfo = mediaSourceReader.GetCurrentMediaType(MF::SourceReader::FirstAudio);
			CreateR(PAF::SampleRate, mInfo.GetSampleRate( ));
			CreateR(PAF::NumChannels, mInfo.GetNumChannels( ));
			CreateR(PAF::BitDepth, mInfo.GetBitsPerSample( ));

			std::int64_t dur;

			if (mediaSourceReader.GetPresentationAttribute(MF::SourceReader::MediaSource, MF_PD_DURATION, dur)) {
				dur *= Get(PAF::SampleRate);
				dur +=  5000000ll;
				dur /= 10000000ll;

				CreateR(PAF::NumSampleFrames, dur);
			}
		}

		~MMFReader() {
			Close( );
		}

		void Close( ) { 
			mediaSourceReader.Close( );
		}

		void StreamWithDelegate(PAF::IAudioFileReaderDelegate& d, int sz) {
			std::vector<float> buf(sz);
			int didread(0);
			while ((didread = operator()(buf.data(), sz))) {
				if (d(buf.data(), didread) != didread) break;
			}
		}

		MF::MediaBuffer readBuffer;
		int consumed;

		int operator()(float *buffer, int maxFrames) {
			int didRead(0);
			while (didRead < maxFrames) {
				if (!readBuffer) {
					readBuffer = mediaSourceReader.ReadBuffer(MF::SourceReader::FirstAudio);
					consumed = 0;
				}

				if (readBuffer) {
					switch (Get(PAF::BitDepth)) {
					case 16: {
							auto buf = readBuffer.GetData<std::int16_t>( );
							int avail = (int)std::min<std::int64_t>(buf.size() - consumed, maxFrames - didRead);
							if (avail < 1) return didRead;
							for (int i = 0; i < avail; ++i) {
								static const float norm = -1.f / (float)std::numeric_limits<int16_t>::min( );
								buffer[i] = ((float)buf[consumed + i]) * norm;
							}
							consumed += avail;
							didRead += avail;
							buffer += avail;
							if (consumed >= buf.size()) readBuffer.Release( );
					}
						break;
					case 24: {
						auto buf = readBuffer.GetData<std::uint8_t>( );
						int avail = (int)std::min<std::int64_t>(buf.size() - consumed, (maxFrames - didRead) * 3);
						if (avail < 1) return didRead;
						assert(avail % 3 == 0);
#if _M_IX86 || _M_AMD64
						for (int i = 0; i < avail; i += 3) {
							static const float norm = 1.f / float(1 << 23);
							std::uint8_t lsb = buf[consumed + i];
							std::int16_t msb = *(std::int16_t*)&buf[consumed + i + 1];
							std::int32_t s24 = ((std::int32_t)msb) << 8;
							s24 |= lsb;
							*buffer++ = s24 * norm;
						}
						consumed += avail;
						didRead += avail / 3;
						if (consumed >= buf.size()) readBuffer.Release( );

#else
#error only little endian code written
#endif
						}
						break;
					default:
						return didRead;
					}

				} else return didRead;
			}
			return didRead;
		}
	};

	MF::SinkWriter CreateSinkWriter(const std::wstring& path, PAF::IPropertySet& ps, GUID subFormat, DWORD& sindex, MF::MediaType& srcTy) {
		int numChannels = (int)ps.Get(PAF::NumChannels);
		int sampleRate = (int)ps.Get(PAF::SampleRate);
		int bitRate = (int)(ps.GetMode(PAF::BitRate) != PAF::NA ? ps.Get(PAF::BitRate) : 0);
		int bitDepth = (int)(ps.GetMode(PAF::BitDepth) != PAF::NA ? ps.Get(PAF::BitDepth) : 0);

		// negotiate source format; 32-bit float, 24-bit int, 16-bit int
		for (int bits = 32; bits >= 16; bits -= 8) {
			MFT_REGISTER_TYPE_INFO inTy, outTy;

			inTy.guidMajorType = outTy.guidMajorType = MFMediaType_Audio;
			inTy.guidSubtype = bits == 32 ? MFAudioFormat_Float : MFAudioFormat_PCM; 
			outTy.guidSubtype = subFormat;

			MF::SinkWriter writer;

			MF::ForEachTransform(MFT_CATEGORY_AUDIO_ENCODER,
				MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_TRANSCODE_ONLY, 
				inTy, outTy, [&](MF::Transform enc) {

				int inputStream(0);
				try {
					inputStream = enc.AddInputStreams(1);
				} catch (...) { }

				COM<IPropertyStore> encoderProperties = enc.GetEncoderProperties();
				if (ps.GetMode(PAF::Lossless) != PAF::NA && ps.Get(PAF::Lossless) != 0 && encoderProperties ) {
					// configure vbr
					PROPVARIANT pv;
					pv.vt = VT_BOOL;
					pv.boolVal = VARIANT_TRUE;
					InitPropVariantFromBoolean(true, &pv);
					encoderProperties->SetValue(MFPKEY_VBRENABLED, pv);
					encoderProperties->SetValue(MFPKEY_CONSTRAIN_ENUMERATED_VBRQUALITY, pv);
					pv.vt = VT_UI4;
					pv.ulVal = 100;
					encoderProperties->SetValue(MFPKEY_DESIRED_VBRQUALITY, pv);
				}

				for (auto outTy : enc.GetOutputAvailableTypes(0)) {
					auto bps = outTy.GetBitsPerSample();
					auto sr = outTy.GetSampleRate();
					auto nch = outTy.GetNumChannels();
					auto brate = outTy.GetBitRate();

					if ((bitDepth == 0 || bps >= bitDepth) &&
						sr == sampleRate &&
						brate >= bitRate / 8 &&
						nch == numChannels) {

						enc.SetOutputType(0, outTy, 0);

						auto inTys = enc.GetInputAvailableTypes(inputStream);
						for (auto i(inTys.rbegin( )); i != inTys.rend( ); ++i) {
							if (i->GetBitsPerSample() == bits &&
								i->GetNumChannels() == numChannels &&
								i->GetSampleRate() == sampleRate) {

								srcTy = *i;

								writer = MF::SinkWriter(path.c_str( ));
								sindex = writer.AddStream(outTy);
								writer.SetInputMediaType(sindex, srcTy);
								return false;
							}
						}
					}
				}
				return true;
			});
			if (writer) return writer;
		}
		return MF::SinkWriter();
	}

	using sinkCtorFuncTy = std::function<MF::SinkWriter(PAF::IPropertySet&, DWORD&, MF::MediaType&)>;

	class PCMConverter : public PAF::IAudioFileWriter {
		std::vector<std::uint8_t> cvtBuf;
	public:
		void StreamWithDelegate(PAF::IAudioFileWriterDelegate& d, int numSamples) {
			std::vector<float> buf(numSamples);
			int didRead;
			while ((didRead = d(buf.data( ), (int)buf.size( ))) != 0) {
				if (operator()(buf.data( ), didRead) != didRead) return;
			}
		}

		virtual int WriteSampleBuffer(const void* srcBytes, int numSamples, int numBytes) = 0;

		int operator()(const float *sampleData, int samples) {
			const int maxOneTime = 65536;
			if (samples > maxOneTime) {
				int half = samples / 2;
				int fst = operator()(sampleData, half);
				if (fst < half) return fst;
				return fst + operator()(sampleData + half, samples - half);
			}

			const float * __restrict buffer = sampleData;
			switch (Get(PAF::BitDepth)) {
#if _M_IX86 || _M_AMD64
			case 16: {
				cvtBuf.resize(samples * 2);
				std::int16_t * __restrict cvt16 = (std::int16_t*)cvtBuf.data( );
				const float ppeak = (float)std::numeric_limits<std::int16_t>::max( );
				const float npeak = (float)std::numeric_limits<std::int16_t>::min( );
				const float norm = -npeak;
				for (int i(0); i < samples; ++i)
					cvt16[i] = (std::int16_t)std::max(npeak, std::min(ppeak, buffer[i] * norm));
				break;
			}

			case 32: {
				return WriteSampleBuffer(buffer, samples, samples * 4);

				cvtBuf.resize(samples * 4);
				std::int32_t * __restrict cvt32 = (std::int32_t*)cvtBuf.data( );
				const double ppeak = (double)std::numeric_limits<std::int32_t>::max( );
				const double npeak = (double)std::numeric_limits<std::int32_t>::min( );
				const double norm = -npeak;
				for (int i(0); i < samples; ++i)
					cvt32[i] = (std::int32_t)std::max(npeak, std::min(ppeak, buffer[i] * norm));
				break;
			}

			case 24: {
				cvtBuf.resize(samples * 3);
				std::uint8_t* __restrict cvt = cvtBuf.data( );
				const double ppeak = (double)std::numeric_limits<std::int32_t>::max( );
				const double npeak = (double)std::numeric_limits<std::int32_t>::min( );
				const double norm = -npeak;
				for (int i(0); i < samples; ++i) {
					std::int32_t tmp = (std::int32_t)
						std::max(npeak, std::min(ppeak, buffer[i] * norm));
					for (int j(0); j < 3; ++j) cvt[i * 3 + j] = *(((const char*)&tmp) + 1 + j);
				}
				break;
			}
#else
#error Only little endian code written
#endif
			default:
				throw std::range_error("Only 8 to 32 bit WAVs supported");
			}
			return WriteSampleBuffer((const char*)cvtBuf.data( ), samples, (int)cvtBuf.size( ));
		}
	};

	class MMFWriter : public PCMConverter, public PAF::PropertySetImpl {
		MF::SinkWriter sink;
		MF::MediaType srcMediaType;
		sinkCtorFuncTy sinkConstructor;
		DWORD streamIndex;

		MF::SinkWriter& GetSinkWriter( ) { 
 			if (!sink) {
				sink = sinkConstructor(
					*this,
					streamIndex,
					srcMediaType);

				try {
					CreateR(PAF::BitDepth, srcMediaType.GetBitsPerSample( ));
				} catch (...) { }

				if (!sink) throw std::invalid_argument("Couldn't create sink");

				Finalize( );
				sink.BeginWriting( );
			}
			return sink;
		}

		int count = 0;
		void Retain( ) { count++; }
		void Release( ) {
			if (--count == 0) delete this;
		}

	public:
		MMFWriter(sinkCtorFuncTy scf) : sinkConstructor(scf) { 
		}

		~MMFWriter( ) {
			Close( );		
		}

		void Close( ) {
			if (sink) {
				sink.Finalize( );
				sink.Release( );
				sinkConstructor = [](PAF::IPropertySet&, DWORD&, MF::MediaType&) -> MF::SinkWriter {
					throw std::runtime_error("Media file was closed already");
				};
			}
		}

		std::int64_t timeStamp = 0ll;

		int operator()(const float* data, int numSamples) {
			GetSinkWriter( );
			return PCMConverter::operator()(data, numSamples);
		}

		int WriteSampleBuffer(const void* srcBytes, int numSamples, int numBytes) {
			auto&& writer(GetSinkWriter( ));
			if (numBytes < 1) return numBytes;

			MF::Sample smp;
			MF::MediaBuffer buf;

			const std::int64_t hns = 1000ll * 1000ll * 10ll;
			std::int64_t frames = numSamples / Get(PAF::NumChannels);
			std::int64_t dur = (frames * hns) / Get(PAF::SampleRate);

			smp.SetSampleTime((timeStamp * hns) / Get(PAF::SampleRate));

			smp.SetSampleDuration(numSamples * hns / (Get(PAF::SampleRate) * Get(PAF::NumChannels)));
			timeStamp += frames;

			buf = MF::MediaBuffer::CreateInMemory(numBytes);
			buf.SetCurrentLength(numBytes);
			smp.AddBuffer(buf);

			{
				auto mem = buf.GetData<char>( );
				assert(mem.maxLen == numBytes);
				memcpy(mem.ptr, srcBytes, numBytes);
			}

			writer.WriteSample(streamIndex, smp);
			return numSamples;
		}
	};

	class WaveWriter : public PCMConverter, public PAF::PropertySetImpl {
		int count = 0;
		void Retain( ) { count++; }
		void Release( ) {
			if (--count == 0) delete this;
		}

		struct {
			RIFFCHUNK       FileHeader;
			DWORD           fccWaveType;    // must be 'WAVE'
			RIFFCHUNK       WaveHeader;
			WAVEFORMATEXTENSIBLE    WaveFormat;
			RIFFCHUNK       DataHeader;
		} header;

		std::ofstream riff;
		std::wstring url;
	public:
		WaveWriter(const std::wstring& url) :url(url) { }
		
		void Open( ) { 
			if (riff.is_open( ) == false) {
				riff.open(url.c_str(), std::ios::binary);
				if (riff.is_open() == false) {
					throw std::invalid_argument("Can't open URL for writing");
				}
				riff.write((const char*)&header, sizeof(header));
			}
		}

		~WaveWriter( ) {
			Close( );
		}

		int WriteSampleBuffer(const void* bytes, int numSmp, int numBytes) {
			int smpSz = numBytes / numSmp;
			Open( );
			return (int)riff.rdbuf( )->sputn((const char*)bytes, numBytes) / smpSz;
		}

		void Close( ) {
			if (riff.is_open( )) {
				std::streamoff filesize = riff.tellp( );
				riff.seekp(0);
				header.FileHeader.fcc = MAKEFOURCC('R', 'I', 'F', 'F');
				header.FileHeader.cb = (DWORD)filesize - 8;
				header.fccWaveType = MAKEFOURCC('W', 'A', 'V', 'E');
				header.WaveHeader.fcc = MAKEFOURCC('f', 'm', 't', ' ');;
				header.WaveHeader.cb = RIFFROUND(sizeof(WAVEFORMATEXTENSIBLE));
				header.WaveFormat.Format.cbSize = 22;
				header.WaveFormat.Format.nAvgBytesPerSec = DWORD(Get(PAF::SampleRate) * Get(PAF::NumChannels) * Get(PAF::BitDepth) / 8);
				header.WaveFormat.Format.nBlockAlign = WORD(Get(PAF::NumChannels) * Get(PAF::BitDepth) / 8);
				header.WaveFormat.Format.nChannels = WORD(Get(PAF::NumChannels));
				header.WaveFormat.Format.nSamplesPerSec = DWORD(Get(PAF::SampleRate));
				header.WaveFormat.Format.wBitsPerSample = WORD((Get(PAF::BitDepth) * 8 + 7) / 8);
				header.WaveFormat.Format.wFormatTag = WAVE_FORMAT_PCM;
				header.WaveFormat.Samples.wValidBitsPerSample = (WORD)Get(PAF::BitDepth);
				header.WaveFormat.dwChannelMask = 0;
				header.WaveFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
				header.DataHeader.fcc = MAKEFOURCC('d', 'a', 't', 'a');
				header.DataHeader.cb = DWORD(filesize - sizeof(header));
				if (riff.rdbuf()->sputn((const char*)&header, sizeof(header)) != sizeof(header)) throw std::runtime_error("Failed to finalize WAVE header: file is garbage");
				riff.close( );
			}
		}
	};
}

namespace PAF {
	IShared::~IShared( ) { }

	IAudioFileReader* IAudioFileReader::Construct(const char *path) {
		try {
			std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
			return new MMFReader(cvt.from_bytes(path).c_str());
		} catch (std::exception& ) {
			return nullptr;
		}
	}

	std::wstring GetExt(std::wstring path) {
		std::locale loc;
		for (auto& c : path) c = std::tolower(c, loc);
		auto pos = path.find_last_of('.');
		if (pos != path.npos) return path.substr(pos);
		else return L"";
	}

	IAudioFileWriter* IAudioFileWriter::Construct(const char *path) {
		std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> cvt;
		std::wstring url = cvt.from_bytes(path);
		auto ext = GetExt(url);

		using namespace std::placeholders;
		if (ext == L".wma") {
			auto writer = new MMFWriter(std::bind(CreateSinkWriter, url, _1, MFAudioFormat_WMAudioV9, _2, _3));
			writer->CreateRW(BitRate, 128000);
			writer->CreateRW(SampleRate, 44100);
			writer->CreateRW(NumChannels, 2);
			writer->CreateRW(Lossless, 0);
			return writer;
		} else if (ext == L".mp3") {
			auto writer = new MMFWriter(std::bind(CreateSinkWriter, url, _1, MFAudioFormat_MP3, _2, _3));
			writer->CreateRW(BitRate, 128000);
			writer->CreateRW(SampleRate, 44100);
			writer->CreateRW(NumChannels, 2);
			return writer;
		} else if (ext == L".aac") {
			auto writer = new MMFWriter(std::bind(CreateSinkWriter, url, _1, MFAudioFormat_AAC, _2, _3));
			writer->CreateRW(BitRate, 128000);
			writer->CreateRW(SampleRate, 44100);
			writer->CreateRW(NumChannels, 2);
			return writer;
		} else if (ext == L".wav") {
			auto writer = new WaveWriter(url);
			writer->CreateRW(SampleRate, 44100);
			writer->CreateRW(NumChannels, 2);
			writer->CreateRW(BitDepth, 16);
			return writer;
		} else {
			return nullptr;
		}
	}

	std::vector<std::string> GetReadableFormats() {
		return GetInputCodecs();
	}

	std::vector<std::string> GetWritableFormats() {
		return{ "wav","wma","aac","mp3" };
	}
}
