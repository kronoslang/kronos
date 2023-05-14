#pragma once

#include <unordered_map>
#include <istream>
#include <vector>

namespace Sxx {
	namespace http {
		struct Request {
			std::string Method;
			std::string Uri;
			std::unordered_map<std::string, std::string> Headers;
			std::vector<char> Body;
			std::string Peer;
		};

		struct Response {
			enum Code {
				Continue = 100,
				SwitchingProtocols = 101,
				Processing = 102,
				OK = 200,
				Created = 201,
				Accepted = 202,
				NonAuthoritativeInformation = 203,
				NoContent = 204,
				ResetContent = 205,
				PartialContent = 206,
				MultiStatus = 207,
				AlreadyReported = 208,
				IMUsed = 226,
				MultipleChoices = 300,
				Moved = 301,
				Found = 302,
				SeeOther = 303,
				NotModified = 304,
				UseProxy = 305,
				SwitchProxy = 306,
				TemporaryRedirect = 307,
				PermanentRedirect = 308,
				BadRequest = 400,
				Unauthorized = 401,
				PaymentRequired = 402,
				Forbidden = 403,
				NotFound = 404,
				MethodNotAllowed = 405,
				NotAcceptable = 406,
				ProxyAuthenticationRequired = 407,
				RequestTimeout = 408,
				Conflict = 409,
				Gone = 410,
				LengthRequired = 411,
				PreconditionFailed = 412,
				PayloadTooLArge = 413,
				UriTooLong = 414,
				UnsupportedMediaType = 415,
				RangeNotSatisfiable = 416,
				ExpectationFailed = 417,
				Teapot = 418,
				MisdirectedRequest = 421,
				UnprocessableEntity = 422,
				Locked = 423,
				FailedDependancy = 424,
				UpgradeRequired = 426,
				PreconditionRequired = 428,
				TooManyRequests = 429,
				RequestHEaderFieldsTooLarge = 431,
				UnavailableForLegalReasons = 451,
				InternalError = 500,
				NotImplemented = 501,
				BadGateway = 502,
				ServiceUnavailable = 503,
				GatewayTimeout = 504,
				HTTPVersionNotSupported = 505,
				VariantAlsoNegotiates = 506,
				InsufficientStorage = 507,
				LoopDetected = 508,
				NotExtended = 510,
				NetworkAuthenticationRequired = 511
			} ResultCode = NotFound;
			std::string Mime;
			std::unordered_map<std::string, std::string> Headers;
			std::vector<char> Body;
			const char *ReasonPhrase() const {
				if (ResultCode < 100) return nullptr;
				if (ResultCode < 300) {
					return ResultCode < 200 ? "Informational" : "Success";
				} else {
					if (ResultCode < 400) return "Redirection";
					if (ResultCode < 500) return "Error";
					return "Server error";
				}
			}
		};

		Request Parse(std::istream& data);

		std::string UrlDecode(std::string url);
		std::string UrlEncode(std::string txt);
	}
}

namespace std {
	std::ostream& operator << (std::ostream&, const Sxx::http::Request&);
	std::ostream& operator << (std::ostream&, const Sxx::http::Response&);
}
