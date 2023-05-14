#pragma once

#include <type_traits>
#include <cassert>
#include <algorithm>

namespace lithe {
	namespace detail {
		struct type_data {
			void(*copy)(void *dst, const void *src);
			void(*move)(void *dst, const void *src);
			void(*destroy)(void *ptr);

			template <typename T> static type_data make() {
				type_data data;
				data.copy = [](void *dst, const void *src) {
					new (dst) T(*(const T*)src);
				};

				data.move = [](void *dst, const void *src) {
					new (dst) T(std::move(*(T*)src));
				};

				data.destroy = [](void *ptr) {
					((T*)ptr)->~T();
				};

				return data;
			}

			template <typename T> static const type_data* select() {
				static type_data td = make<T>();
				return &td;
			}

		};
	}

	template <typename... Ts> struct sum_type {
		static constexpr size_t maximum(size_t v) { return v; }

		typename std::aligned_union<0, Ts...>::type data;
		const detail::type_data* helper;

		template <bool...> struct check {};

		template <typename T> void select() {
			helper = detail::type_data::select<T>();
		}

		void destroy() {
			helper->destroy(&data);
			helper = nullptr;
		}

		template <typename T> T& as() & {
			return *(T*)&data;
		}

		template <typename T> T&& as() && {
			return std::move(*(T*)&data);
		}

		template <typename T> const T& as() const& {
			return *(T*)&data;
		}

		static constexpr bool valid = true;

		const detail::type_data* id() const {
			return helper;
		}

		~sum_type() {
			destroy();
		}

		template <typename T> using illegal = std::is_same<
				check<false, std::is_same<T, Ts>::value...>,
				check<std::is_same<T, Ts>::value..., false>>;

		template <typename T> using checked =
			typename std::enable_if<!illegal<T>::value, T>::type;

		template <typename T> using disjoint =
			typename std::enable_if<illegal<T>::value, T>::type;

		template <typename T> sum_type(T v, checked<T>* ptr = nullptr) {
			select<T>();
			helper->move(&data, &v);
		}

		sum_type(const sum_type& src) {
			src.helper->copy(&data, &src.data);
			helper = src.helper;
		}

		sum_type(sum_type&& src) {
			src.helper->move(&data, &src.data);
			helper = src.helper;
		}

		template <typename... Tb> sum_type(const sum_type<Tb...>& src, const sum_type<checked<Tb>...>* = nullptr) {
			src.helper->copy(&data, &src.data);
			helper = src.helper;
		}

		sum_type& operator=(const sum_type& src) {
			destroy();
			src.helper->copy(&data, &src.data);
			helper = src.helper;
			return *this;
		}

		sum_type& operator=(sum_type&& src) {
			destroy();
			src.helper->move(&data, &src.data);
			helper = src.helper;
			return *this;
		}

		template <typename... Tb> typename std::enable_if<sum_type<checked<Tb>...>::valid, sum_type&>::type operator=(const sum_type<Tb...>& src) {
			destroy();
			src.helper->copy(&data, &src.data);
			helper = src.helper;
			return *this;
		}

		template <typename T>
		sum_type& operator=(checked<T> v) {
			destroy();
			select<T>();
			helper->copy(&data, &v);
			return *this;
		}

		template <typename... TSet> struct dispatch_helper;

		template <typename T1, typename... TSet> struct dispatch_helper<T1, TSet...> {
			template <typename TFn, typename TSum> auto operator()(const TFn& fn, TSum& sum) const {
				if (sum.helper == detail::type_data::select<T1>()) {
					return fn(sum.as<T1>());
				} else {
					return dispatch_helper<TSet...>()(fn, sum);
				}
			}

			template <typename TFn, typename TSum> auto operator()(const TFn& fn, TSum&& sum) const {
				if (sum.helper == detail::type_data::select<T1>()) {
					return fn(std::move(sum).as<T1>());
				} else {
					return dispatch_helper<TSet...>()(fn, std::move(sum));
				}
			}

		};

		template <> struct dispatch_helper<> {
			template <typename TFn, typename TSum, typename... TSumOpts> auto operator()(const TFn& fn, const sum_type<TSum, TSumOpts...>& st) { 
				assert(0 && "broken dispatch");
				return fn(st.as<TSum>());
			}

			template <typename TFn, typename TSum, typename... TSumOpts> auto operator()(const TFn& fn, sum_type<TSum, TSumOpts...>&& st) {
				assert(0 && "broken dispatch");
				return fn(std::move(st.as<TSum>()));
			}
		};

		template <typename TFn> auto dispatch(const TFn& fn) const& {
			return dispatch_helper<Ts...>()(fn, *this);
		}

		template <typename TFn> auto dispatch(const TFn& fn) && {
			return dispatch_helper<Ts...>()(fn, std::move(*this));
		}

		template <typename T> bool is(const checked<T>* _type_check = nullptr) const {
			return helper == detail::type_data::select<T>();
		}

		template <typename T> T&& get(const checked<T>* _type_check = nullptr) && {
			assert(is<T>());
			return std::move(as<T>());
		}

		template <typename T> T& get(const checked<T>* _type_check = nullptr) & {
			assert(is<T>());
			return as<T>();
		}

		template <typename T> const T& get(const checked<T>* _type_check = nullptr) const& {
			assert(is<T>());
			return as<T>();
		}
	};

	namespace detail {
		template <typename... TArgs> struct distinct_sum_type;

		template <> struct distinct_sum_type<> {
			using type = sum_type<>;
		};

		template <typename T1, typename... Ts>
		struct distinct_sum_type<T1, Ts...> {
			using check = typename sum_type<Ts...>::template illegal<T1>;
			using type = typename std::conditional<
				check::value,
				sum_type<T1, Ts...>,
				sum_type<Ts...>>::type;
		};


		template <typename... TOptArgs, typename... Ts>
		struct distinct_sum_type<sum_type<TOptArgs...>, Ts...> {
			using type = typename distinct_sum_type<TOptArgs..., Ts...>::type;
		};

		template <typename... TArgs> struct flat_sum_type;

		template <typename Ta, typename Tb> struct sum_choose {
			typename sum_type<Ta, Tb> operator()(bool w, Ta a, Tb b) const {
				if (w) return std::move(a);
				else return std::move(b);
			}
		};

		template <typename T> struct sum_choose<T, T> {
			T operator()(bool w, T a, T b) const {
				return w ? std::move(a) : std::move(b);
			}
		};

		template <typename Ta, typename... TBOpts> struct sum_choose<Ta, sum_type<TBOpts...>> {
			using result_t = typename distinct_sum_type<Ta, TBOpts...>::type;
			result_t operator()(bool w, Ta a, sum_type<TBOpts...> b) const {
				if (w) return std::move(a);
				else return std::move(b);
			}
		};

		template <typename... TAOpts, typename Tb> struct sum_choose<sum_type<TAOpts...>, Tb> {
			using result_t = typename distinct_sum_type<TAOpts..., Tb>::type;
			result_t operator()(bool w, sum_type<TAOpts...> a, Tb b) const {
				if (w) return std::move(a);
				else return std::move(b);
			}
		};

		template <typename... TAOpts, typename... TBOpts> struct sum_choose<sum_type<TAOpts...>, sum_type<TBOpts...>> {
			using result_t = typename distinct_sum_type<TAOpts..., TBOpts...>::type;
			result_t operator()(bool w, sum_type<TAOpts...> a, sum_type<TBOpts...> b) const {
				if (w) return std::move(a);
				else return std::move(b);
			}
		};
	}

	template <typename Ta, typename Tb> static auto choose(bool which, Ta a, Tb b) {
		using Tsa = typename std::remove_reference<Ta>::type;
		using Tsb = typename std::remove_reference<Tb>::type;
		return detail::sum_choose<Tsa, Tsb>()(which, std::move(a), std::move(b));
	}

/*	template <typename... TSum>
	static std::ostream& operator<<(std::ostream& s, const sum_type<TSum...>& st) {
		st.dispatch([&s](auto reified) {
			s << reified;
		});
		return s;
	}*/
}