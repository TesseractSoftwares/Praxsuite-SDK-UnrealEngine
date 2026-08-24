#include "PraxsuiteTransport.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"

#include "Core/PraxKeyGuard.h"
#include "Core/PraxRoutes.h"
#include "PraxsuiteLog.h"
#include "PraxsuiteSettings.h"

DEFINE_LOG_CATEGORY(LogPraxsuite);

namespace
{
	/** Scrubs before logging, always. See PraxsuiteLog.h for why this is not optional. */
	void LogScrubbed(bool bVerbose, const FString& Prefix, const FString& Text)
	{
		if (!bVerbose)
		{
			return;
		}
		const FString Safe = PraxConvert::ToUnreal(
			Prax::KeyGuard::Scrub(PraxConvert::ToStd(Text)));
		UE_LOG(LogPraxsuite, Verbose, TEXT("%s %s"), *Prefix, *Safe);
	}

	bool LooksLikeInteger(const FString& In)
	{
		if (In.IsEmpty())
		{
			return false;
		}
		int32 Start = (In[0] == TEXT('-')) ? 1 : 0;
		if (Start >= In.Len())
		{
			return false;
		}
		// A leading zero is treated as text, not a number: "007" is a player's chosen name or an id
		// with meaningful padding far more often than it is the integer seven.
		if (In[Start] == TEXT('0') && In.Len() > Start + 1)
		{
			return false;
		}
		for (int32 i = Start; i < In.Len(); ++i)
		{
			if (!FChar::IsDigit(In[i]))
			{
				return false;
			}
		}
		return true;
	}

	bool LooksLikeDouble(const FString& In)
	{
		if (In.IsEmpty() || !In.Contains(TEXT(".")))
		{
			return false;
		}
		int32 Start = (In[0] == TEXT('-')) ? 1 : 0;
		int32 Dots = 0;
		for (int32 i = Start; i < In.Len(); ++i)
		{
			if (In[i] == TEXT('.')) { ++Dots; continue; }
			if (!FChar::IsDigit(In[i])) { return false; }
		}
		return Dots == 1 && In.Len() > Start + 1;
	}
}

namespace PraxConvert
{
	std::string ToStd(const FString& In)
	{
		// UTF-8, not ANSI. TCHAR_TO_ANSI would replace every non-Latin character with '?', which
		// silently corrupts any player name that is not English.
		return std::string(TCHAR_TO_UTF8(*In));
	}

	FString ToUnreal(const std::string& In)
	{
		return FString(UTF8_TO_TCHAR(In.c_str()));
	}

	EPraxError ToBlueprintError(Prax::EPraxErrorCode In)
	{
		switch (In)
		{
		case Prax::EPraxErrorCode::None:                return EPraxError::None;
		case Prax::EPraxErrorCode::RateLimitExceeded:   return EPraxError::RateLimit;
		case Prax::EPraxErrorCode::QuotaExceeded:       return EPraxError::Quota;
		case Prax::EPraxErrorCode::EgressLimitExceeded: return EPraxError::EgressLimit;
		case Prax::EPraxErrorCode::Unauthorized:        return EPraxError::Unauthorized;
		case Prax::EPraxErrorCode::Forbidden:           return EPraxError::Forbidden;
		case Prax::EPraxErrorCode::InvalidRequest:      return EPraxError::InvalidRequest;
		case Prax::EPraxErrorCode::InvalidRefs:         return EPraxError::InvalidRefs;
		case Prax::EPraxErrorCode::NotFound:            return EPraxError::NotFound;
		case Prax::EPraxErrorCode::Validation:          return EPraxError::Validation;
		case Prax::EPraxErrorCode::Network:             return EPraxError::Network;
		case Prax::EPraxErrorCode::Timeout:             return EPraxError::Timeout;
		case Prax::EPraxErrorCode::ServerError:         return EPraxError::ServerError;
		case Prax::EPraxErrorCode::Unknown:             return EPraxError::Unknown;
		}
		return EPraxError::Unknown;
	}

	FPraxResult ToResult(const Prax::FPraxError& In)
	{
		FPraxResult Out;
		Out.bSuccess = !In.IsError();
		Out.Error = ToBlueprintError(In.Code);
		Out.ErrorCode = ToUnreal(In.ServerCode.empty()
			? std::string(Prax::FPraxError::CodeToString(In.Code))
			: In.ServerCode);
		Out.Message = ToUnreal(In.Message);
		Out.HttpStatus = In.Status;
		Out.bRetryable = In.IsRetryable();
		return Out;
	}

	FPraxResult MakeSuccess()
	{
		FPraxResult Out;
		Out.bSuccess = true;
		Out.Error = EPraxError::None;
		return Out;
	}

	FPraxRow ToRow(const Prax::FJsonValue& In)
	{
		FPraxRow Row;
		for (const Prax::FJsonMember& Member : In.AsObject())
		{
			const FString Key = ToUnreal(Member.Key);
			if (Member.Value.IsString())
			{
				Row.Fields.Add(Key, ToUnreal(Member.Value.AsString()));
			}
			else if (Member.Value.IsNull())
			{
				// Empty rather than the text "null". A Blueprint comparing against "" is the natural
				// reading of an absent value, and "null" would compare unequal to everything.
				Row.Fields.Add(Key, FString());
			}
			else
			{
				// Numbers, booleans, nested objects and arrays all become their JSON text. Flattening
				// a nested object would invent field names that do not exist in the table.
				Row.Fields.Add(Key, ToUnreal(Member.Value.ToString()));
			}
		}
		return Row;
	}

	FPraxPageResult ToPage(const Prax::FPraxPage& In)
	{
		FPraxPageResult Out;
		Out.Rows.Reserve(static_cast<int32>(In.Rows.size()));
		for (const Prax::FJsonValue& Row : In.Rows)
		{
			Out.Rows.Add(ToRow(Row));
		}
		Out.Limit = static_cast<int32>(In.Limit);
		Out.Offset = static_cast<int32>(In.Offset);
		Out.Count = static_cast<int32>(In.Count);
		Out.Total = static_cast<int32>(In.Total);
		Out.bHasTotal = In.bHasTotal;
		return Out;
	}

	Prax::FJsonValue ToJsonValue(const FString& In)
	{
		if (In.Equals(TEXT("true"), ESearchCase::CaseSensitive))  { return Prax::FJsonValue(true); }
		if (In.Equals(TEXT("false"), ESearchCase::CaseSensitive)) { return Prax::FJsonValue(false); }
		if (LooksLikeInteger(In))
		{
			return Prax::FJsonValue(static_cast<int64>(FCString::Atoi64(*In)));
		}
		if (LooksLikeDouble(In))
		{
			return Prax::FJsonValue(FCString::Atod(*In));
		}
		return Prax::FJsonValue(ToStd(In));
	}

	Prax::FJsonValue ToJsonObject(const TMap<FString, FString>& In)
	{
		Prax::FJsonValue Out = Prax::FJsonValue::Object();
		for (const TPair<FString, FString>& Pair : In)
		{
			Out.SetField(ToStd(Pair.Key), ToJsonValue(Pair.Value));
		}
		return Out;
	}
}

FPraxsuiteTransport::FPraxsuiteTransport() = default;

bool FPraxsuiteTransport::LoadFromSettings(Prax::FPraxError& OutError)
{
	const UPraxsuiteSettings* Settings = UPraxsuiteSettings::Get();
	if (Settings == nullptr)
	{
		OutError = Prax::FPraxError::MakeValidation("NO_SETTINGS",
			"the Praxsuite settings object was not available");
		return false;
	}

	TimeoutSeconds = Settings->TimeoutSeconds;
	EndpointTimeoutSeconds = Settings->EndpointTimeoutSeconds;
	MaxReadAttempts = Settings->MaxReadAttempts;
	bVerboseLogging = Settings->bVerboseLogging;

	return Configure(Settings->WorkspaceId, Settings->PublishableKey, Settings->GatewayHost,
					 Settings->bClientSide, OutError);
}

bool FPraxsuiteTransport::Configure(const FString& InWorkspaceId, const FString& InCredential,
									const FString& InGatewayHost, bool bInClientSide,
									Prax::FPraxError& OutError)
{
	const std::string StdCredential = PraxConvert::ToStd(InCredential);

	// The one check that happens before anything else. A packaged game is a program on somebody
	// else's computer, so a secret key in it is a published secret - and there is no override,
	// because an override is a slower route to the same leak.
	if (bInClientSide && !Prax::KeyGuard::IsAllowedClientSide(StdCredential))
	{
		OutError = Prax::FPraxError::MakeValidation("SECRET_KEY_REFUSED",
			"a secret key (sk_live_) cannot be used in a client build - any string inside a packaged "
			"game is readable. Use a publishable key (pk_live_), or turn off Client Side only if this "
			"build never reaches a player.");
		return false;
	}

	WorkspaceId = InWorkspaceId;
	Credential = InCredential;
	GatewayHost = InGatewayHost;
	bClientSide = bInClientSide;

	OutError = Prax::FPraxError{};

	if (bVerboseLogging)
	{
		UE_LOG(LogPraxsuite, Log, TEXT("configured workspace %s at %s with key %s"),
			*WorkspaceId, *GatewayHost,
			*PraxConvert::ToUnreal(Prax::KeyGuard::Fingerprint(StdCredential)));
	}
	return true;
}

bool FPraxsuiteTransport::IsConfigured() const
{
	return !WorkspaceId.IsEmpty() && !Credential.IsEmpty() && !GatewayHost.IsEmpty();
}

void FPraxsuiteTransport::SetSession(const FString& InUserId, const FString& InEmail,
									 const FString& InToken)
{
	SessionUserId = InUserId;
	SessionEmail = InEmail;
	SessionToken = InToken;
}

void FPraxsuiteTransport::ClearSession()
{
	SessionUserId.Empty();
	SessionEmail.Empty();
	SessionToken.Empty();
}

FPraxSession FPraxsuiteTransport::GetSession() const
{
	FPraxSession Out;
	Out.UserId = SessionUserId;
	Out.Email = SessionEmail;
	// The token itself is never copied out - see FPraxSession's comment.
	Out.bHasToken = !SessionToken.IsEmpty();
	return Out;
}

bool FPraxsuiteTransport::IsLoggedIn() const
{
	return !SessionToken.IsEmpty();
}

FString FPraxsuiteTransport::BuildUrl(const FString& Route) const
{
	return PraxConvert::ToUnreal(Prax::Routes::Build(
		PraxConvert::ToStd(GatewayHost), PraxConvert::ToStd(WorkspaceId),
		PraxConvert::ToStd(Route)));
}

void FPraxsuiteTransport::Post(const FString& Url, const Prax::FJsonValue& Body, bool bIsWrite,
							   float InTimeoutSeconds, FResponseHandler Handler)
{
	Send(Url, PraxConvert::ToUnreal(Body.ToString()), bIsWrite, InTimeoutSeconds, 1,
		 MoveTemp(Handler));
}

void FPraxsuiteTransport::Send(const FString& Url, const FString& BodyText, bool bIsWrite,
							   float InTimeoutSeconds, int32 Attempt, FResponseHandler Handler)
{
	LogScrubbed(bVerboseLogging, FString::Printf(TEXT("POST %s attempt %d body"), *Url, Attempt),
				BodyText);

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request =
		FHttpModule::Get().CreateRequest();

	Request->SetURL(Url);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	// The credential goes in a HEADER, never a URL. URLs are logged by proxies, kept in crash
	// reports and pasted into bug threads; headers are not.
	Request->SetHeader(TEXT("x-api-key"), Credential);

	// A session token, when we have one, is what the server derives the player's identity from.
	// It is the reason an automation can trust a claim about who is calling.
	if (!SessionToken.IsEmpty())
	{
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *SessionToken));
	}

	Request->SetTimeout(InTimeoutSeconds);
	Request->SetContentAsString(BodyText);

	const int32 MaxAttempts = bIsWrite ? 1 : FMath::Max(1, MaxReadAttempts);
	const bool bVerbose = bVerboseLogging;

	Request->OnProcessRequestComplete().BindLambda(
		[this, Url, BodyText, bIsWrite, InTimeoutSeconds, Attempt, MaxAttempts, bVerbose, Handler]
		(FHttpRequestPtr, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			if (!bConnectedSuccessfully || !Response.IsValid())
			{
				const Prax::FPraxError Error = Prax::FPraxError::MakeNetwork(
					"the request did not reach the gateway");

				// Retry reads only. A failed WRITE is never retried: the request may well have been
				// applied before the connection dropped, and retrying is how one purchase becomes
				// two rows. There is no way to tell from here which happened.
				if (!bIsWrite && Attempt < MaxAttempts)
				{
					UE_LOG(LogPraxsuite, Warning,
						TEXT("network failure on attempt %d of %d, retrying"), Attempt, MaxAttempts);
					Send(Url, BodyText, bIsWrite, InTimeoutSeconds, Attempt + 1, Handler);
					return;
				}
				Handler(0, Prax::FJsonValue(), false, FString());
				return;
			}

			const int32 Status = Response->GetResponseCode();
			const FString Raw = Response->GetContentAsString();
			LogScrubbed(bVerbose, FString::Printf(TEXT("HTTP %d body"), Status), Raw);

			Prax::FJsonValue Parsed;
			std::string ParseError;
			const bool bParsed = Prax::FJsonValue::Parse(PraxConvert::ToStd(Raw), Parsed, ParseError);

			// A retryable status gets the same treatment as a network failure, for reads.
			if (!bIsWrite && Attempt < MaxAttempts)
			{
				const Prax::FPraxError Probe = bParsed
					? Prax::FPraxError::FromResponse(Status, Parsed)
					: Prax::FPraxError::FromUnparseableResponse(Status, PraxConvert::ToStd(Raw));
				if (Probe.IsError() && Probe.IsRetryable())
				{
					UE_LOG(LogPraxsuite, Warning, TEXT("%s on attempt %d of %d, retrying"),
						*PraxConvert::ToUnreal(Probe.ToString()), Attempt, MaxAttempts);
					Send(Url, BodyText, bIsWrite, InTimeoutSeconds, Attempt + 1, Handler);
					return;
				}
			}

			Handler(Status, Parsed, bParsed, Raw);
		});

	Request->ProcessRequest();
}
