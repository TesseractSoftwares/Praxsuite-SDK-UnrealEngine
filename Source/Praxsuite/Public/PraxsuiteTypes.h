// Blueprint-facing types.
//
// THIS FILE IS PART OF THE UNREAL LAYER, which CI cannot compile - an Unreal plugin needs a licensed
// engine install of around 100 GB. So this layer is kept deliberately thin: transport and type
// conversion, no logic. Everything that decides anything lives under Core/, which is compiled and
// executed on every commit under two compilers and two sanitizers.
//
// If you are reading this to judge how much of the SDK is tested: the answer is the core, thoroughly,
// and this layer only by a human opening it in Unreal. The README says the same thing.

#pragma once

#include "CoreMinimal.h"
#include "PraxsuiteTypes.generated.h"

/**
 * Error codes, mirroring Prax::EPraxErrorCode for Blueprint.
 *
 * Duplicated rather than exposed directly because UENUM cannot wrap a plain C++ enum declared in a
 * non-UObject header, and pulling the engine into Core to avoid the duplication would cost the
 * testability the whole design rests on.
 *
 * Two things keep the lists in step, and neither is a core unit test - the core cannot see this type
 * at all, so it cannot assert anything about it. First, PraxConvert::ToBlueprintError switches over
 * every core code with no default, so adding one to the core produces an unhandled-case warning in
 * the Unreal build. Second, a CI step counts the enumerators in both files and fails if they
 * disagree - which is the check that runs where CI can actually reach.
 */
UENUM(BlueprintType)
enum class EPraxError : uint8
{
	None                UMETA(DisplayName = "None"),

	/** Transient. Back off and retry. */
	RateLimit           UMETA(DisplayName = "Rate Limit Exceeded"),

	/** NOT transient - the monthly allowance is gone. Retrying cannot fix it. */
	Quota               UMETA(DisplayName = "Quota Exceeded"),

	/** NOT transient - the data-transfer allowance is gone. */
	EgressLimit         UMETA(DisplayName = "Egress Limit Exceeded"),

	Unauthorized        UMETA(DisplayName = "Unauthorized"),

	/** A permissions problem, not a query problem. Rewording the query will not help. */
	Forbidden           UMETA(DisplayName = "Forbidden"),

	InvalidRequest      UMETA(DisplayName = "Invalid Request"),
	InvalidRefs         UMETA(DisplayName = "Invalid Refs"),

	/** Often the wrong gateway host rather than a missing row. See the README on tiers. */
	NotFound            UMETA(DisplayName = "Not Found"),

	/** Refused by the SDK before anything was sent. */
	Validation          UMETA(DisplayName = "Validation Failed"),

	Network             UMETA(DisplayName = "Network Failure"),
	Timeout             UMETA(DisplayName = "Timed Out"),
	ServerError         UMETA(DisplayName = "Server Error"),
	Unknown             UMETA(DisplayName = "Unknown"),
};

/** The outcome of a call. */
USTRUCT(BlueprintType)
struct PRAXSUITE_API FPraxResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	bool bSuccess = false;

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	EPraxError Error = EPraxError::None;

	/** The code as the server sent it. Useful when Error is Unknown. */
	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	FString ErrorCode;

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	FString Message;

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	int32 HttpStatus = 0;

	/**
	 * Whether retrying could plausibly succeed.
	 *
	 * Note this says nothing about whether retrying is SAFE. The SDK never retries a write, because
	 * a failed insert may well have been applied - retrying one is how a single purchase becomes two
	 * rows. This flag is about reads.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	bool bRetryable = false;
};

/**
 * One row.
 *
 * Blueprint gets fields as strings, which is a real limitation and worth being straight about: a
 * Blueprint graph has no clean way to hold a heterogeneous value, and inventing a variant type would
 * be more surface than it is worth. Use the typed helpers on UPraxsuiteFunctionLibrary to read
 * numbers and booleans, or use the C++ API for typed access to the underlying JSON.
 *
 * Nested objects and arrays arrive as their serialised JSON text rather than being flattened.
 * Flattening would invent field names that do not exist in the table, which is worse than handing
 * back something a caller can parse deliberately.
 */
USTRUCT(BlueprintType)
struct PRAXSUITE_API FPraxRow
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	TMap<FString, FString> Fields;

	/** True when the column exists in this row - which is not the same as being non-empty. */
	bool Has(const FString& Column) const { return Fields.Contains(Column); }
};

/** A page of rows plus its metadata. */
USTRUCT(BlueprintType)
struct PRAXSUITE_API FPraxPageResult
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	TArray<FPraxRow> Rows;

	/**
	 * The limit the SERVER applied, which may be lower than the one requested.
	 *
	 * A table scope can clamp it. Code that pages by assuming its own limit was honoured loops
	 * forever when that happens, so this is the value to page against.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	int32 Limit = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	int32 Offset = 0;

	/** Rows in this page. */
	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	int32 Count = 0;

	/**
	 * Total matching rows - meaningful ONLY when bHasTotal is true.
	 *
	 * Without asking for a count, this is 0 because there is nothing to report, not because nothing
	 * matched. Reading it without checking bHasTotal turns "nobody asked" into "zero rows", which is
	 * a wrong answer with no error attached.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	int32 Total = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	bool bHasTotal = false;
};

/** A session, returned by login and register. */
USTRUCT(BlueprintType)
struct PRAXSUITE_API FPraxSession
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	FString UserId;

	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	FString Email;

	/**
	 * Whether a session token is currently held.
	 *
	 * The token itself is deliberately NOT exposed to Blueprint. A token in a Blueprint variable
	 * ends up printed to the log by the next person debugging, and a player's log routinely ends up
	 * in a public bug report. The subsystem attaches it to requests itself.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Praxsuite")
	bool bHasToken = false;
};

DECLARE_DYNAMIC_DELEGATE_TwoParams(FPraxQueryDelegate, FPraxResult, Result, FPraxPageResult, Page);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FPraxWriteDelegate, FPraxResult, Result, int32, AffectedRows);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FPraxEndpointDelegate, FPraxResult, Result, FString, ResponseJson);
DECLARE_DYNAMIC_DELEGATE_TwoParams(FPraxAuthDelegate, FPraxResult, Result, FPraxSession, Session);
