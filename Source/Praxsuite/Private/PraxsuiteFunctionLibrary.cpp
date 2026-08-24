#include "PraxsuiteFunctionLibrary.h"

bool UPraxsuiteFunctionLibrary::HasColumn(const FPraxRow& Row, const FString& Column)
{
	return Row.Fields.Contains(Column);
}

FString UPraxsuiteFunctionLibrary::GetString(const FPraxRow& Row, const FString& Column,
											const FString& Default)
{
	const FString* Found = Row.Fields.Find(Column);
	return Found != nullptr ? *Found : Default;
}

int32 UPraxsuiteFunctionLibrary::GetInt(const FPraxRow& Row, const FString& Column, int32 Default)
{
	const FString* Found = Row.Fields.Find(Column);
	if (Found == nullptr || Found->IsEmpty())
	{
		// The default rather than 0, so a Blueprint can distinguish an absent or null column from a
		// column whose value really is zero. A score of 0 and no score at all mean different things.
		return Default;
	}
	return FCString::Atoi(**Found);
}

float UPraxsuiteFunctionLibrary::GetFloat(const FPraxRow& Row, const FString& Column, float Default)
{
	const FString* Found = Row.Fields.Find(Column);
	if (Found == nullptr || Found->IsEmpty())
	{
		return Default;
	}
	return static_cast<float>(FCString::Atod(**Found));
}

bool UPraxsuiteFunctionLibrary::GetBool(const FPraxRow& Row, const FString& Column, bool Default)
{
	const FString* Found = Row.Fields.Find(Column);
	if (Found == nullptr || Found->IsEmpty())
	{
		return Default;
	}
	// The JSON codec writes booleans as true/false, but a Bool column read back through a text path
	// can arrive in several spellings, so all the plausible ones are accepted rather than only the
	// one this SDK happens to emit.
	if (Found->Equals(TEXT("true"), ESearchCase::IgnoreCase)) { return true; }
	if (Found->Equals(TEXT("false"), ESearchCase::IgnoreCase)) { return false; }
	if (Found->Equals(TEXT("1"))) { return true; }
	if (Found->Equals(TEXT("0"))) { return false; }
	return Default;
}

FString UPraxsuiteFunctionLibrary::GetRowId(const FPraxRow& Row)
{
	// Both spellings, because which one a row carries depends on how it was selected.
	if (const FString* Upper = Row.Fields.Find(TEXT("ID")))
	{
		return *Upper;
	}
	if (const FString* Mixed = Row.Fields.Find(TEXT("Id")))
	{
		return *Mixed;
	}
	return FString();
}

TArray<FString> UPraxsuiteFunctionLibrary::GetColumnNames(const FPraxRow& Row)
{
	TArray<FString> Names;
	Row.Fields.GetKeys(Names);
	return Names;
}

FString UPraxsuiteFunctionLibrary::DescribeResult(const FPraxResult& Result)
{
	if (Result.bSuccess)
	{
		return TEXT("ok");
	}
	return FString::Printf(TEXT("%s (HTTP %d): %s"), *Result.ErrorCode, Result.HttpStatus,
						   *Result.Message);
}

bool UPraxsuiteFunctionLibrary::ShouldRetry(const FPraxResult& Result)
{
	return !Result.bSuccess && Result.bRetryable;
}
