// Query conditions.
//
// Portable C++17, no Unreal types - see Core/PraxJson.h for why.
//
// THIRTEEN OPERATORS EXIST. The gateway's parser accepts exactly these:
//
//   eq neq gt gte lt lte like ilike in is between contains textsearch
//
// Anything else is rejected at parse time. That makes offering a friendlier-sounding operator
// actively harmful: "notIn" or "isNull" as an operator compiles fine, reads well in review, and then
// fails at runtime on a player's machine. A sibling SDK exposed five such names and every call using
// one fails.
//
// So the helpers below with friendly names - StartsWith, IsNull, IsNotNull, EndsWith - are exactly
// that: helpers that COMPILE DOWN to a real operator. StartsWith becomes like "value%". IsNull
// becomes is null. The name is a convenience; what goes on the wire is always one of the thirteen.
//
// WHY THESE FUNCTIONS CANNOT FAIL. They return a condition, not a result, so they have nowhere to
// report a problem - and Unreal is routinely built with exceptions disabled, so throwing is not an
// option either. Validation therefore lives in the query and mutation builders, which DO have an
// error channel. The one case that matters is an empty IN list: it is valid JSON that matches
// nothing, silently, so the builders reject it before anything is sent.

#pragma once

#include <string>
#include <vector>

#include "Core/PraxJson.h"

namespace Prax
{
	namespace Filters
	{
		/** The complete set the gateway implements. Used by the builders to validate. */
		const std::vector<std::string>& SupportedOperators();

		bool IsSupportedOperator(const std::string& Op);

		FJsonValue Eq(const std::string& Field, FJsonValue Value);
		FJsonValue Neq(const std::string& Field, FJsonValue Value);
		FJsonValue Gt(const std::string& Field, FJsonValue Value);
		FJsonValue Gte(const std::string& Field, FJsonValue Value);
		FJsonValue Lt(const std::string& Field, FJsonValue Value);
		FJsonValue Lte(const std::string& Field, FJsonValue Value);

		/** SQL LIKE, case-sensitive. You supply the wildcards. */
		FJsonValue Like(const std::string& Field, const std::string& Pattern);

		/** Case-insensitive LIKE. */
		FJsonValue ILike(const std::string& Field, const std::string& Pattern);

		/** Substring match; no wildcards needed. */
		FJsonValue Contains(const std::string& Field, const std::string& Text);

		/** Full-text search over the column. */
		FJsonValue TextSearch(const std::string& Field, const std::string& Query);

		/**
		 * IN a list of values.
		 *
		 * An empty list produces a condition the BUILDER rejects. An empty IN is valid JSON that
		 * matches nothing, so left alone it returns an empty page and looks like "no rows matched" -
		 * which is the failure mode this whole SDK is organised around avoiding.
		 */
		FJsonValue In(const std::string& Field, const std::vector<FJsonValue>& Values);

		FJsonValue Between(const std::string& Field, FJsonValue Low, FJsonValue High);

		/** Compiles to `is null`. The gateway's `is` operator only tests for null. */
		FJsonValue IsNull(const std::string& Field);

		/** Compiles to `neq null` - there is no isNotNull operator. */
		FJsonValue IsNotNull(const std::string& Field);

		/** Compiles to like "value%". There is no startsWith operator. */
		FJsonValue StartsWith(const std::string& Field, const std::string& Value);

		/** Compiles to like "%value". */
		FJsonValue EndsWith(const std::string& Field, const std::string& Value);

		/** Matches when ANY nested condition matches. Emits {"or":[...]}. */
		FJsonValue AnyOf(const std::vector<FJsonValue>& Conditions);

		/** Matches when EVERY nested condition matches. Only needed inside an AnyOf. */
		FJsonValue AllOf(const std::vector<FJsonValue>& Conditions);

		/**
		 * Escapes LIKE wildcards in a value that should be matched literally.
		 *
		 * A player-supplied search term containing % or _ is otherwise a wildcard, so searching for
		 * "100%" matches far more than intended. StartsWith and EndsWith do NOT apply this: they take
		 * a value the caller may deliberately want wildcards in.
		 */
		std::string EscapeLikeValue(const std::string& Value);
	}
}
