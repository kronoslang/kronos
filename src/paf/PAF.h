#pragma once

#include <utility>
#include <cstdint>
#include <vector>
#include <string>

using std::uint64_t;
using std::uint32_t;
using std::uint8_t;

#define PAF_CODEC_PROPERTIES \
 F(SampleRate) \
 F(BitDepth) \
 F(NumChannels) \
 F(BitRate) \
 F(Quality) \
 F(Lossless) \
 F(NumSampleFrames) \
 F(CanSeek) 

namespace PAF {

	enum CodecProperty {
#define F(prop) prop,
		PAF_CODEC_PROPERTIES
#undef F
		NumCodecProperties
	};

	enum PropertyMode {
		NA = 0,
		Read = 1,
		Write = 2,
		ReadWrite = Read | Write
	};

	class IPropertySet {
	public:
		virtual PropertyMode GetMode(CodecProperty) const = 0;
		virtual std::int64_t Get(CodecProperty) const = 0;
		virtual void Set(CodecProperty, std::int64_t) = 0;
		virtual bool TrySet(CodecProperty, std::int64_t) = 0;
		inline bool Has(CodecProperty key) const { return GetMode(key) != NA; }
	};

	template <typename... ARGS> static void Configure(IPropertySet& ps, CodecProperty key, std::int64_t val, ARGS... as) {
		ps.Set(key, val); Configure(ps, as...);
	}

	class IShared {
	public:
		virtual ~IShared() = 0;
		virtual void Retain() = 0;
		virtual void Release() = 0;
	};

	template <typename T> class Shared {
		T* s;
	public:
		template <typename U> Shared(const Shared<U>& src):s(src.s) { if (s) s->Retain(); }
		template <typename U> Shared(Shared<U>&& src) : s(nullptr) { std::swap(s, src.s); }
		template <typename... ARGS> Shared(ARGS&&... as) : s(T::Construct(std::forward<ARGS>(as)...)) { if (s) s->Retain(); }
		~Shared() { if (s) s->Release(); }
		Shared<T>& operator=(Shared<T> from) { std::swap(s, from.s); return *this; }
		T* operator->() { return s; }
		const T* operator->() const { return s; }
		operator bool() const { return s != nullptr; }
		T& operator*() { return *s; }
		const T& operator*() const { return *s; }
		void Close() { s->Release(); s = nullptr; }
		template <typename... ARGS> auto operator()(ARGS&&... as) -> decltype((*s)(std::forward<ARGS>(as)...)) {
			return (*s)(std::forward<ARGS>(as)...);
		}
		std::int64_t operator[](CodecProperty key) { return s->Get(key); }
	};

	class IAudioFileReaderDelegate {
	public:
		virtual int operator()(const float* buffer, int numFrames) = 0;
	};

	class IAudioFileWriterDelegate {
	public:
		virtual int operator()(float* buffer, int numFrames) = 0;
	};

	class IAudioFileReader : IShared, public IAudioFileWriterDelegate, public virtual IPropertySet {
		friend class Shared<IAudioFileReader>;
		static IAudioFileReader* Construct(const char *URL);
	public:
		virtual  ~IAudioFileReader( ) { };

		inline bool CanRead(CodecProperty key) { return (GetMode(key) & Read) == Read; }
		inline bool CanWrite(CodecProperty key) { return (GetMode(key) & Write) == Write; }

		virtual int operator()(float *buffer, int maxFrames) = 0;

		virtual void StreamWithDelegate(IAudioFileReaderDelegate&, int bufferSize = 2048) = 0;

		virtual uint64_t Seek(uint64_t) { return -1; };

		template <typename DELEGATE> void Stream(DELEGATE&& d) {
			struct wrap : public IAudioFileReaderDelegate {
				DELEGATE d;
                wrap(DELEGATE d):d(std::forward<DELEGATE>(d)) { }
				int operator()(const float *b, int nf) { return d(b, nf); }
            } w(std::forward<DELEGATE>(d));
            StreamWithDelegate(w);
		}

		virtual void Close( ) = 0;
	};

	class IAudioFileWriter : IShared, public IAudioFileReaderDelegate, public virtual IPropertySet {
		friend class Shared<IAudioFileWriter>;
		static IAudioFileWriter* Construct(const char *path);
	public:
		virtual  ~IAudioFileWriter( ) { };

		inline bool CanRead(CodecProperty key) { return (GetMode(key) & Read) == Read; }
		inline bool CanWrite(CodecProperty key) { return (GetMode(key) & Write) == Write; }

		virtual int operator()(const float *buffer, int maxFrames) = 0;
		virtual void StreamWithDelegate(IAudioFileWriterDelegate&, int frames = 2048) = 0;
		virtual void Close( ) = 0;

		template <typename DELEGATE> void Stream(DELEGATE&& d) {
			struct wrap : public IAudioFileWriterDelegate {
				DELEGATE d;
                wrap(DELEGATE d):d(std::forward<DELEGATE>(d)) { }
				int operator()(float *b, int nf) { return d(b, nf); }
            } w(std::forward<DELEGATE>(d));
            StreamWithDelegate(w);
		}
	};

	using AudioFileReader = Shared<IAudioFileReader>;
	using AudioFileWriter = Shared<IAudioFileWriter>;
    std::vector<std::string> GetWritableFormats();
    std::vector<std::string> GetReadableFormats();
}
