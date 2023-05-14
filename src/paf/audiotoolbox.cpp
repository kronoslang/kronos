// OSX Audio Toolbox

#include "PAF.h"
#include "PropertySet.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <string>
#include <stdexcept>
#include <vector>
#include <unordered_map>
#include <sched.h>
#include <iostream>


#define CHECK(x) { OSStatus err = (x); if (err != noErr) { std::cerr << #x " failed: " << err; throw std::range_error(#x " failed"); } }

namespace {
    class URL {
        CFURLRef url = nullptr;
    public:
        URL(const std::string& str) {
            url = CFURLCreateWithBytes(kCFAllocatorDefault, (const UInt8*)str.c_str(), str.size(), kCFStringEncodingUTF8, nullptr);
        }
        ~URL() { CFRelease(url); }
        operator CFURLRef() const { return url; }
    };
    
    
    class ATReader : public PAF::PropertySetImpl, public PAF::IAudioFileReader {
        std::string url;
        ExtAudioFileRef audioFile;
        AudioStreamBasicDescription asbd;
    public:
        ATReader(const char *url):url(url), audioFile(nullptr) { Open(); }
        
        void Open() {
            if (audioFile == nullptr) {
                URL tmpUrl(url);
                
                ExtAudioFileOpenURL(tmpUrl, &audioFile);
                
                if (audioFile == nullptr) throw std::invalid_argument("Couldn't open URL");
                
                UInt32 asbdSize(sizeof(asbd));
                CHECK(ExtAudioFileGetProperty(audioFile, kExtAudioFileProperty_FileDataFormat, &asbdSize, &asbd));
                
                if (asbd.mBitsPerChannel)
                    CreateR(PAF::BitDepth, asbd.mBitsPerChannel);
                CreateR(PAF::NumChannels, asbd.mChannelsPerFrame);
                CreateR(PAF::SampleRate, asbd.mSampleRate);
                
                SInt64 numFrames; UInt32 nfSize(sizeof(numFrames));
                CHECK(ExtAudioFileGetProperty(audioFile, kExtAudioFileProperty_FileLengthFrames, &nfSize, &numFrames));
                
                CreateR(PAF::NumSampleFrames, numFrames);
                
                if (asbd.mFormatID == kAudioFormatLinearPCM ||
                    asbd.mFormatID == kAudioFormatAppleLossless) CreateR(PAF::Lossless, 1);
                
                asbd.mFormatID = kAudioFormatLinearPCM;
                asbd.mBitsPerChannel = 32;
                asbd.mBytesPerFrame = asbd.mChannelsPerFrame * 4;
                asbd.mFramesPerPacket = 1;
                asbd.mBytesPerPacket = asbd.mBytesPerFrame;
                asbd.mFormatFlags = kAudioFormatFlagIsFloat;
                
                CHECK(ExtAudioFileSetProperty(audioFile, kExtAudioFileProperty_ClientDataFormat, sizeof(asbd), &asbd));
            }
        }
        
        void Close() {
            if (audioFile != nullptr) {
                ExtAudioFileDispose(audioFile);
                audioFile = nullptr;
            }
        }
        
        ~ATReader() {
            Close();
        }
        
        void StreamWithDelegate(PAF::IAudioFileReaderDelegate& d, int sz) {
            std::vector<float> buf(sz);
            int didread(0);
            while ((didread = operator()(buf.data(), sz))) {
                if (d(buf.data(), didread) == false) break;
            }
        }
        
        int operator()(float *buffer, int samples) {
            Open();
            
            AudioBufferList bufList;
            auto numCh = Get(PAF::NumChannels);
            bufList.mNumberBuffers = 1;
            bufList.mBuffers[0].mNumberChannels = (UInt32)numCh;
            bufList.mBuffers[0].mData = buffer;
            bufList.mBuffers[0].mDataByteSize = samples * sizeof(float);
            assert(samples % numCh == 0);
            UInt32 numFrames = samples / numCh;
            CHECK(ExtAudioFileRead(audioFile, &numFrames, &bufList));
            return numFrames * numCh;
        }

    private:
        int count = 0;
        void Retain() { count++; }
        void Release() {
            if (--count == 0) delete this;
        }
    };
    
    struct FileTypeProps {
        AudioFileTypeID id;
        std::vector<AudioStreamBasicDescription> writable;
    };
    using FileTypeMap = std::unordered_map<std::string, FileTypeProps>;
    
    template <typename TYPE, typename SPECIFIER> UInt32 GetGlobal(AudioFilePropertyID id, const SPECIFIER& spec, TYPE& io) {
        UInt32 sz = sizeof(TYPE);
        UInt32 specSz = sizeof(SPECIFIER);
        CHECK(AudioFileGetGlobalInfo(id, specSz, (void*)&spec, &sz, (void*)&io));
        return sz;
    }
    
    template <typename ELEM> void GetGlobal(AudioFilePropertyID id, std::vector<ELEM>& buf) {
        UInt32 sz = (UInt32)(buf.size() * sizeof(ELEM));
        CHECK(AudioFileGetGlobalInfo(id, 0, nullptr, &sz, buf.data()));
    }
    
    template <typename ELEM, typename SPEC> UInt32 GetGlobal(AudioFilePropertyID id, const SPEC& spec, std::vector<ELEM>& buf) {
        UInt32 sz = (UInt32)(buf.size() * sizeof(ELEM));
        CHECK(AudioFileGetGlobalInfo(id, sizeof(SPEC), (void*)&spec, &sz, buf.data()));
        return sz;
    }

    size_t GetGlobalSize(AudioFilePropertyID id) {
        UInt32 size;
        CHECK(AudioFileGetGlobalInfoSize(id, 0, nullptr, &size));
        return size;
    }

    template <typename SPEC> size_t GetGlobalSize(AudioFilePropertyID id, const SPEC& spec) {
        UInt32 size;
        CHECK(AudioFileGetGlobalInfoSize(id, sizeof(SPEC), (void*)&spec, &size));
        return size;
    }
    
    template <typename ELEM, typename SPEC> std::vector<ELEM> GetGlobalVec(AudioFilePropertyID id, const SPEC& spec) {
        std::vector<ELEM> v(GetGlobalSize(id,spec) / sizeof(ELEM));
        GetGlobal(id, spec, v);
        return v;
    }

#define AG(p) kAudioFileGlobalInfo_ ## p

    std::vector<UInt32> GetAudioTypes(bool writable) {
        std::vector<UInt32> types(GetGlobalSize(writable ? AG(WritableTypes) : AG(ReadableTypes)) / sizeof(UInt32));
        GetGlobal(writable ? AG(WritableTypes) : AG(ReadableTypes), types);
        return types;
    }
    
    std::string FourChar(UInt32 cc) {
        std::string fc(' ',4);
        fc[3] = cc & 0xff;
        fc[2] = (cc >> 8) & 0xff;
        fc[1] = (cc >> 16) & 0xff;
        fc[0] = (cc >> 24) & 0xff;
        return fc;
    }
    
    FileTypeMap GetFileTypeInfo(const std::vector<UInt32>& types) {
        FileTypeMap ftm;
        for(auto t : types) {
            CFArrayRef exts = nullptr;
            GetGlobal(AG(ExtensionsForType), t, exts);
            auto num = CFArrayGetCount(exts);
            for(auto i=0;i<num;++i) {
                auto ext = CFStringGetCStringPtr((CFStringRef)CFArrayGetValueAtIndex(exts, i), kCFStringEncodingUTF8);
                
                auto fmts = GetGlobalVec<AudioFormatID>(AG(AvailableFormatIDs), t);
                for(auto id : fmts) {
                    FileTypeProps& props(ftm[ext]);
                    props.id = t;
                    AudioFileTypeAndFormatID fileAndType;
                    fileAndType.mFileType = t;
                    fileAndType.mFormatID = id;
                    auto asbds = GetGlobalVec<AudioStreamBasicDescription>(AG(AvailableStreamDescriptionsForFormat), fileAndType);
                    for(auto &a : asbds) {
                        props.writable.emplace_back(a);
                    }
                }
            }
            
            CFRelease(exts);
        }
        return ftm;
    }
    
    std::vector<std::string> GetFileTypeExts(const std::vector<UInt32>& types) {
        std::vector<std::string> e;
        for(auto t : types) {
            CFArrayRef exts = nullptr;
            GetGlobal(AG(ExtensionsForType), t, exts);
            int num = CFArrayGetCount(exts);
            for(int i=0;i<num;++i) {
                e.push_back(CFStringGetCStringPtr((CFStringRef)CFArrayGetValueAtIndex(exts, i), kCFStringEncodingUTF8));
                
            }
            
            CFRelease(exts);
        }
        std::sort(e.begin(), e.end());
        e.erase(std::unique(e.begin(), e.end()), e.end());
        return e;
    }

    static FileTypeMap& GetWritableFileTypes() {
        static FileTypeMap map = GetFileTypeInfo(GetAudioTypes(true));
        return map;
    }

    static std::vector<std::string>& GetReadableFileTypes() {
        static auto ftexts = GetFileTypeExts(GetAudioTypes(false));
        return ftexts;
    }
    
    class ATWriter : public PAF::IAudioFileWriter, public PAF::PropertySetImpl {
        ExtAudioFileRef audioFile;
        std::string path;
        const FileTypeProps& props;
    public:
        ATWriter(const char *url, const FileTypeProps& afp):path(url), audioFile(nullptr),props(afp) {
            
            for(auto &p : props.writable) {
                if (p.mFormatID == kAudioFormatAppleLossless) CreateRW(PAF::Lossless, 1);
                if (p.mBitsPerChannel) CreateRW(PAF::BitDepth, p.mBitsPerChannel);
                else CreateRW(PAF::BitRate, 192000);
            }
            
            CreateRW(PAF::SampleRate, 44100);
            CreateRW(PAF::NumChannels, 2);
        }
        

        void Open() {
            if (audioFile) return;
            AudioStreamBasicDescription asbd;

            bool good = false;
            if (Has(PAF::BitDepth) && Get(PAF::BitDepth)) {
                for(const auto &w : props.writable) {
                    if (w.mBitsPerChannel == Get(PAF::BitDepth) && w.mFormatID == kAudioFormatLinearPCM) {
                        asbd = w;
                        good = true;
                        break;
                    }
                }
                if (!good) throw std::invalid_argument("Linear PCM encoding is not available");
            } else if (Has(PAF::Lossless) && Get(PAF::Lossless)) {
                for(const auto &w : props.writable) {
                    if (w.mFormatID == kAudioFormatAppleLossless) {
                        asbd = w;
                        good = true;
                        break;
                    }
                }
                if (!good) throw std::invalid_argument("Lossless encoding is not available");
            } else asbd = props.writable.front();
            URL url(path);
#define F(VAR,PROP) asbd.VAR = Has(PAF::PROP) ? Get(PAF::PROP) : 0
            F(mSampleRate,SampleRate);
            F(mChannelsPerFrame,NumChannels);
#undef F
            switch(asbd.mFormatID) {
                    case kAudioFormatLinearPCM:
                    asbd.mBytesPerFrame = asbd.mChannelsPerFrame * asbd.mBitsPerChannel / 8;
                    asbd.mFramesPerPacket = 1;
                    asbd.mBytesPerPacket = asbd.mBytesPerFrame;
                    break;
            }
            
            CHECK(ExtAudioFileCreateWithURL(url, props.id, &asbd, nullptr, kAudioFileFlags_EraseFile, &audioFile));
            
            AudioStreamBasicDescription client;
            client.mFormatID = kAudioFormatLinearPCM;
            client.mChannelsPerFrame = asbd.mChannelsPerFrame;
            client.mBitsPerChannel = 32;
            client.mBytesPerFrame = client.mChannelsPerFrame * 4;
            client.mFramesPerPacket = 1;
            client.mBytesPerPacket = client.mBytesPerFrame;
            client.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
            client.mSampleRate = asbd.mSampleRate;
            client.mReserved = 0;
            
            UInt32 size = sizeof(client);
            CHECK(ExtAudioFileSetProperty(audioFile, kExtAudioFileProperty_ClientDataFormat, size, &client));
           
            if (Has(PAF::BitRate)) {
                AudioConverterRef ac;
                size = sizeof(ac);
                if (ExtAudioFileGetProperty(audioFile, kExtAudioFileProperty_ConverterConfig, &size, &ac) != noErr) {
                    UInt32 bitRate = (UInt32)Get(PAF::BitRate);
                    if (noErr == AudioConverterSetProperty(ac, kAudioConverterEncodeBitRate, sizeof(bitRate), &bitRate)) {
                        ExtAudioFileSetProperty(audioFile, kExtAudioFileProperty_AudioConverter, sizeof(size),&size);
                    }
                }
            }
            // initialize async writing
//            ExtAudioFileWriteAsync(audioFile, 0, nullptr);
        }

        int operator()(const float *buffer, int samples) {
            Open();
            
            auto numCh = Get(PAF::NumChannels);
            
            AudioBufferList bufList;
            bufList.mNumberBuffers = 1;
            bufList.mBuffers[0].mNumberChannels = numCh;
            bufList.mBuffers[0].mData = (void*)buffer;
            bufList.mBuffers[0].mDataByteSize = samples * sizeof(float);
            assert(samples % bufList.mBuffers[0].mNumberChannels == 0);
            while(true) {
                OSStatus async_write = ExtAudioFileWrite(audioFile, samples / numCh, &bufList);
                if (async_write == kExtAudioFileError_AsyncWriteBufferOverflow) {
                    sched_yield();
                } else {
                    CHECK(async_write);
                    break;
                }
            }
            
            return samples;
        }
        
        void Close() {
            if (audioFile) {
                ExtAudioFileDispose(audioFile);
                audioFile = nullptr;
            }
        }
        
        ~ATWriter() {
            Close();
        }
    
        virtual void StreamWithDelegate(PAF::IAudioFileWriterDelegate& d, int sz = 2048) {
            std::vector<float> buf(sz);
            int didread(0);
            while ((didread = d(buf.data(), sz))) {
                if (!operator()(buf.data(), didread)) break;
            }
        }

    private:
        int count = 0;
        void Retain() { count++; }
        void Release() {
            if (--count == 0) delete this;
        }
    };
}

namespace PAF {
    IAudioFileReader* IAudioFileReader::Construct(const char *path) {
        return new ATReader(path);
    }
    
    IShared::~IShared() { }
    
    IAudioFileWriter* IAudioFileWriter::Construct(const char *path) {
        std::string file(path);
        auto extp = file.find_last_of('.');
        if (extp != file.npos) {
            auto ext = file.substr(extp+1);
            for(auto& c:ext) c = tolower(c);
            auto f = GetWritableFileTypes().find(ext);
            if (f != GetWritableFileTypes().end()) {
                return new ATWriter(path, f->second);
            }
        }
        return nullptr;
    }

    std::vector<std::string> GetWritableFormats() {
        std::vector<std::string> exts;
        auto& wf(GetWritableFileTypes());
        for(auto& fp:wf) {
            if (fp.second.writable.size()) exts.push_back(fp.first);
        }
        std::sort(exts.begin(), exts.end());
        exts.erase(std::unique(exts.begin(), exts.end()), exts.end());
        return exts;
    }
    
    std::vector<std::string> GetReadableFormats() {
        return GetReadableFileTypes();
    }
}
