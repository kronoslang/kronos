#include "mmfspackle.h"

#include <VersionHelpers.h>
#include <aviriff.h>
#include <codecvt>
#include <vector>
#include <string>
#include <algorithm>
#include <comdef.h>
#include <Propvarutil.h>
#include <memory>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

#define CHECK(EXPR) ThrowOnError((EXPR), #EXPR);
static void ThrowOnError(HRESULT err, const char *what) {
	if (err != S_OK) {
		throw std::runtime_error(what);
	}
}

namespace MF {
	struct Initializer {
		Initializer( ) {
			CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			CHECK(MFStartup(MF_VERSION));
		}

		~Initializer( ) {
			//MFShutdown();
			//CoUninitialize( );
		}
	};

	static void Init( ) {
		static Initializer init;
	}

	std::wstring_convert<std::codecvt_utf8_utf16<TCHAR>> utf8cvt;

	std::string GetWinError(HRESULT hr) {
		_com_error error(hr);
		std::string tmp(utf8cvt.to_bytes(error.ErrorMessage( )));
		return tmp;
	}

	MediaType::MediaType( ) {
		CHECK(MFCreateMediaType(&p));
	}

	void MediaType::SetMajorType(GUID majorMediaType) {
		CHECK(p->SetGUID(MF_MT_MAJOR_TYPE, majorMediaType));
	}

	void MediaType::SetSubType(GUID subType) {
		CHECK(p->SetGUID(MF_MT_SUBTYPE, subType));
	}

	int MediaType::GetBitsPerSample( ) const {
		UINT32 bits(0);
		p->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
		return bits;
	}

	int MediaType::GetNumChannels( ) const {
		UINT32 chans(0);
		p->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &chans);
		return chans;
	}

	int MediaType::GetSampleRate( ) const {
		UINT32 sr(0);
		CHECK(p->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr));
		return sr;
	}

	int MediaType::GetBitRate( ) const {
		UINT32 sr(0);
		p->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, &sr);
		return sr;
	}

	GUID MediaType::GetMajorType( ) const {
		GUID mt;
		CHECK(p->GetMajorType(&mt));
		return mt;
	}

	void MediaType::SetBitsPerSample(int bits) {
		UINT32 b(bits);
		CHECK(p->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, b));
	}

	bool MediaType::GetItem(GUID property, PROPVARIANT& pv) {
		return p->GetItem(property, &pv) == S_OK;
	}

	MediaBuffer MediaBuffer::CreateInMemory( int maxBytes ) {
		COM<IMFMediaBuffer> mb;
		CHECK(MFCreateMemoryBuffer(maxBytes, &mb));
		return MediaBuffer(mb);
	}

	void MediaBuffer::SetCurrentLength(int bytes) {
		CHECK(p->SetCurrentLength(bytes));
	}

	Sample::Sample( ) {
		CHECK(MFCreateSample(&p));
	}

	std::int64_t Sample::GetSampleDuration( ) {
		LONGLONG dur;
		CHECK(p->GetSampleDuration(&dur));
		return dur;
	}

	MediaBuffer Sample::ToContiguous( ) {
		COM<IMFMediaBuffer> mb;
		if (!p) return MediaBuffer( );
		CHECK(p->ConvertToContiguousBuffer(&mb));
		return MediaBuffer(mb);
	}

	void Sample::SetSampleTime(std::int64_t hns) {
		CHECK(p->SetSampleTime(hns));
	}

	void Sample::SetSampleDuration(std::int64_t hns) {
		CHECK(p->SetSampleDuration(hns));
	}

	void Sample::AddBuffer(MediaBuffer buf) {
		p->AddBuffer(buf.Get( ));
	}

	SourceReader::SourceReader(const wchar_t* url) {
			Init( );
			CHECK(MFCreateSourceReaderFromURL(url, nullptr, &p));
		}

	SourceReader::~SourceReader( ) {
		}

	void SourceReader::SetStreamSelection(StreamID id, bool selected) {
			p->SetStreamSelection(id, selected?TRUE:FALSE);
		}

	MediaType SourceReader::GetNativeMediaType(StreamID id, int mediaTypeIndex) {
			COM<IMFMediaType> mInfo;
			CHECK(p->GetNativeMediaType(id, mediaTypeIndex, &mInfo));
			return MediaType(mInfo);
		}

	bool SourceReader::SetCurrentMediaType(StreamID id, MediaType mt) {
			return (p->SetCurrentMediaType(id, nullptr, mt.p)) == S_OK;
		}

	MediaType SourceReader::GetCurrentMediaType(StreamID id) {
			COM<IMFMediaType> mt;
			CHECK(p->GetCurrentMediaType(id, &mt));
			return mt;
		}

	bool SourceReader::GetPresentationAttribute(StreamID id, const GUID& key, PROPVARIANT& pv) {
			return p->GetPresentationAttribute(id, key, &pv) == S_OK;
		}

	bool SourceReader::GetPresentationAttribute(StreamID id, const GUID& key, std::int64_t& val) {
			LONGLONG ll;
			PROPVARIANT pv;
			if (GetPresentationAttribute(id, key, pv) &&
				PropVariantToInt64(pv, &ll) == S_OK) {
				PropVariantClear(&pv);
				val = ll;
				return true;
			}
			return false;
		}

	Sample SourceReader::ReadSample(StreamID id) {
			COM<IMFSample> incoming;
			DWORD flags(0);
			p->ReadSample(id, 0, nullptr, &flags, nullptr, &incoming);
			return Sample(incoming);
		}

	MediaBuffer SourceReader::ReadBuffer(StreamID id) {
			return ReadSample(id).ToContiguous( );
		}

	SinkWriter::SinkWriter(const wchar_t* url) {
			Init( );
			CHECK(MFCreateSinkWriterFromURL(url, nullptr, nullptr, &p));
		}

	SinkWriter::~SinkWriter( ) {
		}

	int SinkWriter::AddStream(MediaType mt) {
			DWORD si(0);
			CHECK(p->AddStream(mt.Get( ), &si));
			return si;
		}

	void SinkWriter::SetInputMediaType(int streamId, MediaType srcTy) {
			CHECK(p->SetInputMediaType(streamId, srcTy.Get( ), nullptr));
		}

	void SinkWriter::BeginWriting( ) {
			CHECK(p->BeginWriting( ));
		}

	void SinkWriter::Finalize( ) {
			CHECK(p->Finalize( ));
		}

	void SinkWriter::Release( ) {
			p.Release( );
		}

	void SinkWriter::WriteSample(int streamId, Sample smp) {
			p->WriteSample(streamId, smp.Get( ));
		}
		
		void Transform::SortMediaTypes(std::vector<MediaType>& mTys) {
			std::sort(mTys.begin( ), mTys.end( ), [](MediaType lhs, MediaType rhs) -> bool {
				if (lhs.GetSampleRate( ) < rhs.GetSampleRate( )) return true;
				if (lhs.GetSampleRate( ) > rhs.GetSampleRate( )) return false;
				if (lhs.GetBitRate( ) < rhs.GetBitRate( )) return true;
				if (lhs.GetBitRate( ) > rhs.GetBitRate( )) return false;
				return false;
			});
		}

		int Transform::AddInputStreams(int num) {
			DWORD inputStream(0);
			CHECK(p->AddInputStreams(num, &inputStream));
			return inputStream;
		}

		std::vector<MediaType> Transform::GetOutputAvailableTypes(int streamId) {
			std::vector<MediaType> mTys;
			DWORD typeIdx(0);
			COM<IMFMediaType> mTy;
			while (p->GetOutputAvailableType(streamId, typeIdx++, &mTy) == S_OK) {
				mTys.push_back(mTy);
				mTy.Release( );
			}
			SortMediaTypes(mTys);
			return mTys;
		}

		std::vector<MediaType> Transform::GetInputAvailableTypes(int streamId) {
			std::vector<MediaType> mTys;
			DWORD typeIdx(0);
			COM<IMFMediaType> mTy;
			while (p->GetInputAvailableType(streamId, typeIdx++, &mTy) == S_OK) {
				mTys.push_back(mTy);
				mTy.Release( );
			}
			SortMediaTypes(mTys);
			return mTys;
		}

		COM<IPropertyStore> Transform::GetEncoderProperties( ) {
			COM<IPropertyStore> props;
			p->QueryInterface(&props);
			return props;
		}

		void Transform::SetOutputType(int stream, MediaType mt, int x) {
			CHECK(p->SetOutputType(stream, mt.Get( ), x));
		}

	void ForEachTransform(const GUID& category, 
		unsigned int flags, 
		const MFT_REGISTER_TYPE_INFO& inputType, 
		const MFT_REGISTER_TYPE_INFO& outputType,
		std::function<bool(Transform)> f) {

		Init( );

		UINT32 numActivations(32);
		IMFActivate** activations(nullptr);

		CHECK(MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
			MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER | MFT_ENUM_FLAG_TRANSCODE_ONLY,
			&inputType,
			&outputType,
			&activations,
			&numActivations));

		auto deleter = [numActivations](IMFActivate ** ptr) {
			for (UINT32 i(0); i < numActivations; ++i) ptr[i]->Release( );
			CoTaskMemFree(ptr);
		};

		std::unique_ptr<IMFActivate*, decltype(deleter)> activationHolder(activations, deleter);

		for (unsigned i = 0; i < numActivations; ++i) {
			COM<IMFActivate> act(activations[i]);
			COM<IMFTransform> enc;
			act->ActivateObject(IID_PPV_ARGS(&enc));
			if (f(Transform(enc)) == false) break;
		}
	}
}