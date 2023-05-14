#pragma once
#include <unordered_map>
#include <memory>
#include <istream>
#include <ostream>
#include <functional>

#include "math.h" // ::isnan, ::isinf used by picojson
#include "picojson.h"

namespace JsonRPC {

	picojson::value Get(std::istream& s);
	void Put(std::ostream&, const picojson::value& rpc);

	class IEndpoint {
	public:
		using Ref = std::shared_ptr<IEndpoint>;
		virtual picojson::value operator()(const picojson::value&) const = 0;
		virtual bool GetPendingMessage(picojson::value&) { return false; }
        virtual ~IEndpoint() { };
	};

	using Method = std::function<picojson::value(const picojson::value&)>;
	struct Endpoint : public std::unordered_map<std::string, Method>, public IEndpoint {
		picojson::value operator()(const picojson::value& rpc) const;
	};
}
