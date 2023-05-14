#include "CompareTestResultJSON.h"
#include <limits>

namespace {
	template <typename T>
	bool equal(T const& l, T const& r, std::ostream* diags) {
		if (l == r) return true;
		if (diags) {
			*diags << "! " << l << " <> " << l << "\n";
		}
		return false;
	}

	bool equal(double l, double r, std::ostream* diags) {
		auto E = fabs(l - r);
		auto rE = 2.0 * E / (l + r + 8.0 * std::numeric_limits<double>::epsilon());
		double eps = std::numeric_limits<float>::epsilon();
		if (rE < 0.00001) {
			return true;
		}
		if (diags) {
			*diags << "! " << l << " <> " << l << " (" << (fabs(l - r) * 200.0 / (l + r)) << "%, " << E / eps << " epsilon)\n";
		}
		return false;
	}

	bool equal(picojson::array const& l, picojson::array const& r, std::ostream* diags) {
		if (l.size() != r.size()) {
			if (diags) {
				*diags << "! " << l.size() << " <> " << r.size() << " items\n";
			}
			return false;
		}
		for (int i = 0; i < l.size(); ++i) {
			if (!::equal(l[i], r[i], diags)) return false;
		}
		return true;
	}

	bool equal(picojson::object const& l, picojson::object const& r, std::ostream* diags) {
		if (l.size() != r.size()) {
			if (diags) {
				*diags << "! ";
				for (auto& kv : l) {
					if (!r.count(kv.first)) *diags << " <" << kv.first;
				}
				for (auto& kv : r) {
					if (!l.count(kv.first)) *diags << " >" << kv.first;
				}
				*diags << " different keys\n";
			}
			return false;
		}
		for (auto& kv : l) {
			auto rf = r.find(kv.first);
			if (rf == r.end()) {
				if (diags) {
					*diags << "! <" << kv.first << " missing from reference\n"; 
				}
				return false;
			}
			if (!::equal(kv.second, rf->second, diags)) return false;
		}
		return true;
	}
}

bool equal(picojson::value const& l, picojson::value const& r, std::ostream* diags) {
#define CMP(TY) if (l.is<TY>() && r.is<TY>()) return equal(l.get<TY>(), r.get<TY>(), diags);
	CMP(std::string);
	CMP(double);
	CMP(bool);
	CMP(picojson::array);
	CMP(picojson::object);
	if (diags) {
		*diags << "! type difference: " << l << " <> " << r << "\n";
	}
	return false;
}
