#include "Core/PraxEnvelope.h"

namespace Prax
{
	namespace
	{
		bool IsSuccessStatus(int Status)
		{
			return Status >= 200 && Status <= 299;
		}
	}

	namespace Envelope
	{
		bool ReadQuery(int Status, const FJsonValue& Body, FPraxPage& OutPage, FPraxError& OutError)
		{
			if (!IsSuccessStatus(Status))
			{
				OutError = FPraxError::FromResponse(Status, Body);
				return false;
			}

			// No unwrapping. The body IS the result.
			const FJsonValue& Data = Body.Field("data");
			if (!Data.IsArray())
			{
				// A 2xx with no data array is a malformed response, not an empty page. Treating it as
				// empty would report "no rows matched" for what is actually a broken reply.
				OutError = FPraxError::MakeValidation("MALFORMED_RESPONSE",
					"a /query response carried no data array");
				return false;
			}

			OutPage = FPraxPage{};
			OutPage.Rows = Data.AsArray();

			const FJsonValue& Meta = Body.Field("meta");

			// meta.limit, not the limit that was requested. A table scope can clamp it, and code that
			// assumes its own limit was honoured turns pagination into an infinite loop.
			OutPage.Limit = Meta.Field("limit").AsInt(0);
			OutPage.Offset = Meta.Field("offset").AsInt(0);
			OutPage.Count = Meta.Field("count").AsInt(static_cast<int64_t>(OutPage.Rows.size()));
			OutPage.DurationMs = Meta.Field("durationMs").AsInt(0);

			// "total" is the field's real name. totalCount, totalRows and rowCount do not exist.
			// It is absent - not zero - unless includeTotalCount was requested, so presence is
			// checked rather than the value read blindly.
			const FJsonValue& Total = Meta.Field("total");
			if (Total.IsNumber())
			{
				OutPage.Total = Total.AsInt(0);
				OutPage.bHasTotal = true;
			}

			OutError = FPraxError{};
			return true;
		}

		bool ReadAuth(int Status, const FJsonValue& Body, FJsonValue& OutData, FPraxError& OutError)
		{
			// The status alone is not enough here. The platform envelope carries its own isSuccess,
			// and a 200 with isSuccess false is a failure that a status check would wave through.
			const FJsonValue& IsSuccess = Body.Field("isSuccess");
			const bool bEnvelopeSaysFailed = IsSuccess.IsBool() && !IsSuccess.AsBool();

			if (!IsSuccessStatus(Status) || bEnvelopeSaysFailed)
			{
				OutError = FPraxError::FromResponse(Status, Body);
				return false;
			}

			// This is the one route family whose payload is nested under .data.
			OutData = Body.Field("data");
			OutError = FPraxError{};
			return true;
		}

		bool ReadEndpoint(int Status, const FJsonValue& Body, FJsonValue& OutResult,
						  FPraxError& OutError)
		{
			if (!IsSuccessStatus(Status))
			{
				OutError = FPraxError::FromResponse(Status, Body);
				return false;
			}

			// The body, unchanged. No .data lookup, deliberately - see PraxEnvelope.h. An automation
			// returning {"ok":true,"data":{...}} must reach the caller whole, with the ok field
			// intact, not silently reduced to its data member.
			OutResult = Body;
			OutError = FPraxError{};
			return true;
		}

		bool ReadFiles(int Status, const FJsonValue& Body, FJsonValue& OutResult,
					   FPraxError& OutError)
		{
			if (!IsSuccessStatus(Status))
			{
				// FromResponse already handles the bare-string error shape this route uses.
				OutError = FPraxError::FromResponse(Status, Body);
				return false;
			}
			OutResult = Body;
			OutError = FPraxError{};
			return true;
		}
	}
}
