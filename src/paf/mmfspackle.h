#pragma once

#include <comdef.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <cassert>

#include <functional>
#include <vector>

namespace MF {
	template <typename T> class COM {
		mutable T* ptr = nullptr;
	public:
		template <typename U> COM(U* s = nullptr):ptr(s) {
			Retain();
		}

		COM() { }

		COM(const COM<T>& src) :ptr(src) {
			Retain();
		}

		COM(COM<T>&& src) noexcept {
			std::swap(ptr, src.ptr);
		}

		~COM() {
			Release();
		}

		operator T* () const { return ptr; }
		operator T* () { return ptr; }

		T** operator & () {
			Release();
			return &ptr;
		}

		COM<T>& operator=(COM<T>&& src) noexcept {
			std::swap(ptr, src.ptr);
			return *this;
		}

		COM<T>& operator=(const COM<T>& src) {
			src.Retain();
			Release();
			ptr = src.ptr;
			return *this;
		}

		void Retain() const {
			if (ptr) ptr->AddRef();
		}

		void Release() const {
			if (ptr) ptr->Release();
			ptr = nullptr;
		}

		T* operator->() {
			return ptr;
		}

		T* operator->() const {
			return ptr;
		}
	};

	class MediaType {
		friend class SourceReader;
		COM<IMFMediaType> p;
	public:
		COM<IMFMediaType> Get( ) { return p; }
		operator bool( ) const { return p != nullptr; }

		MediaType(COM<IMFMediaType> p) :p(p) { }
		MediaType( );
		void SetMajorType(GUID majorMediaType);
		void SetSubType(GUID subType);
		int GetBitsPerSample( ) const;
		int GetNumChannels( ) const;
		int GetSampleRate( ) const;
		int GetBitRate( ) const;
		GUID GetMajorType( ) const;
		void SetBitsPerSample(int bits);
		bool GetItem(GUID property, PROPVARIANT& pv);
	};

	class MediaBuffer {
		COM<IMFMediaBuffer> p;
	public:
		COM<IMFMediaBuffer> Get( ) { return p; }
		operator bool( ) const { return p != nullptr; }
		void Release( ) { p.Release( ); }

		MediaBuffer(COM<IMFMediaBuffer> p) :p(p) { }
		MediaBuffer( ) { }

		static MediaBuffer CreateInMemory(int maxBytes);
		void SetCurrentLength(int bytes);

		template <typename SMP> struct Lock {
			COM<IMFMediaBuffer> buf;
			BYTE *ptr; DWORD maxLen, curLen;
		public:
			Lock(IMFMediaBuffer* buf) :buf(buf) {
				buf->Lock(&ptr, &maxLen, &curLen);
			}

			~Lock( ) {
				buf->Unlock( );
			}

			SMP& operator[](int idx) {
				assert(idx >= 0 && (idx + 1)*sizeof(SMP) <= curLen);
				return ((SMP*)ptr)[idx];
			}

			int size( ) const {
				return curLen / sizeof(SMP);
			}
		};

		template <typename T> Lock<T> GetData( ) {
			return Lock<T>(p);
		}
	};

	class Sample {
		COM<IMFSample> p;
	public:
		COM<IMFSample> Get( ) { return p; }
		Sample(COM<IMFSample> p) : p(p) { }
		Sample( );
		std::int64_t GetSampleDuration( );
		MediaBuffer ToContiguous( );
		void SetSampleTime(std::int64_t hns);
		void SetSampleDuration(std::int64_t hns);
		void AddBuffer(MediaBuffer buf);
	};

	class SourceReader {
		COM<IMFSourceReader> p;
	public:
		enum StreamID {
			All = MF_SOURCE_READER_ALL_STREAMS,
			FirstAudio = MF_SOURCE_READER_FIRST_AUDIO_STREAM,
			MediaSource = MF_SOURCE_READER_MEDIASOURCE
		};

		operator bool( ) const { return p != nullptr; }

		void Close( ) {
			p.Release( );
		}

		SourceReader(const wchar_t* url);
		~SourceReader( );
		void SetStreamSelection(StreamID id, bool selected);
		MediaType GetNativeMediaType(StreamID id, int mediaTypeIndex);
		bool SetCurrentMediaType(StreamID id, MediaType mt);
		MediaType GetCurrentMediaType(StreamID id);
		bool GetPresentationAttribute(StreamID id, const GUID& key, PROPVARIANT& pv);
		bool GetPresentationAttribute(StreamID id, const GUID& key, std::int64_t& val);
		Sample ReadSample(StreamID id);
		MediaBuffer ReadBuffer(StreamID id);
	};

	class SinkWriter {
		COM<IMFSinkWriter> p;
	public:
		operator bool( ) const { return p != nullptr; }

		SinkWriter(const wchar_t* url);
		SinkWriter( ) { }
		~SinkWriter( );

		int AddStream(MediaType mt);
		void SetInputMediaType(int streamId, MediaType srcTy);
		void BeginWriting( );
		void Finalize( );
		void Release( );
		void WriteSample(int streamId, Sample smp);
	};

	class Transform {
		COM<IMFTransform> p;
		void SortMediaTypes(std::vector<MediaType>& mTys);
	public:
		Transform(COM<IMFTransform> p) :p(p) { }
		int AddInputStreams(int num);
		std::vector<MediaType> GetOutputAvailableTypes(int streamId);
		std::vector<MediaType> GetInputAvailableTypes(int streamId);
		COM<IPropertyStore> GetEncoderProperties( );
		void SetOutputType(int stream, MediaType mt, int x);
	};

	void ForEachTransform(const GUID& category,
		unsigned int flags,
		const MFT_REGISTER_TYPE_INFO& inputType,
		const MFT_REGISTER_TYPE_INFO& outputType,
		std::function<bool(Transform)> f);
};