#pragma once

#include "PAF.h"
#include <array>
#include <stdexcept>
#include <cstdint>

namespace PAF {

	class PropertySetImpl : public virtual IPropertySet {
		struct propval_t {
			propval_t(PropertyMode m = NA, std::int64_t val = 0) :mode(m), value(val) { }
			PropertyMode mode;
			int64_t value;
		};

		std::array<propval_t, NumCodecProperties> props;

	public:
		void CreateR(CodecProperty key, std::int64_t val) {
			props[key] = propval_t(Read, val);
		}

		void CreateW(CodecProperty key, std::int64_t val) {
			props[key] = propval_t(Write, val);
		}

		void CreateRW(CodecProperty key, std::int64_t val) {
			props[key] = propval_t(ReadWrite, val);
		}

		void Finalize( ) {
			for (auto&& p : props) p.mode = Read;
		}

		PropertyMode GetMode(CodecProperty key) const {
			if (key < 0 || key >= NumCodecProperties) return NA;
			else return props[key].mode;
		}

		std::int64_t Get(CodecProperty key) const {
			if (key < 0 || key >= NumCodecProperties) throw std::range_error("Property not available");
			else if ((props[key].mode & Read) == Read) return props[key].value;
			else throw std::range_error("Property not readable");
 		}

		void Set(CodecProperty key, std::int64_t value) {
			if (key < 0 || key >= NumCodecProperties) throw std::range_error("Property not available");
			else if ((props[key].mode & Write) == Write) props[key].value = value;
			else throw std::range_error("Property not writable");
		}

		bool TrySet(CodecProperty key, std::int64_t value) {
			if (key < 0 || key >= NumCodecProperties) throw std::range_error("Property not available");
			else if ((props[key].mode & Write) == Write) {
				props[key].value = value;
				return true;
			} else return false;
		}
	};
}