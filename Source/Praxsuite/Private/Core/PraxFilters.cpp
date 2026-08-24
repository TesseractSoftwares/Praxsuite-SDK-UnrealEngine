#include "Core/PraxFilters.h"

#include <algorithm>

namespace Prax
{
	namespace
	{
		FJsonValue MakeCondition(const std::string& Field, const char* Op, FJsonValue Value)
		{
			FJsonValue Out = FJsonValue::Object();
			Out.SetField("field", FJsonValue(Field));
			Out.SetField("op", FJsonValue(Op));
			Out.SetField("value", std::move(Value));
			return Out;
		}

		FJsonValue MakeGroup(const char* Key, const std::vector<FJsonValue>& Conditions)
		{
			FJsonValue Out = FJsonValue::Object();
			FJsonValue List = FJsonValue::Array();
			for (const FJsonValue& Condition : Conditions)
			{
				List.Push(Condition);
			}
			Out.SetField(Key, std::move(List));
			return Out;
		}
	}

	namespace Filters
	{
		const std::vector<std::string>& SupportedOperators()
		{
			// Exactly the thirteen the parser accepts. Adding a name here without the gateway
			// implementing it moves a runtime 400 to a place nobody will look for it.
			static const std::vector<std::string> Ops = {
				"eq", "neq", "gt", "gte", "lt", "lte",
				"like", "ilike", "in", "is", "between", "contains", "textsearch",
			};
			return Ops;
		}

		bool IsSupportedOperator(const std::string& Op)
		{
			const std::vector<std::string>& Ops = SupportedOperators();
			return std::find(Ops.begin(), Ops.end(), Op) != Ops.end();
		}

		FJsonValue Eq(const std::string& Field, FJsonValue Value)
		{
			return MakeCondition(Field, "eq", std::move(Value));
		}

		FJsonValue Neq(const std::string& Field, FJsonValue Value)
		{
			return MakeCondition(Field, "neq", std::move(Value));
		}

		FJsonValue Gt(const std::string& Field, FJsonValue Value)
		{
			return MakeCondition(Field, "gt", std::move(Value));
		}

		FJsonValue Gte(const std::string& Field, FJsonValue Value)
		{
			return MakeCondition(Field, "gte", std::move(Value));
		}

		FJsonValue Lt(const std::string& Field, FJsonValue Value)
		{
			return MakeCondition(Field, "lt", std::move(Value));
		}

		FJsonValue Lte(const std::string& Field, FJsonValue Value)
		{
			return MakeCondition(Field, "lte", std::move(Value));
		}

		FJsonValue Like(const std::string& Field, const std::string& Pattern)
		{
			return MakeCondition(Field, "like", FJsonValue(Pattern));
		}

		FJsonValue ILike(const std::string& Field, const std::string& Pattern)
		{
			return MakeCondition(Field, "ilike", FJsonValue(Pattern));
		}

		FJsonValue Contains(const std::string& Field, const std::string& Text)
		{
			return MakeCondition(Field, "contains", FJsonValue(Text));
		}

		FJsonValue TextSearch(const std::string& Field, const std::string& Query)
		{
			return MakeCondition(Field, "textsearch", FJsonValue(Query));
		}

		FJsonValue In(const std::string& Field, const std::vector<FJsonValue>& Values)
		{
			FJsonValue List = FJsonValue::Array();
			for (const FJsonValue& Value : Values)
			{
				List.Push(Value);
			}
			// An empty list is emitted as-is and rejected by the builder, which has an error channel.
			// See PraxFilters.h for why this function cannot report it itself.
			return MakeCondition(Field, "in", std::move(List));
		}

		FJsonValue Between(const std::string& Field, FJsonValue Low, FJsonValue High)
		{
			FJsonValue Bounds = FJsonValue::Array();
			Bounds.Push(std::move(Low));
			Bounds.Push(std::move(High));
			return MakeCondition(Field, "between", std::move(Bounds));
		}

		FJsonValue IsNull(const std::string& Field)
		{
			// `is` with a null value. The gateway's `is` operator tests only for null, so there is no
			// isNull operator to offer and offering one would be a runtime 400.
			return MakeCondition(Field, "is", FJsonValue(nullptr));
		}

		FJsonValue IsNotNull(const std::string& Field)
		{
			return MakeCondition(Field, "neq", FJsonValue(nullptr));
		}

		FJsonValue StartsWith(const std::string& Field, const std::string& Value)
		{
			// No escaping of the caller's value: someone writing StartsWith("Name", "A%B") may well
			// mean that wildcard. EscapeLikeValue is available for a literal match.
			return Like(Field, Value + "%");
		}

		FJsonValue EndsWith(const std::string& Field, const std::string& Value)
		{
			return Like(Field, "%" + Value);
		}

		FJsonValue AnyOf(const std::vector<FJsonValue>& Conditions)
		{
			return MakeGroup("or", Conditions);
		}

		FJsonValue AllOf(const std::vector<FJsonValue>& Conditions)
		{
			return MakeGroup("and", Conditions);
		}

		std::string EscapeLikeValue(const std::string& Value)
		{
			// % and _ are SQL LIKE wildcards. A player searching for "100%" otherwise matches every
			// row beginning "100".
			std::string Out;
			Out.reserve(Value.size());
			for (const char C : Value)
			{
				if (C == '%' || C == '_' || C == '\\')
				{
					Out.push_back('\\');
				}
				Out.push_back(C);
			}
			return Out;
		}
	}
}
