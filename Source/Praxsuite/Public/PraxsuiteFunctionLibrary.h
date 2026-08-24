// Blueprint helpers for reading rows.
//
// Part of the Unreal layer - CI cannot compile this. See PraxsuiteTypes.h.
//
// A row arrives with string values because a Blueprint map cannot hold a heterogeneous type. These
// convert on read. They are deliberately total - a missing column or an unparseable value returns
// the supplied default rather than a zero that could equally be a real zero, so a Blueprint can tell
// "absent" from "0" if it cares to.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "PraxsuiteTypes.h"

#include "PraxsuiteFunctionLibrary.generated.h"

UCLASS()
class PRAXSUITE_API UPraxsuiteFunctionLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** True when the column is present in the row, whether or not its value is empty. */
	UFUNCTION(BlueprintPure, Category = "Praxsuite|Row")
	static bool HasColumn(const FPraxRow& Row, const FString& Column);

	UFUNCTION(BlueprintPure, Category = "Praxsuite|Row")
	static FString GetString(const FPraxRow& Row, const FString& Column,
							 const FString& Default = TEXT(""));

	UFUNCTION(BlueprintPure, Category = "Praxsuite|Row")
	static int32 GetInt(const FPraxRow& Row, const FString& Column, int32 Default = 0);

	UFUNCTION(BlueprintPure, Category = "Praxsuite|Row")
	static float GetFloat(const FPraxRow& Row, const FString& Column, float Default = 0.0f);

	UFUNCTION(BlueprintPure, Category = "Praxsuite|Row")
	static bool GetBool(const FPraxRow& Row, const FString& Column, bool Default = false);

	/**
	 * The row's ID, which is what UpdateRowById and DeleteRowById take.
	 *
	 * Present in every row unless the query's select list excluded it - which is a common way to
	 * end up unable to update a row you just read.
	 */
	UFUNCTION(BlueprintPure, Category = "Praxsuite|Row")
	static FString GetRowId(const FPraxRow& Row);

	/** Every column name in the row, for iterating a table whose shape is not known ahead of time. */
	UFUNCTION(BlueprintPure, Category = "Praxsuite|Row")
	static TArray<FString> GetColumnNames(const FPraxRow& Row);

	/**
	 * A one-line description of a failure, suitable for a log or a debug overlay.
	 *
	 * Not for showing to a player: it names codes and HTTP statuses. Branch on Result.Error and write
	 * your own message for anything a player sees.
	 */
	UFUNCTION(BlueprintPure, Category = "Praxsuite")
	static FString DescribeResult(const FPraxResult& Result);

	/**
	 * Whether this failure is worth retrying.
	 *
	 * Reads only. The SDK never retries a write, because a failed insert may already have been
	 * applied - see the README.
	 */
	UFUNCTION(BlueprintPure, Category = "Praxsuite")
	static bool ShouldRetry(const FPraxResult& Result);
};
