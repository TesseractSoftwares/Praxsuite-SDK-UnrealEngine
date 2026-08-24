#include "Core/PraxError.h"

namespace Prax
{
	namespace
	{
		/** Collects the details array, which the gateway sends as strings and sometimes omits. */
		std::vector<std::string> ReadDetails(const FJsonValue& In)
		{
			std::vector<std::string> Out;
			for (const FJsonValue& Item : In.AsArray())
			{
				if (Item.IsString())
				{
					Out.push_back(Item.AsString());
				}
				else if (!Item.IsNull())
				{
					// A non-string detail is unexpected but throwing it away would discard the only
					// explanation of a rejection. Serialise it instead.
					Out.push_back(Item.ToString());
				}
			}
			return Out;
		}
	}

	EPraxErrorCode FPraxError::CodeFromString(const std::string& In, int Status)
	{
		if (In == "RATE_LIMIT_EXCEEDED")   { return EPraxErrorCode::RateLimitExceeded; }
		if (In == "QUOTA_EXCEEDED")        { return EPraxErrorCode::QuotaExceeded; }
		if (In == "EGRESS_LIMIT_EXCEEDED") { return EPraxErrorCode::EgressLimitExceeded; }
		if (In == "UNAUTHORIZED")          { return EPraxErrorCode::Unauthorized; }
		if (In == "FORBIDDEN")             { return EPraxErrorCode::Forbidden; }
		if (In == "INVALID_REQUEST")       { return EPraxErrorCode::InvalidRequest; }
		if (In == "INVALID_REFS")          { return EPraxErrorCode::InvalidRefs; }

		// No recognised code, so fall back to the status. This is the ONLY place a 429 with no code
		// is decided, and it resolves to RateLimitExceeded - the retryable reading.
		//
		// That direction is deliberate. Guessing QuotaExceeded would make an SDK give up on a
		// transient throttle and report an unfixable error to a player; guessing RateLimitExceeded
		// costs a few backed-off retries against a quota that is genuinely exhausted. The gateway
		// does send a code in practice, so this is the unlikely path either way.
		switch (Status)
		{
		case 400: return EPraxErrorCode::InvalidRequest;
		case 401: return EPraxErrorCode::Unauthorized;
		case 403: return EPraxErrorCode::Forbidden;
		case 404: return EPraxErrorCode::NotFound;
		case 429: return EPraxErrorCode::RateLimitExceeded;
		default:
			if (Status >= 500 && Status <= 599) { return EPraxErrorCode::ServerError; }
			return EPraxErrorCode::Unknown;
		}
	}

	const char* FPraxError::CodeToString(EPraxErrorCode In)
	{
		switch (In)
		{
		case EPraxErrorCode::None:                return "NONE";
		case EPraxErrorCode::RateLimitExceeded:   return "RATE_LIMIT_EXCEEDED";
		case EPraxErrorCode::QuotaExceeded:       return "QUOTA_EXCEEDED";
		case EPraxErrorCode::EgressLimitExceeded: return "EGRESS_LIMIT_EXCEEDED";
		case EPraxErrorCode::Unauthorized:        return "UNAUTHORIZED";
		case EPraxErrorCode::Forbidden:           return "FORBIDDEN";
		case EPraxErrorCode::InvalidRequest:      return "INVALID_REQUEST";
		case EPraxErrorCode::InvalidRefs:         return "INVALID_REFS";
		case EPraxErrorCode::NotFound:            return "NOT_FOUND";
		case EPraxErrorCode::Validation:          return "VALIDATION";
		case EPraxErrorCode::Network:             return "NETWORK";
		case EPraxErrorCode::Timeout:             return "TIMEOUT";
		case EPraxErrorCode::ServerError:         return "SERVER_ERROR";
		case EPraxErrorCode::Unknown:             return "UNKNOWN";
		}
		return "UNKNOWN";
	}

	bool FPraxError::IsRetryable() const
	{
		switch (Code)
		{
		// Transient by nature.
		case EPraxErrorCode::Network:
		case EPraxErrorCode::Timeout:
		case EPraxErrorCode::ServerError:
		case EPraxErrorCode::RateLimitExceeded:
			return true;

		// Both of these are HTTP 429 and neither is transient. This is the distinction the whole
		// enum exists for: retrying an exhausted allowance cannot succeed, so a retry loop here
		// drains a player's battery and the workspace's remaining calls to no purpose.
		case EPraxErrorCode::QuotaExceeded:
		case EPraxErrorCode::EgressLimitExceeded:
			return false;

		// A 401 is not retried by this flag. A caller may refresh the session and replay ONCE, which
		// is a different operation from retrying the same request with the same dead token.
		case EPraxErrorCode::Unauthorized:
		case EPraxErrorCode::Forbidden:
		case EPraxErrorCode::InvalidRequest:
		case EPraxErrorCode::InvalidRefs:
		case EPraxErrorCode::NotFound:
		case EPraxErrorCode::Validation:
		case EPraxErrorCode::None:
			return false;

		// An unclassified failure is not retried. Retrying something we could not identify is how a
		// single bad request becomes a loop against production.
		case EPraxErrorCode::Unknown:
			return false;
		}
		return false;
	}

	std::string FPraxError::ToString() const
	{
		std::string Out = CodeToString(Code);
		if (!ServerCode.empty() && ServerCode != Out)
		{
			Out += "(" + ServerCode + ")";
		}
		if (Status != 0)
		{
			Out += " HTTP " + std::to_string(Status);
		}
		if (!Message.empty())
		{
			Out += ": " + Message;
		}
		for (const std::string& Detail : Details)
		{
			Out += " | " + Detail;
		}
		return Out;
	}

	FPraxError FPraxError::FromResponse(int Status, const FJsonValue& Body)
	{
		FPraxError Out;
		Out.Status = Status;

		// The gateway has THREE error shapes, and assuming one mis-parses the other two. All three
		// are handled here so no caller has to know which route it was talking to.
		const FJsonValue& ErrorField = Body.Field("error");

		if (ErrorField.IsObject())
		{
			// /query - {"error":{"code":..,"message":..,"details":[..]}}
			Out.ServerCode = ErrorField.Field("code").AsString();
			Out.Message = ErrorField.Field("message").AsString();
			Out.Details = ReadDetails(ErrorField.Field("details"));
		}
		else if (ErrorField.IsString())
		{
			// /files and /endpoint - a BARE STRING, not an object. There is no code to read, so the
			// status is all there is to classify on.
			Out.Message = ErrorField.AsString();
		}
		else if (Body.HasField("isSuccess"))
		{
			// /auth/* - the platform envelope. The message is top level and the failures are in an
			// "errors" array rather than "details".
			Out.Message = Body.Field("message").AsString();
			Out.Details = ReadDetails(Body.Field("errors"));
			if (Out.Message.empty() && !Out.Details.empty())
			{
				Out.Message = Out.Details.front();
			}
		}
		else
		{
			// A failing status with a body in none of the three shapes. Keep the body rather than
			// discard it: it is the only evidence of what happened.
			Out.Message = Body.IsNull() ? "" : Body.ToString();
		}

		Out.Code = CodeFromString(Out.ServerCode, Status);
		if (Out.Message.empty())
		{
			Out.Message = std::string("request failed with status ") + std::to_string(Status);
		}
		return Out;
	}

	FPraxError FPraxError::FromUnparseableResponse(int Status, const std::string& RawBody)
	{
		FPraxError Out;
		Out.Status = Status;
		Out.Code = (Status >= 200 && Status <= 299)
			// A 2xx whose body is not JSON is a broken response, not a success. Reporting success
			// here and handing the caller an empty result would be the silent-wrong-data failure the
			// whole contract exists to avoid.
			? EPraxErrorCode::Unknown
			: CodeFromString(std::string(), Status);

		// Truncated deliberately. An HTML error page from a proxy is tens of kilobytes and putting
		// all of it in a log line buries every other message around it.
		const size_t Limit = 256;
		Out.Message = "response was not JSON: "
			+ (RawBody.size() > Limit ? RawBody.substr(0, Limit) + "..." : RawBody);
		return Out;
	}

	FPraxError FPraxError::MakeValidation(std::string InServerCode, std::string InMessage)
	{
		FPraxError Out;
		Out.Code = EPraxErrorCode::Validation;
		Out.ServerCode = std::move(InServerCode);
		Out.Message = std::move(InMessage);
		return Out;
	}

	FPraxError FPraxError::MakeNetwork(std::string InMessage)
	{
		FPraxError Out;
		Out.Code = EPraxErrorCode::Network;
		Out.Message = std::move(InMessage);
		return Out;
	}

	FPraxError FPraxError::MakeTimeout(std::string InMessage)
	{
		FPraxError Out;
		Out.Code = EPraxErrorCode::Timeout;
		Out.Message = std::move(InMessage);
		return Out;
	}
}
