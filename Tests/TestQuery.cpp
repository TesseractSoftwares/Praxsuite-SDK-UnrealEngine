// Filters, query building, and the write guardrails.
//
// The two rules with the most consequences here are both counter-intuitive, and both fail with no
// error at all if got wrong:
//
//   includeTotalCount belongs BESIDE query, not inside it. Inside, it is ignored - the request
//   succeeds, meta.total is absent, and a count derived from it reads as zero.
//
//   limit is clamped UP to 1. A zero-row request does not exist, so an SDK that "optimises" a count
//   by asking for no rows gets a row back and may well count it.

#include "PraxTestHarness.h"

#include "Core/PraxFilters.h"
#include "Core/PraxJson.h"
#include "Core/PraxQuery.h"

using Prax::FJsonValue;
using Prax::FPraxError;
using Prax::FQueryBuilder;
namespace Filters = Prax::Filters;
namespace Mutations = Prax::Mutations;

namespace
{
	/** Builds and returns the serialised body, or the refusal reason prefixed so it is obvious. */
	std::string BuildOrReason(const FQueryBuilder& Builder)
	{
		FJsonValue Body;
		FPraxError Error;
		if (!Builder.Build(Body, Error))
		{
			return "REFUSED:" + Error.ServerCode;
		}
		return Body.ToString();
	}

	std::string OpOf(const FJsonValue& Condition) { return Condition.Field("op").AsString(); }
}

void TestFilters()
{
	PraxTest::Section("Filters - exactly thirteen operators exist");
	{
		PRAX_EQ(static_cast<long long>(Filters::SupportedOperators().size()), 13LL,
				"thirteen operators, no more");

		for (const char* Op : {"eq", "neq", "gt", "gte", "lt", "lte", "like", "ilike",
							   "in", "is", "between", "contains", "textsearch"})
		{
			PRAX_CHECK(Filters::IsSupportedOperator(Op), Op);
		}

		// The plausible names an SDK author reaches for. Offering any of these as an OPERATOR moves
		// the failure to a player's machine, where it arrives as a 400 with no context. A sibling
		// SDK exposed five of them and every call using one fails.
		for (const char* Op : {"notIn", "isNull", "isNotNull", "startsWith", "endsWith", "nin",
							   "neq_null", "regex", "not", "exists"})
		{
			PRAX_CHECK(!Filters::IsSupportedOperator(Op),
					   (std::string("not an operator: ") + Op).c_str());
		}
	}

	PraxTest::Section("Filters - the friendly names compile down to real operators");
	{
		// The helpers are named for readability but must emit one of the thirteen.
		PRAX_STR_EQ(OpOf(Filters::StartsWith("Name", "Ari")), "like", "startsWith becomes like");
		PRAX_STR_EQ(Filters::StartsWith("Name", "Ari").Field("value").AsString(), "Ari%",
					"with the wildcard appended");

		PRAX_STR_EQ(OpOf(Filters::EndsWith("Name", "son")), "like", "endsWith becomes like");
		PRAX_STR_EQ(Filters::EndsWith("Name", "son").Field("value").AsString(), "%son",
					"with the wildcard prepended");

		// `is` tests only for null, so there is no isNull operator to offer.
		PRAX_STR_EQ(OpOf(Filters::IsNull("Owner")), "is", "isNull becomes is");
		PRAX_CHECK(Filters::IsNull("Owner").Field("value").IsNull(), "with a null value");

		PRAX_STR_EQ(OpOf(Filters::IsNotNull("Owner")), "neq", "isNotNull becomes neq");
		PRAX_CHECK(Filters::IsNotNull("Owner").Field("value").IsNull(), "with a null value");

		// Every helper, checked against the supported set - so a new one cannot be added with an
		// invented operator name.
		const std::vector<FJsonValue> Every = {
			Filters::Eq("a", FJsonValue(1)),      Filters::Neq("a", FJsonValue(1)),
			Filters::Gt("a", FJsonValue(1)),      Filters::Gte("a", FJsonValue(1)),
			Filters::Lt("a", FJsonValue(1)),      Filters::Lte("a", FJsonValue(1)),
			Filters::Like("a", "x%"),             Filters::ILike("a", "x%"),
			Filters::Contains("a", "x"),          Filters::TextSearch("a", "x"),
			Filters::StartsWith("a", "x"),        Filters::EndsWith("a", "x"),
			Filters::IsNull("a"),                 Filters::IsNotNull("a"),
			Filters::In("a", {FJsonValue(1)}),    Filters::Between("a", FJsonValue(1), FJsonValue(2)),
		};
		for (const FJsonValue& Condition : Every)
		{
			PRAX_CHECK(Filters::IsSupportedOperator(OpOf(Condition)),
					   ("every helper emits a real operator: " + OpOf(Condition)).c_str());
		}
	}

	PraxTest::Section("Filters - shapes");
	{
		PRAX_STR_EQ(Filters::Eq("Score", FJsonValue(100)).ToString(),
					"{\"field\":\"Score\",\"op\":\"eq\",\"value\":100}", "eq shape");

		PRAX_STR_EQ(Filters::Between("Score", FJsonValue(1), FJsonValue(9)).Field("value").ToString(),
					"[1,9]", "between carries two bounds as an array");

		PRAX_STR_EQ(Filters::In("Level", {FJsonValue(1), FJsonValue(2)}).Field("value").ToString(),
					"[1,2]", "in carries a list");

		// Groups nest under or/and rather than carrying a field.
		const FJsonValue Any = Filters::AnyOf({Filters::Eq("a", FJsonValue(1)),
											   Filters::Eq("b", FJsonValue(2))});
		PRAX_CHECK(Any.HasField("or"), "anyOf nests under 'or'");
		PRAX_EQ(static_cast<long long>(Any.Field("or").AsArray().size()), 2LL, "with both children");

		const FJsonValue All = Filters::AllOf({Filters::Eq("a", FJsonValue(1))});
		PRAX_CHECK(All.HasField("and"), "allOf nests under 'and'");
	}

	PraxTest::Section("Filters - LIKE wildcards in a player-supplied value");
	{
		// A player searching for "100%" otherwise matches every row beginning "100".
		PRAX_STR_EQ(Filters::EscapeLikeValue("100%"), "100\\%", "percent is escaped");
		PRAX_STR_EQ(Filters::EscapeLikeValue("a_b"), "a\\_b", "underscore is escaped");
		PRAX_STR_EQ(Filters::EscapeLikeValue("back\\slash"), "back\\\\slash", "backslash is escaped");
		PRAX_STR_EQ(Filters::EscapeLikeValue("ordinary"), "ordinary", "ordinary text is untouched");

		// StartsWith deliberately does NOT escape: a caller writing "A%B" may mean that wildcard.
		PRAX_STR_EQ(Filters::StartsWith("Name", "A%B").Field("value").AsString(), "A%B%",
					"startsWith leaves the caller's wildcards alone");
	}
}

void TestQueryBuilder()
{
	PraxTest::Section("Query - includeTotalCount sits BESIDE query, not inside it");
	{
		FJsonValue Body;
		FPraxError Error;
		PRAX_CHECK(FQueryBuilder("Scores").WithTotalCount().Build(Body, Error), "builds");

		// Inside the query object it is silently ignored: the request succeeds, meta.total is
		// absent, and any count derived from it reads as zero with no error anywhere.
		PRAX_CHECK(Body.HasField("includeTotalCount"), "it is a top-level field");
		PRAX_CHECK(Body.Field("includeTotalCount").AsBool(), "and set to true");
		PRAX_CHECK(!Body.Field("query").HasField("includeTotalCount"),
				   "and is NOT inside the query object");

		// Absent rather than false when not asked for, matching the gateway's own reading.
		FJsonValue Without;
		FQueryBuilder("Scores").Build(Without, Error);
		PRAX_CHECK(!Without.HasField("includeTotalCount"), "omitted entirely when not requested");
	}

	PraxTest::Section("Query - limit is clamped UP to one");
	{
		// There is no zero-row request. Asking for zero returns one row, so an SDK that "optimises"
		// a count by requesting no rows gets a row back and may well count it.
		PRAX_EQ(FQueryBuilder("T").Limit(0).EffectiveLimit(), 1LL, "limit 0 becomes 1");
		PRAX_EQ(FQueryBuilder("T").Limit(-5).EffectiveLimit(), 1LL, "a negative limit becomes 1");
		PRAX_EQ(FQueryBuilder("T").Limit(1).EffectiveLimit(), 1LL, "limit 1 is honoured");
		PRAX_EQ(FQueryBuilder("T").Limit(200).EffectiveLimit(), 200LL, "a normal limit is honoured");

		// The absolute server cap. Asking for more is clamped, so a caller must read meta.limit
		// rather than assume - which is what the envelope tests cover.
		PRAX_EQ(FQueryBuilder("T").Limit(5000).EffectiveLimit(), 1000LL, "5000 is clamped to 1000");
		PRAX_EQ(FQueryBuilder("T").EffectiveLimit(), 50LL, "the default limit is 50");

		FJsonValue Body;
		FPraxError Error;
		FQueryBuilder("T").Limit(0).Build(Body, Error);
		PRAX_EQ(Body.Field("query").Field("limit").AsInt(), 1LL, "and the clamp reaches the body");
	}

	PraxTest::Section("Query - the request shape");
	{
		FJsonValue Body;
		FPraxError Error;
		const bool bOk = FQueryBuilder("Leaderboard")
			.Select({"Player", "Score"})
			.Where(Filters::Gte("Score", FJsonValue(100)))
			.WhereEquals("Season", FJsonValue(3))
			.OrderBy("Score", true)
			.Limit(20)
			.Offset(40)
			.Build(Body, Error);

		PRAX_CHECK(bOk, "a full query builds");

		// The table name lives in refs; the query refers to it by alias.
		PRAX_STR_EQ(Body.Field("refs").Field("t").AsString(), "Leaderboard",
					"the table name is in refs");
		PRAX_STR_EQ(Body.Field("query").Field("from").AsString(), "t",
					"and the query refers to the alias");

		const FJsonValue& Query = Body.Field("query");
		PRAX_EQ(static_cast<long long>(Query.Field("select").AsArray().size()), 2LL, "select");
		PRAX_EQ(static_cast<long long>(Query.Field("where").AsArray().size()), 2LL,
				"both where conditions");
		PRAX_EQ(Query.Field("limit").AsInt(), 20LL, "limit");
		PRAX_EQ(Query.Field("offset").AsInt(), 40LL, "offset");

		// orderBy is a list of {field, dir} objects, not a bare string or a pair.
		const FJsonValue& Order = Query.Field("orderBy");
		PRAX_EQ(static_cast<long long>(Order.AsArray().size()), 1LL, "one orderBy clause");
		PRAX_STR_EQ(Order.AsArray()[0].Field("field").AsString(), "Score", "with a field");
		PRAX_STR_EQ(Order.AsArray()[0].Field("dir").AsString(), "desc", "and a direction");

		FJsonValue Ascending;
		FQueryBuilder("T").OrderBy("A").Build(Ascending, Error);
		PRAX_STR_EQ(Ascending.Field("query").Field("orderBy").AsArray()[0].Field("dir").AsString(),
					"asc", "ascending is the default direction");
	}

	PraxTest::Section("Query - empty clauses are omitted, not sent empty");
	{
		FJsonValue Body;
		FPraxError Error;
		FQueryBuilder("T").Build(Body, Error);
		const FJsonValue& Query = Body.Field("query");

		// An empty select means "all columns"; sending select:[] could reasonably mean "no columns".
		PRAX_CHECK(!Query.HasField("select"), "an empty select is omitted");
		PRAX_CHECK(!Query.HasField("where"), "an empty where is omitted");
		PRAX_CHECK(!Query.HasField("orderBy"), "an empty orderBy is omitted");
		PRAX_CHECK(!Query.HasField("groupBy"), "an empty groupBy is omitted");
		PRAX_CHECK(!Query.HasField("having"), "an empty having is omitted");
	}

	PraxTest::Section("Query - refusals happen before anything is sent");
	{
		// An empty IN is valid JSON that matches nothing, so it would return an empty page that
		// reads exactly like a legitimate no-results answer.
		PRAX_STR_EQ(BuildOrReason(FQueryBuilder("T").Where(Filters::In("Level", {}))),
					"REFUSED:EMPTY_IN_LIST", "an empty 'in' list is refused");

		// A hand-written condition using an operator the gateway lacks.
		FJsonValue Invented = FJsonValue::Object();
		Invented.SetField("field", FJsonValue("Name"));
		Invented.SetField("op", FJsonValue("startsWith"));
		Invented.SetField("value", FJsonValue("A"));
		PRAX_STR_EQ(BuildOrReason(FQueryBuilder("T").Where(Invented)),
					"REFUSED:UNSUPPORTED_OPERATOR", "an unsupported operator is refused");

		PRAX_STR_EQ(BuildOrReason(FQueryBuilder("")), "REFUSED:NO_TABLE", "no table is refused");
		PRAX_STR_EQ(BuildOrReason(FQueryBuilder("T").Offset(-1)),
					"REFUSED:NEGATIVE_OFFSET", "a negative offset is refused");

		FJsonValue NoField = FJsonValue::Object();
		NoField.SetField("op", FJsonValue("eq"));
		NoField.SetField("value", FJsonValue(1));
		PRAX_STR_EQ(BuildOrReason(FQueryBuilder("T").Where(NoField)),
					"REFUSED:INVALID_CONDITION", "a condition with no field is refused");

		PRAX_STR_EQ(BuildOrReason(FQueryBuilder("T").Where(Filters::AnyOf({}))),
					"REFUSED:EMPTY_CONDITION_GROUP", "an empty or-group is refused");

		// Two bounds exactly - a between with one is a 400 that names no field.
		FJsonValue BadBetween = FJsonValue::Object();
		BadBetween.SetField("field", FJsonValue("Score"));
		BadBetween.SetField("op", FJsonValue("between"));
		BadBetween.SetField("value", FJsonValue(5));
		PRAX_STR_EQ(BuildOrReason(FQueryBuilder("T").Where(BadBetween)),
					"REFUSED:INVALID_CONDITION", "a malformed between is refused");
	}

	PraxTest::Section("Query - the server's own limits are enforced locally");
	{
		// Exceeding these is a 400 that names no field, which is far harder to act on than a
		// refusal here.
		FQueryBuilder TooManyColumns("T");
		std::vector<std::string> Columns;
		for (int i = 0; i < 101; ++i) { Columns.push_back("c" + std::to_string(i)); }
		TooManyColumns.Select(Columns);
		PRAX_STR_EQ(BuildOrReason(TooManyColumns), "REFUSED:TOO_MANY_COLUMNS",
					"more than 100 columns is refused");

		FQueryBuilder TooManyConditions("T");
		for (int i = 0; i < 51; ++i)
		{
			TooManyConditions.WhereEquals("c", FJsonValue(i));
		}
		PRAX_STR_EQ(BuildOrReason(TooManyConditions), "REFUSED:TOO_MANY_CONDITIONS",
					"more than 50 conditions is refused");

		// Nesting deeper than the parser allows.
		FJsonValue Deep = Filters::Eq("a", FJsonValue(1));
		for (int i = 0; i < 8; ++i)
		{
			Deep = Filters::AnyOf({Deep});
		}
		PRAX_STR_EQ(BuildOrReason(FQueryBuilder("T").Where(Deep)), "REFUSED:CONDITION_TOO_DEEP",
					"nesting beyond the limit is refused");

		// And nesting WITHIN the limit still works, or the check would be useless.
		FJsonValue Shallow = Filters::AnyOf({Filters::Eq("a", FJsonValue(1)),
											 Filters::AllOf({Filters::Eq("b", FJsonValue(2))})});
		FJsonValue Body;
		FPraxError Error;
		PRAX_CHECK(FQueryBuilder("T").Where(Shallow).Build(Body, Error),
				   "legal nesting is still allowed");
	}
}

void TestMutations()
{
	PraxTest::Section("Mutations - an unscoped update or delete is refused SYNCHRONOUSLY");
	{
		// The refusal is a returned false, right here, specifically so a caller cannot miss it.
		// Reporting this through a future or a callback means a caller who does not check gets
		// silence - no write, no error - and for a guardrail against an accidental table-wide UPDATE,
		// silence is the worst available outcome.
		FJsonValue Values = FJsonValue::Object();
		Values.SetField("Level", FJsonValue(13));

		FJsonValue Body;
		FPraxError Error;
		PRAX_CHECK(!Mutations::Update("Saves", Values, {}, Body, Error),
				   "an update with no conditions is refused");
		PRAX_STR_EQ(Error.ServerCode, "UNSCOPED_MUTATION", "with a machine-checkable code");
		PRAX_CHECK(Error.Message.find("every row") != std::string::npos,
				   "and a message that says what would have happened");

		PRAX_CHECK(!Mutations::Delete("Saves", {}, Body, Error),
				   "a delete with no conditions is refused");
		PRAX_STR_EQ(Error.ServerCode, "UNSCOPED_MUTATION", "with the same code");

		// One condition is enough - the rule is "scoped", not "well scoped".
		PRAX_CHECK(Mutations::Update("Saves", Values, {Filters::Eq("Slot", FJsonValue(1))},
									 Body, Error), "one condition is sufficient");
	}

	PraxTest::Section("Mutations - shapes");
	{
		FJsonValue Values = FJsonValue::Object();
		Values.SetField("Slot", FJsonValue(1));
		Values.SetField("Level", FJsonValue(12));

		FJsonValue Body;
		FPraxError Error;
		PRAX_CHECK(Mutations::Insert("Saves", Values, Body, Error), "insert builds");
		PRAX_STR_EQ(Body.Field("refs").Field("t").AsString(), "Saves", "the table is in refs");
		PRAX_STR_EQ(Body.Field("mutation").Field("type").AsString(), "insert", "type is insert");
		PRAX_STR_EQ(Body.Field("mutation").Field("table").AsString(), "t", "table is the alias");
		PRAX_CHECK(Body.Field("mutation").Field("values").IsObject(),
				   "values is an OBJECT, not an array of key/value pairs");

		Mutations::Update("Saves", Values, {Filters::Eq("Slot", FJsonValue(1))}, Body, Error);
		PRAX_STR_EQ(Body.Field("mutation").Field("type").AsString(), "update", "type is update");
		PRAX_CHECK(Body.Field("mutation").HasField("set"), "update uses 'set'");
		PRAX_CHECK(Body.Field("mutation").Field("where").IsArray(), "and carries a where array");

		Mutations::Delete("Saves", {Filters::Eq("Slot", FJsonValue(1))}, Body, Error);
		PRAX_STR_EQ(Body.Field("mutation").Field("type").AsString(), "delete", "type is delete");
		PRAX_CHECK(!Body.Field("mutation").HasField("set"), "delete carries no set");

		PRAX_CHECK(Mutations::InsertMany("Saves", {Values, Values}, Body, Error),
				   "insertMany builds");
		PRAX_CHECK(Body.Field("mutation").Field("values").IsArray(),
				   "with values as an array of rows");
		PRAX_EQ(static_cast<long long>(Body.Field("mutation").Field("values").AsArray().size()), 2LL,
				"carrying both rows");
	}

	PraxTest::Section("Mutations - native columns cannot be set");
	{
		// The backend fills these and rejects a request that supplies them. Catching it here names
		// the column, which the gateway's own rejection does less clearly.
		FJsonValue Body;
		FPraxError Error;
		for (const char* Column : {"ID", "CREATEDDATE", "CREATEDBY", "UPDATEDDATE", "UPDATEDBY",
								   "POSITION"})
		{
			FJsonValue Values = FJsonValue::Object();
			Values.SetField(Column, FJsonValue("x"));
			PRAX_CHECK(!Mutations::Insert("T", Values, Body, Error),
					   (std::string("refuses to set ") + Column).c_str());
			PRAX_STR_EQ(Error.ServerCode, "NATIVE_COLUMN", "with the right code");
		}

		// Case-insensitive: the backend's names are upper case but a caller may write "id".
		FJsonValue Lower = FJsonValue::Object();
		Lower.SetField("id", FJsonValue("x"));
		PRAX_CHECK(!Mutations::Insert("T", Lower, Body, Error), "lower-case 'id' is caught too");

		// Filtering ON a native column is fine and necessary - the rule is about setting them.
		FJsonValue Values = FJsonValue::Object();
		Values.SetField("Level", FJsonValue(1));
		PRAX_CHECK(Mutations::Update("T", Values, {Filters::Eq("ID", FJsonValue("row-1"))},
									 Body, Error),
				   "filtering on ID is allowed - only setting it is not");
	}

	PraxTest::Section("Mutations - values must be an object");
	{
		// A map that serialised as an array of key/value pairs is rejected by the gateway for every
		// mutation. Catching it here names the actual problem.
		FJsonValue Body;
		FPraxError Error;
		FJsonValue AsArray = FJsonValue::Array();
		AsArray.Push(FJsonValue("Level"));
		PRAX_CHECK(!Mutations::Insert("T", AsArray, Body, Error), "an array of values is refused");
		PRAX_STR_EQ(Error.ServerCode, "INVALID_VALUES", "with a clear code");

		PRAX_CHECK(!Mutations::Insert("T", FJsonValue::Object(), Body, Error),
				   "an empty values object is refused - it would do nothing");
		PRAX_STR_EQ(Error.ServerCode, "EMPTY_VALUES", "with its own code");

		PRAX_CHECK(!Mutations::InsertMany("T", {}, Body, Error), "insertMany with no rows is refused");
	}

	PraxTest::Section("Mutations - by-id helpers");
	{
		FJsonValue Values = FJsonValue::Object();
		Values.SetField("Level", FJsonValue(13));

		FJsonValue Body;
		FPraxError Error;
		PRAX_CHECK(Mutations::UpdateById("Saves", "row-1", Values, Body, Error),
				   "updateById builds");
		const FJsonValue& Where = Body.Field("mutation").Field("where");
		PRAX_EQ(static_cast<long long>(Where.AsArray().size()), 1LL, "with one condition");
		PRAX_STR_EQ(Where.AsArray()[0].Field("field").AsString(), "ID", "on ID");
		PRAX_STR_EQ(Where.AsArray()[0].Field("value").AsString(), "row-1", "with the given id");

		// An empty id would build an eq against "" - matching nothing, or whatever row has an empty
		// key. Neither is what the caller meant.
		PRAX_CHECK(!Mutations::UpdateById("Saves", "", Values, Body, Error),
				   "an empty row id is refused");
		PRAX_STR_EQ(Error.ServerCode, "NO_ROW_ID", "with its own code");
		PRAX_CHECK(!Mutations::DeleteById("Saves", "", Body, Error),
				   "and the same for deleteById");
	}
}
