#pragma once

#include <utility>
#include <functional>
#include <unordered_map>

namespace K3 {
	namespace Transform {

		template <typename SRC, typename DST> class IMemoized {
		public:
			virtual bool GetMemoized(const SRC& s, DST& d) = 0;
			virtual void SetMemoized(const SRC& s, const DST& d) = 0;
			virtual void Invalidate() = 0;
		};
		
		template<std::size_t I = 0, typename FuncT, typename... Tp>
		static typename std::enable_if<I == sizeof...(Tp), void>::type
			for_each(const std::tuple<Tp...> &, FuncT) // Unused arguments are given no names.
		{}

		template<std::size_t I = 0, typename FuncT, typename... Tp>
		static typename std::enable_if<I < sizeof...(Tp), void>::type
			for_each(const std::tuple<Tp...>& t, FuncT f) {
			f(std::get<I>(t));
			for_each<I + 1, FuncT, Tp...>(t, f);
		}

		template <typename SRC, typename DST> class Memoized : public virtual IMemoized<SRC,DST> {
			struct Hasher {
				virtual size_t operator()(const SRC& s) const {
					size_t h(0);
					for_each(s, [&h](auto n) mutable {
						return h ^= n->GetHash(true);
					});
					return h;
				}
			};

			std::unordered_map<SRC, DST, Hasher> memo;
		public:
			virtual bool GetMemoized(const SRC& s, DST& d) {
				auto f = memo.find(s);
				if (f == memo.end()) return false;
				d = f->second;
				return true;
			}

			virtual void SetMemoized(const SRC& s, const DST& d) {
#ifndef NDEBUG
				auto f = memo.find(s);
				if (f != memo.end()) {
					assert(f->first == s && f->second == d);
				}
#endif
				memo[s] = d;
			}
			void Invalidate() {
				memo.clear();
			}
		};
	}
}
