// PraxQL request building, for reads and writes.
//
// Portable C++17, no Unreal types - see Core/PraxJson.h for why.
//
// This is where validation lives, because this is where there is somewhere to report it. The filter
// helpers return conditions and have no error channel; these builders return a bool plus an
// FPraxError, and they refuse a bad request BEFORE it is sent.
//
// Two rules are worth stating up front because both are counter-intuitive.
//
// includeTotalCount SITS BESIDE query, NOT INSIDE IT. Putting it inside is silently ignored: the
// request succeeds, meta.total is simply absent, and any count derived from it reads as zero. No
// error anywhere.
//
// limit IS CLAMPED UP TO 1. There is no such thing as a zero-row request - asking for zero returns
// one row. Counting therefore means includeTotalCount plus a one-row fetch, never a zero-row fetch,
// and an SDK that "optimises" a count by asking for no rows gets a row back and may well count it.

#pragma once

#include <string>
#include <vector>

#include "Core/PraxError.h"
#include "Core/PraxJson.h"

namespace Prax
{
	/** Server-side limits an SDK must not assume away. */
	namespace QueryLimits
	{
		constexpr int64_t AbsoluteMaxLimit = 1000;
		constexpr int64_t DefaultLimit = 50;
		constexpr int64_t MinimumLimit = 1;
		constexpr size_t MaxSelectColumns = 100;
		constexpr size_t MaxWhereConditions = 50;
		constexpr int MaxConditionNestingDepth = 5;
	}

	/** Native columns the backend fills. Supplying one is rejected by the gateway. */
	namespace NativeColumns
	{
		const std::vector<std::string>& All();
		bool IsNative(const std::string& ColumnName);
	}

	/**
	 * Builds a read request.
	 *
	 * Nothing is validated until Build, so a caller can assemble a query in whatever order suits and
	 * still get one clear error rather than a cascade.
	 */
	class FQueryBuilder
	{
	public:
		explicit FQueryBuilder(std::string TableName);

		FQueryBuilder& Select(const std::vector<std::string>& Columns);
		FQueryBuilder& Where(FJsonValue Condition);
		FQueryBuilder& WhereEquals(const std::string& Field, FJsonValue Value);
		FQueryBuilder& OrderBy(const std::string& Field, bool bDescending = false);
		FQueryBuilder& GroupBy(const std::vector<std::string>& Columns);
		FQueryBuilder& Having(FJsonValue Condition);
		FQueryBuilder& Limit(int64_t In);
		FQueryBuilder& Offset(int64_t In);

		/** Asks for meta.total. Without this, a total is absent rather than zero. */
		FQueryBuilder& WithTotalCount(bool bEnabled = true);

		/** Produces the request body, or false and an error explaining the refusal. */
		bool Build(FJsonValue& OutBody, FPraxError& OutError) const;

		/**
		 * The limit that will actually be sent, after clamping.
		 *
		 * Exposed so a caller can see that asking for 0 sends 1, and asking for 5000 sends 1000,
		 * rather than discovering it from a row count that does not match the request.
		 */
		int64_t EffectiveLimit() const;

	private:
		std::string Table;
		std::vector<std::string> SelectColumns;
		std::vector<FJsonValue> WhereConditions;
		std::vector<FJsonValue> OrderByClauses;
		std::vector<std::string> GroupByColumns;
		std::vector<FJsonValue> HavingConditions;
		int64_t LimitValue = QueryLimits::DefaultLimit;
		int64_t OffsetValue = 0;
		bool bIncludeTotalCount = false;
	};

	/**
	 * Builds write requests.
	 *
	 * Update and Delete REQUIRE at least one condition and refuse without one. The refusal is
	 * synchronous - a returned false, right here - specifically so a caller cannot miss it. Reporting
	 * an unscoped-write guardrail through a future or a callback means a caller who does not check
	 * gets silence: no write, no error, no clue. For a guardrail whose entire job is preventing an
	 * accidental table-wide UPDATE, silence is the worst available outcome.
	 */
	namespace Mutations
	{
		bool Insert(const std::string& Table, const FJsonValue& Values,
					FJsonValue& OutBody, FPraxError& OutError);

		bool InsertMany(const std::string& Table, const std::vector<FJsonValue>& Rows,
						FJsonValue& OutBody, FPraxError& OutError);

		bool Update(const std::string& Table, const FJsonValue& Values,
					const std::vector<FJsonValue>& Conditions,
					FJsonValue& OutBody, FPraxError& OutError);

		bool Delete(const std::string& Table, const std::vector<FJsonValue>& Conditions,
					FJsonValue& OutBody, FPraxError& OutError);

		bool UpdateById(const std::string& Table, const std::string& RowId, const FJsonValue& Values,
						FJsonValue& OutBody, FPraxError& OutError);

		bool DeleteById(const std::string& Table, const std::string& RowId,
						FJsonValue& OutBody, FPraxError& OutError);
	}
}
