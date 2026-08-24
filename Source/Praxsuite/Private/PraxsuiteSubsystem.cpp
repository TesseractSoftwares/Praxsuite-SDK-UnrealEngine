#include "PraxsuiteSubsystem.h"

#include "Core/PraxEnvelope.h"
#include "Core/PraxFilters.h"
#include "Core/PraxQuery.h"
#include "Core/PraxRoutes.h"
#include "PraxsuiteLog.h"
#include "PraxsuiteTransport.h"

namespace
{
	/**
	 * A refusal that still calls back.
	 *
	 * Every guardrail in the core refuses synchronously, which is correct - but a Blueprint node
	 * whose completion pin never fires looks like a hang, and a player reports "the game froze"
	 * rather than "saving failed". So a refusal is delivered through the same delegate as a success.
	 * The one thing worse than an unscoped delete is a game that silently stops instead of saying
	 * why it did not happen.
	 */
	template <typename TDelegate, typename TPayload>
	void FailImmediately(const TDelegate& Delegate, const Prax::FPraxError& Error, TPayload Payload)
	{
		UE_LOG(LogPraxsuite, Warning, TEXT("refused before sending: %s"),
			*PraxConvert::ToUnreal(Error.ToString()));
		Delegate.ExecuteIfBound(PraxConvert::ToResult(Error), Payload);
	}

	Prax::FPraxError NotConfigured()
	{
		return Prax::FPraxError::MakeValidation("NOT_CONFIGURED",
			"Praxsuite is not configured. Fill in Project Settings > Plugins > Praxsuite, or call "
			"Configure at runtime.");
	}
}

void UPraxsuiteSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Transport = MakeUnique<FPraxsuiteTransport>();

	Prax::FPraxError Error;
	if (!Transport->LoadFromSettings(Error))
	{
		// Loud, because a refused credential means nothing will work and the reason is fixable in
		// one settings field. Not fatal, because Configure can still supply valid settings at
		// runtime - a game that fetches its configuration at launch is a legitimate arrangement.
		UE_LOG(LogPraxsuite, Error, TEXT("%s"), *PraxConvert::ToUnreal(Error.ToString()));
	}
	else if (!Transport->IsConfigured())
	{
		UE_LOG(LogPraxsuite, Warning,
			TEXT("Praxsuite is not configured yet. Set the workspace id, publishable key and "
				 "gateway host in Project Settings > Plugins > Praxsuite."));
	}
}

void UPraxsuiteSubsystem::Deinitialize()
{
	Transport.Reset();
	Super::Deinitialize();
}

FPraxResult UPraxsuiteSubsystem::Configure(const FString& WorkspaceId, const FString& Credential,
										   const FString& GatewayHost, bool bClientSide)
{
	if (!Transport.IsValid())
	{
		return PraxConvert::ToResult(NotConfigured());
	}
	Prax::FPraxError Error;
	if (!Transport->Configure(WorkspaceId, Credential, GatewayHost, bClientSide, Error))
	{
		UE_LOG(LogPraxsuite, Error, TEXT("%s"), *PraxConvert::ToUnreal(Error.ToString()));
		return PraxConvert::ToResult(Error);
	}
	return PraxConvert::MakeSuccess();
}

bool UPraxsuiteSubsystem::IsConfigured() const
{
	return Transport.IsValid() && Transport->IsConfigured();
}

void UPraxsuiteSubsystem::QueryTable(const FString& Table, const TArray<FString>& SelectColumns,
									 const FString& OrderByColumn, bool bOrderByDescending,
									 int32 Limit, int32 Offset, bool bIncludeTotalCount,
									 const FPraxQueryDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), FPraxPageResult{});
		return;
	}

	Prax::FQueryBuilder Builder(PraxConvert::ToStd(Table));

	std::vector<std::string> Columns;
	Columns.reserve(static_cast<size_t>(SelectColumns.Num()));
	for (const FString& Column : SelectColumns)
	{
		Columns.push_back(PraxConvert::ToStd(Column));
	}
	if (!Columns.empty())
	{
		Builder.Select(Columns);
	}
	if (!OrderByColumn.IsEmpty())
	{
		Builder.OrderBy(PraxConvert::ToStd(OrderByColumn), bOrderByDescending);
	}
	Builder.Limit(Limit).Offset(Offset).WithTotalCount(bIncludeTotalCount);

	Prax::FJsonValue Body;
	Prax::FPraxError Error;
	if (!Builder.Build(Body, Error))
	{
		FailImmediately(OnComplete, Error, FPraxPageResult{});
		return;
	}

	const FPraxQueryDelegate Delegate = OnComplete;
	Transport->Post(Transport->BuildUrl(TEXT("query")), Body, /*bIsWrite=*/false,
		static_cast<float>(Transport->GetTimeoutSeconds()),
		[Delegate](int32 Status, const Prax::FJsonValue& Parsed, bool bParsed, const FString& Raw)
		{
			if (!bParsed)
			{
				const Prax::FPraxError Failure = Prax::FPraxError::FromUnparseableResponse(
					Status, PraxConvert::ToStd(Raw));
				Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), FPraxPageResult{});
				return;
			}

			Prax::FPraxPage Page;
			Prax::FPraxError Failure;
			if (!Prax::Envelope::ReadQuery(Status, Parsed, Page, Failure))
			{
				Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), FPraxPageResult{});
				return;
			}
			Delegate.ExecuteIfBound(PraxConvert::MakeSuccess(), PraxConvert::ToPage(Page));
		});
}

void UPraxsuiteSubsystem::QueryTableWhereEquals(const FString& Table, const FString& Column,
											    const FString& Value, int32 Limit,
											    const FPraxQueryDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), FPraxPageResult{});
		return;
	}

	Prax::FJsonValue Body;
	Prax::FPraxError Error;
	Prax::FQueryBuilder Builder(PraxConvert::ToStd(Table));
	Builder.Where(Prax::Filters::Eq(PraxConvert::ToStd(Column), PraxConvert::ToJsonValue(Value)))
		   .Limit(Limit);

	if (!Builder.Build(Body, Error))
	{
		FailImmediately(OnComplete, Error, FPraxPageResult{});
		return;
	}

	const FPraxQueryDelegate Delegate = OnComplete;
	Transport->Post(Transport->BuildUrl(TEXT("query")), Body, /*bIsWrite=*/false,
		static_cast<float>(Transport->GetTimeoutSeconds()),
		[Delegate](int32 Status, const Prax::FJsonValue& Parsed, bool bParsed, const FString& Raw)
		{
			if (!bParsed)
			{
				const Prax::FPraxError Failure = Prax::FPraxError::FromUnparseableResponse(
					Status, PraxConvert::ToStd(Raw));
				Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), FPraxPageResult{});
				return;
			}
			Prax::FPraxPage Page;
			Prax::FPraxError Failure;
			if (!Prax::Envelope::ReadQuery(Status, Parsed, Page, Failure))
			{
				Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), FPraxPageResult{});
				return;
			}
			Delegate.ExecuteIfBound(PraxConvert::MakeSuccess(), PraxConvert::ToPage(Page));
		});
}

namespace
{
	/** Every write reports the same way, so the response handling is shared. */
	void SendWrite(FPraxsuiteTransport& Transport, const Prax::FJsonValue& Body,
				   const FPraxWriteDelegate& OnComplete)
	{
		const FPraxWriteDelegate Delegate = OnComplete;
		Transport.Post(Transport.BuildUrl(TEXT("query")), Body, /*bIsWrite=*/true,
			static_cast<float>(Transport.GetTimeoutSeconds()),
			[Delegate](int32 Status, const Prax::FJsonValue& Parsed, bool bParsed, const FString& Raw)
			{
				if (!bParsed)
				{
					const Prax::FPraxError Failure = Prax::FPraxError::FromUnparseableResponse(
						Status, PraxConvert::ToStd(Raw));
					Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), 0);
					return;
				}

				Prax::FPraxPage Page;
				Prax::FPraxError Failure;
				if (!Prax::Envelope::ReadQuery(Status, Parsed, Page, Failure))
				{
					Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), 0);
					return;
				}
				// A mutation returns the affected rows in the same data array a read uses.
				Delegate.ExecuteIfBound(PraxConvert::MakeSuccess(),
										static_cast<int32>(Page.Rows.size()));
			});
	}
}

void UPraxsuiteSubsystem::InsertRow(const FString& Table, const TMap<FString, FString>& Values,
									const FPraxWriteDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), 0);
		return;
	}

	Prax::FJsonValue Body;
	Prax::FPraxError Error;
	if (!Prax::Mutations::Insert(PraxConvert::ToStd(Table), PraxConvert::ToJsonObject(Values),
								 Body, Error))
	{
		FailImmediately(OnComplete, Error, 0);
		return;
	}
	SendWrite(*Transport, Body, OnComplete);
}

void UPraxsuiteSubsystem::UpdateRowById(const FString& Table, const FString& RowId,
										const TMap<FString, FString>& Values,
										const FPraxWriteDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), 0);
		return;
	}

	Prax::FJsonValue Body;
	Prax::FPraxError Error;
	if (!Prax::Mutations::UpdateById(PraxConvert::ToStd(Table), PraxConvert::ToStd(RowId),
									 PraxConvert::ToJsonObject(Values), Body, Error))
	{
		FailImmediately(OnComplete, Error, 0);
		return;
	}
	SendWrite(*Transport, Body, OnComplete);
}

void UPraxsuiteSubsystem::DeleteRowById(const FString& Table, const FString& RowId,
										const FPraxWriteDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), 0);
		return;
	}

	Prax::FJsonValue Body;
	Prax::FPraxError Error;
	if (!Prax::Mutations::DeleteById(PraxConvert::ToStd(Table), PraxConvert::ToStd(RowId),
									Body, Error))
	{
		FailImmediately(OnComplete, Error, 0);
		return;
	}
	SendWrite(*Transport, Body, OnComplete);
}

void UPraxsuiteSubsystem::UpdateRowsWhereEquals(const FString& Table, const FString& Column,
												const FString& Value,
												const TMap<FString, FString>& NewValues,
												const FPraxWriteDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), 0);
		return;
	}

	Prax::FJsonValue Body;
	Prax::FPraxError Error;
	const std::vector<Prax::FJsonValue> Conditions = {
		Prax::Filters::Eq(PraxConvert::ToStd(Column), PraxConvert::ToJsonValue(Value))
	};
	if (!Prax::Mutations::Update(PraxConvert::ToStd(Table), PraxConvert::ToJsonObject(NewValues),
								 Conditions, Body, Error))
	{
		FailImmediately(OnComplete, Error, 0);
		return;
	}
	SendWrite(*Transport, Body, OnComplete);
}

void UPraxsuiteSubsystem::CallEndpoint(const FString& EndpointId, const FString& JsonBody,
									   const FPraxEndpointDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), FString());
		return;
	}
	if (EndpointId.IsEmpty())
	{
		FailImmediately(OnComplete, Prax::FPraxError::MakeValidation("NO_ENDPOINT_ID",
			"an endpoint id is required - it is the GUID from the API Gateway screen"), FString());
		return;
	}

	// An empty body means an empty object, not an empty payload: the gateway expects JSON.
	Prax::FJsonValue Body = Prax::FJsonValue::Object();
	if (!JsonBody.IsEmpty())
	{
		std::string ParseError;
		if (!Prax::FJsonValue::Parse(PraxConvert::ToStd(JsonBody), Body, ParseError))
		{
			// Caught here rather than sent: the gateway's rejection of malformed JSON says nothing
			// about which character was wrong, and this one does.
			FailImmediately(OnComplete, Prax::FPraxError::MakeValidation("INVALID_JSON_BODY",
				"the endpoint body is not valid JSON: " + ParseError), FString());
			return;
		}
	}

	const FPraxEndpointDelegate Delegate = OnComplete;
	Transport->Post(
		Transport->BuildUrl(FString::Printf(TEXT("endpoint/%s"), *EndpointId)),
		Body,
		// Never retried. An endpoint runs an automation, and an automation that got as far as writing
		// something has already written it - a retry runs it twice.
		/*bIsWrite=*/true,
		static_cast<float>(Transport->GetEndpointTimeoutSeconds()),
		[Delegate](int32 Status, const Prax::FJsonValue& Parsed, bool bParsed, const FString& Raw)
		{
			if (!bParsed)
			{
				const Prax::FPraxError Failure = Prax::FPraxError::FromUnparseableResponse(
					Status, PraxConvert::ToStd(Raw));
				Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), Raw);
				return;
			}

			Prax::FJsonValue Result;
			Prax::FPraxError Failure;
			if (!Prax::Envelope::ReadEndpoint(Status, Parsed, Result, Failure))
			{
				Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), FString());
				return;
			}
			// The automation's payload, exactly as it produced it. Nothing is unwrapped - see
			// Core/PraxEnvelope.h for the regression that comes from unwrapping here.
			Delegate.ExecuteIfBound(PraxConvert::MakeSuccess(),
									PraxConvert::ToUnreal(Result.ToString()));
		});
}

namespace
{
	/** Login and register differ only in their route, so the handling is shared. */
	void SendAuth(FPraxsuiteTransport& Transport, const FString& Action, const FString& Email,
				  const FString& Password, const FPraxAuthDelegate& OnComplete)
	{
		Prax::FJsonValue Body = Prax::FJsonValue::Object();
		Body.SetField("email", Prax::FJsonValue(PraxConvert::ToStd(Email)));
		Body.SetField("password", Prax::FJsonValue(PraxConvert::ToStd(Password)));

		FPraxsuiteTransport* TransportPtr = &Transport;
		const FPraxAuthDelegate Delegate = OnComplete;

		Transport.Post(Transport.BuildUrl(FString::Printf(TEXT("auth/%s"), *Action)), Body,
			// Never retried. Repeating a register creates a second account or trips an
			// already-registered error that reads like a failure of the first attempt.
			/*bIsWrite=*/true,
			static_cast<float>(Transport.GetTimeoutSeconds()),
			[TransportPtr, Delegate](int32 Status, const Prax::FJsonValue& Parsed, bool bParsed,
									 const FString& Raw)
			{
				if (!bParsed)
				{
					const Prax::FPraxError Failure = Prax::FPraxError::FromUnparseableResponse(
						Status, PraxConvert::ToStd(Raw));
					Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), FPraxSession{});
					return;
				}

				Prax::FJsonValue Data;
				Prax::FPraxError Failure;
				// The one route family whose payload is nested under .data.
				if (!Prax::Envelope::ReadAuth(Status, Parsed, Data, Failure))
				{
					Delegate.ExecuteIfBound(PraxConvert::ToResult(Failure), FPraxSession{});
					return;
				}

				const FString Token = PraxConvert::ToUnreal(Data.Field("accessToken").AsString());
				const FString UserId = PraxConvert::ToUnreal(Data.Field("userId").AsString());
				const FString Email = PraxConvert::ToUnreal(Data.Field("email").AsString());

				if (TransportPtr != nullptr)
				{
					TransportPtr->SetSession(UserId, Email, Token);
					Delegate.ExecuteIfBound(PraxConvert::MakeSuccess(), TransportPtr->GetSession());
					return;
				}
				Delegate.ExecuteIfBound(PraxConvert::MakeSuccess(), FPraxSession{});
			});
	}
}

void UPraxsuiteSubsystem::Login(const FString& Email, const FString& Password,
								const FPraxAuthDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), FPraxSession{});
		return;
	}
	SendAuth(*Transport, TEXT("login"), Email, Password, OnComplete);
}

void UPraxsuiteSubsystem::Register(const FString& Email, const FString& Password,
								   const FPraxAuthDelegate& OnComplete)
{
	if (!IsConfigured())
	{
		FailImmediately(OnComplete, NotConfigured(), FPraxSession{});
		return;
	}
	SendAuth(*Transport, TEXT("register"), Email, Password, OnComplete);
}

void UPraxsuiteSubsystem::Logout()
{
	if (Transport.IsValid())
	{
		Transport->ClearSession();
	}
}

FPraxSession UPraxsuiteSubsystem::GetSession() const
{
	return Transport.IsValid() ? Transport->GetSession() : FPraxSession{};
}

bool UPraxsuiteSubsystem::IsLoggedIn() const
{
	return Transport.IsValid() && Transport->IsLoggedIn();
}
