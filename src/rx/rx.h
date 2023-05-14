#include <memory>
#include <functional>
#include <cstdint>
#include <vector>
#include <atomic>
#include <cassert>
#include <mutex>
#include <algorithm>

#include "common/Ref.h"

namespace Rx {
	template <typename T> using Strong = Ref<T>;

	using Timestamp = std::int64_t;

	using ResumeFn = std::function<void()>;

	class ISink;

	template <typename T> using Allocator = std::allocator<T>;
	template <typename T> using Vector = std::vector<T, Allocator<T>>;

	class IObservable;

	struct Subscription {
		int index;

		operator int() const {
			assert(index >= 0); return index;
		}
		operator bool() const {
			return index >= 0;
		}

		void Unsub(ISink*, IObservable*);
	};

	class IObservable : public AtomicRefCounting {
	public:
		virtual size_t SizeOfFrame() const = 0;
		virtual Subscription Subscribe(ISink *subscriber, int slot) = 0;
		virtual void Unsubscribe(ISink* subscriber, Subscription) = 0;
		virtual void Request(unsigned numFrames) { };
		using Ref = Strong<IObservable>;
	};

	class ISink {
	public:
		virtual void Begin(Timestamp) = 0;
		virtual size_t PushChunk(int slot, const void* data, size_t numFrames, Timestamp frameInterval) = 0;
		virtual void End() = 0;
		virtual bool Park(Timestamp until, ResumeFn) = 0;
		virtual void RenderAhead(int slot, Timestamp prerenderSlice) = 0;
	};

	class Observable : public IObservable {
		struct PushTo {
			ISink* sink;
			int slot;
		};
	protected:
		Vector<PushTo> Subscribers;
	public:
		Subscription Subscribe(ISink*, int slot) override;
		void Unsubscribe(ISink* subscriber, Subscription) override;
		void PushVia(const void* data, size_t numFrames, Timestamp interval);
	};

	class Subject : public ISink, public Observable {
	public:
		virtual ~Subject() { }
		void Begin(Timestamp) override {};
		void End() override {};
		
		bool Park(Timestamp ts, ResumeFn fn) override {
			for (auto s : Subscribers) {
				if (s.sink->Park(ts, fn)) return true;
			}
			return false;
		}

		void RenderAhead(int slot, Timestamp prerender) override {
			for (auto s : Subscribers) {
				s.sink->RenderAhead(s.slot, prerender);
			}
		}
	};

	template <typename TSelector>
	class Zip : public Subject {
		Vector<IObservable::Ref> upstream;
		Vector<Subscription> subs;

		struct Buffer {
			Vector<std::uint8_t> data;
			size_t frameSize = 0;
			unsigned numFrames = 0;
			unsigned availRead = 0;
			unsigned write = 0;
		};

		std::recursive_mutex bufferLock;

		Vector<Buffer> buffer;
		Vector<const void*> readPtr;
		TSelector sel;
		
		void DoRequest() {
			for (int i = 0; i < upstream.size(); ++i) {
				upstream[i]->Request(buffer[i].numFrames - buffer[i].availRead);
			}
		}

		void DoZip(Timestamp interval) {
			auto avail = std::numeric_limits<unsigned>::max();
			
			for (int i = 0; i < buffer.size(); ++i) {
				auto readPos = buffer[i].write - buffer[i].availRead;
				if (readPos < 0) readPos += buffer[i].numFrames;

				readPtr[i] = buffer[i].data.data() + readPos * buffer[i].frameSize;

				auto availHere = std::min(
					buffer[i].availRead,
					buffer[i].numFrames - readPos);

				avail = std::min(avail, availHere);
			}

			if (avail) {
				sel(readPtr.data(), (int)readPtr.size(), (int)avail, interval, *this);

				for (auto& b : buffer) {
					b.availRead -= avail;
				}
			}
		}

		size_t outSize;
		unsigned reqLimit = std::numeric_limits<unsigned>::max();
	public:
		Zip(Vector<IObservable::Ref> up, TSelector sel, size_t sizeOfOutFrame, size_t bufferBytes = 0x10000)
			:upstream(std::move(up))
			, sel(std::move(sel))
			, outSize(sizeOfOutFrame) { 
			buffer.resize(upstream.size());
			subs.resize(upstream.size());
			readPtr.resize(upstream.size());

			for (int i = 0; i < upstream.size(); ++i) {
				buffer[i].frameSize = upstream[i]->SizeOfFrame();
				auto sz = std::max(bufferBytes, buffer[i].frameSize);
				buffer[i].data.resize(sz);
				buffer[i].numFrames = (unsigned)(sz / upstream[i]->SizeOfFrame());
				buffer[i].availRead = 0;
				subs[i] = upstream[i]->Subscribe(this, i);
				upstream[i]->Request(buffer[i].numFrames);
			}
		}

		~Zip() {
			for (int i = 0; i < subs.size(); ++i) {
				subs[i].Unsub(this, upstream[i]);
			}
		}

		size_t SizeOfFrame() const override {
			return outSize;
		}

		void Request(unsigned limit) override {
			reqLimit = limit;
		}

		size_t PushChunk(int slot, const void* data, size_t numFrames, Timestamp frameInterval) override {
			std::lock_guard<std::recursive_mutex> lg{ bufferLock };
			auto& buf{ buffer[slot] };
			unsigned todo = std::min((unsigned)numFrames, buf.numFrames - buf.availRead);
			todo = std::min(todo, reqLimit);

			if (todo) {
				const auto sz1 = std::min(buf.numFrames - buf.write, todo);
				assert((buf.write + sz1) * buf.frameSize < buf.data.size());
				memcpy(buf.data.data() + buf.write * buf.frameSize, data, sz1 * buf.frameSize);
				
				const auto sz2 = todo - sz1;
				if (sz2) {
					assert(sz2 * buf.frameSize < buf.data.size());
					memcpy(buf.data.data(), (char*)data + buf.frameSize * sz1, sz2 * buf.frameSize);
				}

				buf.write += todo;
				buf.availRead += todo;

				DoZip(frameInterval);
				DoRequest();
			}
			return todo;
		}
	};

	class Buffer : public Subject {
		Vector<std::uint8_t> partialData;
		int count, skip, partialCount;
	public:
		Buffer(IObservable::Ref source, int count);
		Buffer(IObservable::Ref source, int count, int skip);

		size_t SizeOfFrame() const override;

		size_t PushChunk(int slot, const void* data, size_t frames, Timestamp) override;
	};


}