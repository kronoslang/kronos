#pragma once
#include "kronos_abi.h"
#include <memory>
#include "Errors.h"

#define LET_ERR(sym, expr) auto sym ## _opt = expr; if (sym ## _opt.err) return std::move(sym ##_opt.err); auto sym = *sym ## _opt;   
#define CHECK_ERR(expr) { auto ___check = expr; if (___check.err) return std::move(___check.err); }

namespace K3 {
	template <typename TValue> struct Err {
		std::unique_ptr<TValue> value;
		std::unique_ptr<Kronos::IError> err;

		Err(Err&& e) :value(std::move(e.value)), err(std::move(e.err)) {
		}

		Err(const Err& e) {
			if (e.value) value.reset(new TValue(*e.value));
			if (e.err) err.reset(e.err->Clone());
		}

		Err(TValue&& v) :value(new TValue(std::move(v))) {
		}

		Err(const TValue& v) :value(std::make_unique<TValue>(v)) {
		}

		Err(std::unique_ptr<Kronos::IError>&&  ep) :err(std::move(ep)) {
		}

		Err(const Kronos::IError& err) :err(err.Clone()) {
		}

		const TValue& operator*() const { return *value; }

		Err& operator=(Err e) {
			std::swap(value, e.value);
			std::swap(err, e.err);
		}

		~Err() {
		}
	};

	template <> struct Err<void> {
		std::unique_ptr<Kronos::IError> err;

		Err(std::unique_ptr<Kronos::IError>&&  ep) :err(std::move(ep)) {
		}

		Err(const Kronos::IError& err) :err(err.Clone()) {
		}

		Err() {}

		void operator*() const {}
	};

}