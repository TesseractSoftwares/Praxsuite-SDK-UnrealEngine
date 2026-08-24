// The SDK's entry point, as a GameInstance subsystem.
//
// Part of the Unreal layer - CI cannot compile this. See PraxsuiteTypes.h.
//
// A GameInstanceSubsystem rather than a singleton or an Actor Component: it exists for the lifetime
// of the game instance, survives level travel, is reachable from any Blueprint without wiring a
// reference, and is torn down deterministically. A session that vanished on level load would be the
// wrong lifetime for a player login.
//
// EVERYTHING HERE IS ASYNCHRONOUS, and the delegate is always called exactly once - including on a
// synchronous refusal. That last part matters: a guardrail that refuses a bad request before sending
// it must still call back, or a Blueprint waits forever on a pin that will never fire. The one thing
// worse than an unscoped delete is a game that silently hangs instead of telling you why it did not
// happen.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "PraxsuiteTypes.h"

#include "PraxsuiteSubsystem.generated.h"

class FPraxsuiteTransport;

UCLASS()
class PRAXSUITE_API UPraxsuiteSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Overrides Project Settings at runtime.
	 *
	 * For a game that talks to different workspaces per environment, or one that fetches its
	 * configuration at launch. Not needed if Project Settings are filled in.
	 *
	 * Returns a failure - synchronously - if the credential is a secret key while bClientSide is set.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite")
	FPraxResult Configure(const FString& WorkspaceId, const FString& Credential,
						  const FString& GatewayHost, bool bClientSide = true);

	/** True once a usable workspace id, credential and host are all present. */
	UFUNCTION(BlueprintPure, Category = "Praxsuite")
	bool IsConfigured() const;

	// ── reads ────────────────────────────────────────────────────────────────

	/**
	 * Reads rows from a table.
	 *
	 * OrderByDescending applies to OrderByColumn. Leave OrderByColumn empty for the table's own
	 * order. Limit is clamped to at least 1 and at most 1000 by the gateway - a limit of 0 does NOT
	 * mean "no rows", it means one row, so do not use it to count.
	 *
	 * Set bIncludeTotalCount to populate Page.Total. Without it, Page.bHasTotal is false and
	 * Page.Total is 0 because nothing was asked for - not because nothing matched.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Data",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void QueryTable(const FString& Table, const TArray<FString>& SelectColumns,
					const FString& OrderByColumn, bool bOrderByDescending,
					int32 Limit, int32 Offset, bool bIncludeTotalCount,
					const FPraxQueryDelegate& OnComplete);

	/**
	 * Reads rows where a column equals a value.
	 *
	 * The common case, without building a filter. For anything more involved, use the C++ API with
	 * Prax::FQueryBuilder and the full set of filters.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Data",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void QueryTableWhereEquals(const FString& Table, const FString& Column, const FString& Value,
							   int32 Limit, const FPraxQueryDelegate& OnComplete);

	// ── writes ───────────────────────────────────────────────────────────────

	/**
	 * Inserts a row.
	 *
	 * Do NOT include an ownership column. If the table is configured for per-player isolation, the
	 * server stamps ownership from the caller's verified token and REJECTS a client that tries to set
	 * it - that rejection is the anti-tamper guarantee, not an inconvenience. See the README.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Data",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void InsertRow(const FString& Table, const TMap<FString, FString>& Values,
				   const FPraxWriteDelegate& OnComplete);

	/** Updates one row by its ID. */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Data",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void UpdateRowById(const FString& Table, const FString& RowId,
					   const TMap<FString, FString>& Values, const FPraxWriteDelegate& OnComplete);

	/** Deletes one row by its ID. */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Data",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void DeleteRowById(const FString& Table, const FString& RowId,
					   const FPraxWriteDelegate& OnComplete);

	/**
	 * Updates every row where a column equals a value.
	 *
	 * There is deliberately no "update everything" node. An unscoped update rewrites every row in
	 * the table and there is no undo, so the SDK refuses one - and rather than offer a node that
	 * always fails, this one requires the scope as arguments.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Data",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void UpdateRowsWhereEquals(const FString& Table, const FString& Column, const FString& Value,
							   const TMap<FString, FString>& NewValues,
							   const FPraxWriteDelegate& OnComplete);

	// ── automations ──────────────────────────────────────────────────────────

	/**
	 * Calls a server-side automation and returns its response as JSON text.
	 *
	 * EndpointId is the endpoint's GUID from the workspace's API Gateway screen, not a readable name.
	 *
	 * The response is the automation's own payload, unchanged - nothing is unwrapped. It comes back
	 * as a string because an automation can return any shape at all; parse what you expect from it.
	 *
	 * IMPORTANT, and it surprises people: AN ENDPOINT DOES NOT AUTHENTICATE ITS CALLER. A POST with
	 * no credential runs the automation. That follows from what an endpoint is - a webhook receiver,
	 * and Stripe cannot hold your workspace credential - but it means putting logic here makes it
	 * server-EXECUTED, not automatically server-AUTHORITATIVE. The authority comes from the endpoint
	 * verifying who called: a signature secret, or the automation checking a verified claim from the
	 * session token this SDK attaches. Design accordingly, especially for anything a cheater profits
	 * from.
	 */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Automations",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void CallEndpoint(const FString& EndpointId, const FString& JsonBody,
					  const FPraxEndpointDelegate& OnComplete);

	// ── players ──────────────────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Auth",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void Login(const FString& Email, const FString& Password, const FPraxAuthDelegate& OnComplete);

	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Auth",
			  meta = (AutoCreateRefTerm = "OnComplete"))
	void Register(const FString& Email, const FString& Password, const FPraxAuthDelegate& OnComplete);

	/** Clears the held session. Local and immediate. */
	UFUNCTION(BlueprintCallable, Category = "Praxsuite|Auth")
	void Logout();

	UFUNCTION(BlueprintPure, Category = "Praxsuite|Auth")
	FPraxSession GetSession() const;

	UFUNCTION(BlueprintPure, Category = "Praxsuite|Auth")
	bool IsLoggedIn() const;

private:
	/** Holds the transport and the configuration. Opaque so the header pulls in no HTTP types. */
	TUniquePtr<FPraxsuiteTransport> Transport;
};
