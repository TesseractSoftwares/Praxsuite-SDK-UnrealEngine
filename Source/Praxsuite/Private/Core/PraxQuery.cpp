#include "Core/PraxQuery.h"

#include <algorithm>

#include "Core/PraxFilters.h"

namespace Prax
{
	namespace
	{
		/** The single ref alias every request uses. One table per request, so one alias suffices. */
		constexpr const char* RefAlias = "t";

		FPraxError Reject(const char* Code, std::string Message)
		{
			return FPraxError::MakeValidation(Code, std::move(Message));
		}

		/**
		 * Validates one condition, recursively for and/or groups.
		 *
		 * Depth is bounded because the gateway bounds it, and exceeding it is a 400 that names no
		 * field - far easier to understand here.
		 */
		bool ValidateCondition(const FJsonValue& Condition, int Depth, FPraxError& OutError)
		{
			if (Depth > QueryLimits::MaxConditionNestingDepth)
			{
				OutError = Reject("CONDITION_TOO_DEEP",
					"conditions are nested deeper than the gateway's limit of "
					+ std::to_string(QueryLimits::MaxConditionNestingDepth));
				return false;
			}

			if (!Condition.IsObject())
			{
				OutError = Reject("INVALID_CONDITION", "a condition must be an object");
				return false;
			}

			// A group: {"or":[...]} or {"and":[...]}.
			for (const char* GroupKey : {"or", "and"})
			{
				if (Condition.HasField(GroupKey))
				{
					const FJsonValue& Nested = Condition.Field(GroupKey);
					if (!Nested.IsArray() || Nested.AsArray().empty())
					{
						OutError = Reject("EMPTY_CONDITION_GROUP",
							std::string("an empty '") + GroupKey + "' group matches nothing");
						return false;
					}
					for (const FJsonValue& Child : Nested.AsArray())
					{
						if (!ValidateCondition(Child, Depth + 1, OutError))
						{
							return false;
						}
					}
					return true;
				}
			}

			const std::string Op = Condition.Field("op").AsString();
			if (Op.empty())
			{
				OutError = Reject("INVALID_CONDITION", "a condition has no operator");
				return false;
			}

			// Only the thirteen. A friendlier name would otherwise reach the gateway and 400 on a
			// player's machine rather than here.
			if (!Filters::IsSupportedOperator(Op))
			{
				OutError = Reject("UNSUPPORTED_OPERATOR",
					"the gateway does not implement the operator '" + Op + "'");
				return false;
			}

			if (Condition.Field("field").AsString().empty())
			{
				OutError = Reject("INVALID_CONDITION",
					"a condition using '" + Op + "' has no field");
				return false;
			}

			// An empty IN is valid JSON that matches nothing. Left alone it returns an empty page
			// that reads exactly like "no rows matched" - so it is refused instead.
			if (Op == "in")
			{
				const FJsonValue& Values = Condition.Field("value");
				if (!Values.IsArray())
				{
					OutError = Reject("INVALID_CONDITION", "'in' requires a list of values");
					return false;
				}
				if (Values.AsArray().empty())
				{
					OutError = Reject("EMPTY_IN_LIST",
						"an 'in' filter with no values matches nothing - it would return an empty "
						"page that looks like a legitimate no-results answer");
					return false;
				}
			}

			if (Op == "between")
			{
				const FJsonValue& Bounds = Condition.Field("value");
				if (!Bounds.IsArray() || Bounds.AsArray().size() != 2)
				{
					OutError = Reject("INVALID_CONDITION", "'between' requires exactly two bounds");
					return false;
				}
			}

			return true;
		}

		bool ValidateConditions(const std::vector<FJsonValue>& Conditions, const char* What,
								FPraxError& OutError)
		{
			if (Conditions.size() > QueryLimits::MaxWhereConditions)
			{
				OutError = Reject("TOO_MANY_CONDITIONS",
					std::string("more than ") + std::to_string(QueryLimits::MaxWhereConditions)
					+ " " + What + " conditions");
				return false;
			}
			for (const FJsonValue& Condition : Conditions)
			{
				if (!ValidateCondition(Condition, 1, OutError))
				{
					return false;
				}
			}
			return true;
		}

		/** Wraps a table name into the refs block every request carries. */
		FJsonValue MakeRefs(const std::string& Table)
		{
			FJsonValue Refs = FJsonValue::Object();
			Refs.SetField(RefAlias, FJsonValue(Table));
			return Refs;
		}

		FJsonValue ToArray(const std::vector<FJsonValue>& Items)
		{
			FJsonValue Out = FJsonValue::Array();
			for (const FJsonValue& Item : Items)
			{
				Out.Push(Item);
			}
			return Out;
		}

		FJsonValue ToStringArray(const std::vector<std::string>& Items)
		{
			FJsonValue Out = FJsonValue::Array();
			for (const std::string& Item : Items)
			{
				Out.Push(FJsonValue(Item));
			}
			return Out;
		}

		/**
		 * Checks a values object for columns the client is not allowed to set.
		 *
		 * Also checks it IS an object. A map that serialised as an array of key/value pairs is
		 * rejected by the gateway for every mutation, and catching it here names the problem.
		 */
		bool ValidateValues(const FJsonValue& Values, FPraxError& OutError)
		{
			if (!Values.IsObject())
			{
				OutError = Reject("INVALID_VALUES",
					"values must be a JSON object of column names to values, not an array");
				return false;
			}
			if (Values.AsObject().empty())
			{
				OutError = Reject("EMPTY_VALUES", "a write with no values would do nothing");
				return false;
			}
			for (const FJsonMember& Member : Values.AsObject())
			{
				if (NativeColumns::IsNative(Member.Key))
				{
					OutError = Reject("NATIVE_COLUMN",
						"'" + Member.Key + "' is filled by the backend and cannot be set - the "
						"gateway rejects a request that supplies it");
					return false;
				}
			}
			return true;
		}

		bool BuildMutation(const std::string& Table, const char* MutationType,
						   FJsonValue Mutation, FJsonValue& OutBody, FPraxError& OutError)
		{
			if (Table.empty())
			{
				OutError = Reject("NO_TABLE", "a mutation needs a table name");
				return false;
			}
			Mutation.SetField("type", FJsonValue(MutationType));
			Mutation.SetField("table", FJsonValue(RefAlias));

			OutBody = FJsonValue::Object();
			OutBody.SetField("refs", MakeRefs(Table));
			OutBody.SetField("mutation", std::move(Mutation));
			OutError = FPraxError{};
			return true;
		}
	}

	namespace NativeColumns
	{
		const std::vector<std::string>& All()
		{
			static const std::vector<std::string> Columns = {
				"ID", "CREATEDDATE", "CREATEDBY", "UPDATEDDATE", "UPDATEDBY", "POSITION",
			};
			return Columns;
		}

		bool IsNative(const std::string& ColumnName)
		{
			// Case-insensitive: the backend's names are upper case but a caller may well write "id".
			std::string Upper;
			Upper.reserve(ColumnName.size());
			for (const char C : ColumnName)
			{
				Upper.push_back((C >= 'a' && C <= 'z') ? static_cast<char>(C - 'a' + 'A') : C);
			}
			const std::vector<std::string>& Columns = All();
			return std::find(Columns.begin(), Columns.end(), Upper) != Columns.end();
		}
	}

	FQueryBuilder::FQueryBuilder(std::string TableName) : Table(std::move(TableName)) {}

	FQueryBuilder& FQueryBuilder::Select(const std::vector<std::string>& Columns)
	{
		SelectColumns.insert(SelectColumns.end(), Columns.begin(), Columns.end());
		return *this;
	}

	FQueryBuilder& FQueryBuilder::Where(FJsonValue Condition)
	{
		WhereConditions.push_back(std::move(Condition));
		return *this;
	}

	FQueryBuilder& FQueryBuilder::WhereEquals(const std::string& Field, FJsonValue Value)
	{
		return Where(Filters::Eq(Field, std::move(Value)));
	}

	FQueryBuilder& FQueryBuilder::OrderBy(const std::string& Field, bool bDescending)
	{
		FJsonValue Clause = FJsonValue::Object();
		Clause.SetField("field", FJsonValue(Field));
		Clause.SetField("dir", FJsonValue(bDescending ? "desc" : "asc"));
		OrderByClauses.push_back(std::move(Clause));
		return *this;
	}

	FQueryBuilder& FQueryBuilder::GroupBy(const std::vector<std::string>& Columns)
	{
		GroupByColumns.insert(GroupByColumns.end(), Columns.begin(), Columns.end());
		return *this;
	}

	FQueryBuilder& FQueryBuilder::Having(FJsonValue Condition)
	{
		HavingConditions.push_back(std::move(Condition));
		return *this;
	}

	FQueryBuilder& FQueryBuilder::Limit(int64_t In)
	{
		LimitValue = In;
		return *this;
	}

	FQueryBuilder& FQueryBuilder::Offset(int64_t In)
	{
		OffsetValue = In;
		return *this;
	}

	FQueryBuilder& FQueryBuilder::WithTotalCount(bool bEnabled)
	{
		bIncludeTotalCount = bEnabled;
		return *this;
	}

	int64_t FQueryBuilder::EffectiveLimit() const
	{
		// Clamped UP to 1. There is no zero-row request: asking for zero returns one row. So a
		// count is includeTotalCount plus a one-row fetch, never a zero-row fetch - an SDK that
		// "optimises" a count by asking for no rows gets a row back and may well count it.
		if (LimitValue < QueryLimits::MinimumLimit)
		{
			return QueryLimits::MinimumLimit;
		}
		if (LimitValue > QueryLimits::AbsoluteMaxLimit)
		{
			return QueryLimits::AbsoluteMaxLimit;
		}
		return LimitValue;
	}

	bool FQueryBuilder::Build(FJsonValue& OutBody, FPraxError& OutError) const
	{
		if (Table.empty())
		{
			OutError = Reject("NO_TABLE", "a query needs a table name");
			return false;
		}
		if (SelectColumns.size() > QueryLimits::MaxSelectColumns)
		{
			OutError = Reject("TOO_MANY_COLUMNS",
				"more than " + std::to_string(QueryLimits::MaxSelectColumns) + " selected columns");
			return false;
		}
		if (OffsetValue < 0)
		{
			OutError = Reject("NEGATIVE_OFFSET", "offset cannot be negative");
			return false;
		}
		if (!ValidateConditions(WhereConditions, "where", OutError))
		{
			return false;
		}
		if (!ValidateConditions(HavingConditions, "having", OutError))
		{
			return false;
		}

		FJsonValue Query = FJsonValue::Object();
		Query.SetField("from", FJsonValue(RefAlias));

		if (!SelectColumns.empty())
		{
			Query.SetField("select", ToStringArray(SelectColumns));
		}
		if (!WhereConditions.empty())
		{
			Query.SetField("where", ToArray(WhereConditions));
		}
		if (!OrderByClauses.empty())
		{
			Query.SetField("orderBy", ToArray(OrderByClauses));
		}
		if (!GroupByColumns.empty())
		{
			Query.SetField("groupBy", ToStringArray(GroupByColumns));
		}
		if (!HavingConditions.empty())
		{
			Query.SetField("having", ToArray(HavingConditions));
		}

		Query.SetField("limit", FJsonValue(EffectiveLimit()));
		Query.SetField("offset", FJsonValue(OffsetValue));

		OutBody = FJsonValue::Object();
		OutBody.SetField("refs", MakeRefs(Table));
		OutBody.SetField("query", std::move(Query));

		// BESIDE query, not inside it. Inside, it is silently ignored: the request succeeds,
		// meta.total is absent, and a count derived from it reads as zero with no error anywhere.
		if (bIncludeTotalCount)
		{
			OutBody.SetField("includeTotalCount", FJsonValue(true));
		}

		OutError = FPraxError{};
		return true;
	}

	namespace Mutations
	{
		bool Insert(const std::string& Table, const FJsonValue& Values,
					FJsonValue& OutBody, FPraxError& OutError)
		{
			if (!ValidateValues(Values, OutError))
			{
				return false;
			}
			FJsonValue Mutation = FJsonValue::Object();
			Mutation.SetField("values", Values);
			return BuildMutation(Table, "insert", std::move(Mutation), OutBody, OutError);
		}

		bool InsertMany(const std::string& Table, const std::vector<FJsonValue>& Rows,
						FJsonValue& OutBody, FPraxError& OutError)
		{
			if (Rows.empty())
			{
				OutError = Reject("EMPTY_INSERT", "insertMany was given no rows");
				return false;
			}
			for (const FJsonValue& Row : Rows)
			{
				if (!ValidateValues(Row, OutError))
				{
					return false;
				}
			}
			FJsonValue Mutation = FJsonValue::Object();
			Mutation.SetField("values", ToArray(Rows));
			return BuildMutation(Table, "insert", std::move(Mutation), OutBody, OutError);
		}

		bool Update(const std::string& Table, const FJsonValue& Values,
					const std::vector<FJsonValue>& Conditions,
					FJsonValue& OutBody, FPraxError& OutError)
		{
			// The guardrail, refused synchronously. An unscoped UPDATE rewrites every row in the
			// table, and there is no undo.
			if (Conditions.empty())
			{
				OutError = Reject("UNSCOPED_MUTATION",
					"an update with no where conditions would rewrite every row in the table. "
					"Pass at least one condition, or use UpdateById.");
				return false;
			}
			if (!ValidateValues(Values, OutError))
			{
				return false;
			}
			if (!ValidateConditions(Conditions, "where", OutError))
			{
				return false;
			}

			FJsonValue Mutation = FJsonValue::Object();
			Mutation.SetField("set", Values);
			Mutation.SetField("where", ToArray(Conditions));
			return BuildMutation(Table, "update", std::move(Mutation), OutBody, OutError);
		}

		bool Delete(const std::string& Table, const std::vector<FJsonValue>& Conditions,
					FJsonValue& OutBody, FPraxError& OutError)
		{
			if (Conditions.empty())
			{
				OutError = Reject("UNSCOPED_MUTATION",
					"a delete with no where conditions would remove every row in the table. "
					"Pass at least one condition, or use DeleteById.");
				return false;
			}
			if (!ValidateConditions(Conditions, "where", OutError))
			{
				return false;
			}

			FJsonValue Mutation = FJsonValue::Object();
			Mutation.SetField("where", ToArray(Conditions));
			return BuildMutation(Table, "delete", std::move(Mutation), OutBody, OutError);
		}

		bool UpdateById(const std::string& Table, const std::string& RowId, const FJsonValue& Values,
						FJsonValue& OutBody, FPraxError& OutError)
		{
			if (RowId.empty())
			{
				// An empty id would build an eq condition against "", which matches nothing - or,
				// worse, whatever row has an empty key. Refusing is the only safe reading.
				OutError = Reject("NO_ROW_ID", "updateById was given an empty row id");
				return false;
			}
			return Update(Table, Values, {Filters::Eq("ID", FJsonValue(RowId))}, OutBody, OutError);
		}

		bool DeleteById(const std::string& Table, const std::string& RowId,
						FJsonValue& OutBody, FPraxError& OutError)
		{
			if (RowId.empty())
			{
				OutError = Reject("NO_ROW_ID", "deleteById was given an empty row id");
				return false;
			}
			return Delete(Table, {Filters::Eq("ID", FJsonValue(RowId))}, OutBody, OutError);
		}
	}
}
