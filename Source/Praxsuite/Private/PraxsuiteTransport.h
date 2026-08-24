// HTTP transport and type conversion. Private to the module.
//
// Part of the Unreal layer - CI cannot compile this. See PraxsuiteTypes.h.
//
// This is the whole of the engine-dependent surface: build a request, send it, convert what comes
// back. It contains no decisions about what a valid request looks like, which operators exist, how
// an error should be classified or whether a retry is safe - all of that is in Core/, where it is
// compiled and executed on every commit. Keeping this file boring is the point of the split.

#pragma once

#include "CoreMinimal.h"

#include "Core/PraxError.h"
#include "Core/PraxJson.h"
#include "PraxsuiteTypes.h"

/** Configuration and session state, plus the request plumbing. */
class FPraxsuiteTransport
{
public:
	FPraxsuiteTransport();

	/** Loads Project Settings. Returns false and fills OutError on a refused credential. */
	bool LoadFromSettings(Prax::FPraxError& OutError);

	bool Configure(const FString& InWorkspaceId, const FString& InCredential,
				   const FString& InGatewayHost, bool bInClientSide, Prax::FPraxError& OutError);

	bool IsConfigured() const;

	void SetSession(const FString& InUserId, const FString& InEmail, const FString& InToken);
	void ClearSession();
	FPraxSession GetSession() const;
	bool IsLoggedIn() const;

	/** Called with (status, parsed body, parse succeeded, raw body). */
	using FResponseHandler = TFunction<void(int32, const Prax::FJsonValue&, bool, const FString&)>;

	/**
	 * Sends a POST.
	 *
	 * bIsWrite decides whether a transient failure may be retried. A failed write is NEVER retried:
	 * the request may well have been applied before the connection dropped, and retrying is how one
	 * purchase becomes two rows. Reads are retried with backoff.
	 */
	void Post(const FString& Url, const Prax::FJsonValue& Body, bool bIsWrite,
			  float TimeoutSeconds, FResponseHandler Handler);

	FString BuildUrl(const FString& Route) const;
	FString GetWorkspaceId() const { return WorkspaceId; }

	int32 GetTimeoutSeconds() const { return TimeoutSeconds; }
	int32 GetEndpointTimeoutSeconds() const { return EndpointTimeoutSeconds; }

private:
	void Send(const FString& Url, const FString& BodyText, bool bIsWrite, float TimeoutSeconds,
			  int32 Attempt, FResponseHandler Handler);

	FString WorkspaceId;
	FString Credential;
	FString GatewayHost;
	bool bClientSide = true;

	FString SessionUserId;
	FString SessionEmail;
	FString SessionToken;

	int32 TimeoutSeconds = 30;
	int32 EndpointTimeoutSeconds = 100;
	int32 MaxReadAttempts = 3;
	bool bVerboseLogging = false;
};

namespace PraxConvert
{
	/** UTF-8 in both directions. Not TCHAR_TO_ANSI, which would mangle every non-Latin name. */
	std::string ToStd(const FString& In);
	FString ToUnreal(const std::string& In);

	/** Maps the core's error enum onto the Blueprint one. */
	EPraxError ToBlueprintError(Prax::EPraxErrorCode In);

	FPraxResult ToResult(const Prax::FPraxError& In);
	FPraxResult MakeSuccess();

	/** One row. Nested objects and arrays are kept as their JSON text - see FPraxRow's comment. */
	FPraxRow ToRow(const Prax::FJsonValue& In);

	FPraxPageResult ToPage(const Prax::FPraxPage& In);

	/**
	 * A Blueprint string map to a JSON object.
	 *
	 * Values that look like a number or a boolean are converted to that type. That is a judgement
	 * call worth stating: a Blueprint map can only hold strings, so sending everything as a string
	 * would make an integer column reject every write from Blueprint. The cost is that a genuinely
	 * textual "123" becomes a number - so the C++ API exists for callers who need exact control.
	 */
	Prax::FJsonValue ToJsonObject(const TMap<FString, FString>& In);

	/** A single Blueprint string value, with the same number/bool inference. */
	Prax::FJsonValue ToJsonValue(const FString& In);
}
