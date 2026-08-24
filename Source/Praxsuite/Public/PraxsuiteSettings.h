// Project Settings entry.
//
// Part of the Unreal layer - CI cannot compile this. See PraxsuiteTypes.h.
//
// Configuration lives here rather than in a Blueprint because a value hardcoded into a graph is a
// value somebody forgets to change before shipping. It also means the credential is one field in one
// place when it needs rotating, rather than wherever it was pasted.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "PraxsuiteSettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Praxsuite"))
class PRAXSUITE_API UPraxsuiteSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UPraxsuiteSettings();

	/** From your workspace's API Gateway screen. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	FString WorkspaceId;

	/**
	 * A pk_live_ publishable key.
	 *
	 * A packaged game is a program on somebody else's computer, so any string in it is readable with
	 * a hex editor. A secret key here is a published secret, and bClientSide (below, on by default)
	 * makes the SDK refuse one outright rather than trusting you to remember.
	 *
	 * Two things worth knowing even with a publishable key:
	 *
	 *   Every credential carries BOTH halves - there is no publishable-only credential - so whatever
	 *   table scopes you grant this key are reachable by anyone who has it. Scope narrowly.
	 *
	 *   /{workspaceId}/auth/config is unauthenticated, so the workspace id alone yields the
	 *   publishable key. The workspace id is not a secret, but it is not harmless to publish either.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	FString PublishableKey;

	/**
	 * The gateway host for the tier your workspace is on.
	 *
	 * REQUIRED, and not a formality. Praxsuite runs several independent tiers and a workspace exists
	 * on exactly one; pointing at the wrong one returns a plain 404 that reads exactly like a missing
	 * table. If everything 404s, check this before anything else.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection")
	FString GatewayHost;

	/**
	 * Refuse a secret key. On by default, and you almost certainly want it left on.
	 *
	 * The only case for turning it off is a build that never reaches a player: a dedicated server, or
	 * an editor tool. If a player can run it, this belongs on.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Security")
	bool bClientSide = true;

	/** Seconds before a request is abandoned. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (ClampMin = "1", ClampMax = "300"))
	int32 TimeoutSeconds = 30;

	/**
	 * Seconds before an ENDPOINT call is abandoned, which is deliberately much longer.
	 *
	 * A Sync endpoint holds the connection open while its automation runs, and configured
	 * syncTimeoutSeconds values of 30, 45, 60 and 90 all exist in practice. A 30-second client
	 * timeout would abandon an automation that was going to succeed - and for a write, abandoning is
	 * not the same as cancelling: the automation carries on and completes.
	 */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (ClampMin = "1", ClampMax = "600"))
	int32 EndpointTimeoutSeconds = 100;

	/** Attempts for a READ that fails transiently. Writes are never retried. */
	UPROPERTY(Config, EditAnywhere, Category = "Connection", meta = (ClampMin = "1", ClampMax = "10"))
	int32 MaxReadAttempts = 3;

	/** Log every request and response. Credentials and tokens are scrubbed regardless. */
	UPROPERTY(Config, EditAnywhere, Category = "Diagnostics")
	bool bVerboseLogging = false;

	static const UPraxsuiteSettings* Get();
};
