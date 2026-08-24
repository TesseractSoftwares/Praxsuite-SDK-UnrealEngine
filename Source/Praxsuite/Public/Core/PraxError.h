// Error taxonomy.
//
// Portable C++17, no Unreal types - see Core/PraxJson.h for why.
//
// The point of this file is that callers branch on a CODE, never on message text. Message wording is
// not a contract and changes; a code is. The one distinction that earns its own type is quota versus
// rate limit: both arrive as HTTP 429 and they mean opposite things. Retrying a rate limit is
// correct. Retrying an exhausted monthly quota cannot possibly succeed - it just burns a player's
// battery and your API allowance until someone upgrades the workspace.

#pragma once

#include <string>
#include <vector>

#include "Core/PraxJson.h"

namespace Prax
{
	enum class EPraxErrorCode
	{
		/** No error. */
		None,

		/** 429, transient. Back off and retry. */
		RateLimitExceeded,

		/** 429, NOT transient. The monthly allowance is gone; only an upgrade fixes it. */
		QuotaExceeded,

		/** 429, NOT transient. Data-transfer allowance exhausted. */
		EgressLimitExceeded,

		/** 401. May warrant one token refresh and a single replay. */
		Unauthorized,

		/** 403. A scope problem, not a query problem - retrying and rewording both fail. */
		Forbidden,

		/** 400. */
		InvalidRequest,

		/** 400, specifically a bad refs block. */
		InvalidRefs,

		/** 404. Frequently the wrong gateway host rather than a missing row - see PraxRoutes.h. */
		NotFound,

		/** Refused by this SDK before anything was sent. */
		Validation,

		/** Transport failure - no response at all. Retryable. */
		Network,

		/** The request timed out. Retryable for reads. */
		Timeout,

		/** 5xx. Retryable. */
		ServerError,

		/** A response that did not parse, or a status with no mapping. */
		Unknown,
	};

	/**
	 * An error from the SDK or the gateway.
	 *
	 * A value type rather than an exception hierarchy: Unreal is routinely built with exceptions
	 * disabled, so an SDK that signals failure by throwing is unusable in the environment it targets.
	 * Every call returns one of these instead.
	 */
	struct FPraxError
	{
		EPraxErrorCode Code = EPraxErrorCode::None;

		/**
		 * The code exactly as the server sent it.
		 *
		 * Kept alongside the enum so a NEW server-side code is still legible to a caller and still
		 * logged usefully, rather than being flattened to Unknown and losing the only clue.
		 */
		std::string ServerCode;

		int Status = 0;
		std::string Message;
		std::vector<std::string> Details;

		bool IsError() const { return Code != EPraxErrorCode::None; }

		/**
		 * Whether retrying this exact request could plausibly succeed.
		 *
		 * Note what this does NOT decide: whether retrying is safe. A failed insert may well have
		 * been applied, so writes are never retried regardless of what this returns. Retrying a
		 * write is how one purchase becomes two rows.
		 */
		bool IsRetryable() const;

		/** A human-readable one-liner for a log. Credentials are already scrubbed by PraxKeyGuard. */
		std::string ToString() const;

		/** Classifies a gateway response. Body may be any of the gateway's three error shapes. */
		static FPraxError FromResponse(int Status, const FJsonValue& Body);

		/** Classifies a gateway response whose body did not parse as JSON. */
		static FPraxError FromUnparseableResponse(int Status, const std::string& RawBody);

		/** An error this SDK raised itself, before sending anything. */
		static FPraxError MakeValidation(std::string ServerCode, std::string Message);

		static FPraxError MakeNetwork(std::string Message);
		static FPraxError MakeTimeout(std::string Message);

		/** Maps a server code string to the enum. Unrecognised codes become Unknown. */
		static EPraxErrorCode CodeFromString(const std::string& In, int Status);

		/** The stable name of a code, for logs. */
		static const char* CodeToString(EPraxErrorCode In);
	};
}
