// Tests for the bundled JSON codec.
//
// Two of these exist because the conformance contract names them, and both describe failures that
// produce wrong data rather than an error:
//
//   encoding.unicode - astral-plane characters must survive a round trip, as raw UTF-8 AND as
//   paired \uXXXX escapes. A codec that emits the two halves of a surrogate pair separately
//   corrupts every emoji in a display name, silently.
//
//   encoding.dictionarySerialization - a string-keyed map must serialise as a JSON object, never as
//   an array of key/value pairs. std::map is a sequence of pairs as well as a mapping, which is
//   exactly the ambiguity that has produced [{"Key":..,"Value":..}] in other languages and had
//   every mutation rejected.

#include "PraxTestHarness.h"
#include "Core/PraxJson.h"

using Prax::FJsonValue;
using Prax::FJsonObjectMap;
using Prax::FJsonArray;

namespace
{
	std::string RoundTrip(const std::string& Text)
	{
		FJsonValue Value;
		std::string Error;
		if (!FJsonValue::Parse(Text, Value, Error))
		{
			return "PARSE FAILED: " + Error;
		}
		return Value.ToString();
	}
}

void TestJson()
{
	PraxTest::Section("JSON codec - scalars");
	{
		PRAX_STR_EQ(FJsonValue(nullptr).ToString(), "null", "null serialises");
		PRAX_STR_EQ(FJsonValue(true).ToString(), "true", "true serialises");
		PRAX_STR_EQ(FJsonValue(false).ToString(), "false", "false serialises");
		PRAX_STR_EQ(FJsonValue(static_cast<int64_t>(42)).ToString(), "42", "integer serialises");
		PRAX_STR_EQ(FJsonValue(static_cast<int64_t>(-7)).ToString(), "-7", "negative integer serialises");
		PRAX_STR_EQ(FJsonValue(1.5).ToString(), "1.5", "fractional double serialises");

		// A whole double must not gain a ".0". The gateway distinguishes 12 from 12.0 on the wire,
		// and a row id or a level number written as a decimal is a different request.
		PRAX_STR_EQ(FJsonValue(12.0).ToString(), "12", "a whole double loses its decimal point");

		// JSON has no NaN or Infinity. Emitting one produces a body the gateway rejects with a parse
		// error naming no field, so null is the less confusing failure.
		PRAX_STR_EQ(FJsonValue(1.0 / 0.0).ToString(), "null", "infinity becomes null, not garbage");
		PRAX_STR_EQ(FJsonValue(0.0 / 0.0).ToString(), "null", "NaN becomes null, not garbage");
	}

	PraxTest::Section("JSON codec - strings and escapes");
	{
		PRAX_STR_EQ(FJsonValue("hello").ToString(), "\"hello\"", "plain string");
		PRAX_STR_EQ(FJsonValue("a\"b").ToString(), "\"a\\\"b\"", "quote is escaped");
		PRAX_STR_EQ(FJsonValue("a\\b").ToString(), "\"a\\\\b\"", "backslash is escaped");
		PRAX_STR_EQ(FJsonValue("a\nb").ToString(), "\"a\\nb\"", "newline is escaped");
		PRAX_STR_EQ(FJsonValue("a\tb").ToString(), "\"a\\tb\"", "tab is escaped");

		FJsonValue Parsed;
		std::string Error;
		PRAX_CHECK(FJsonValue::Parse("\"a\\u0041b\"", Parsed, Error), "a \\u escape parses");
		PRAX_STR_EQ(Parsed.AsString(), "aAb", "\\u0041 decodes to A");

		PRAX_CHECK(!FJsonValue::Parse("\"unterminated", Parsed, Error), "an unterminated string fails");
		PRAX_CHECK(!Error.empty(), "and reports why");
	}

	PraxTest::Section("JSON codec - astral plane (contract: encoding.unicode)");
	{
		// U+1F600 GRINNING FACE, as raw UTF-8.
		const std::string Emoji = "\xF0\x9F\x98\x80";
		const std::string WithEmoji = "Player " + Emoji + " One";

		FJsonValue Value(WithEmoji);
		const std::string Serialised = Value.ToString();

		FJsonValue Reparsed;
		std::string Error;
		PRAX_CHECK(FJsonValue::Parse(Serialised, Reparsed, Error), "raw UTF-8 emoji round trips");
		PRAX_STR_EQ(Reparsed.AsString(), WithEmoji, "and is byte-identical afterwards");

		// The same code point arriving as a surrogate PAIR, which is how many servers escape it.
		// Decoding the halves independently is the bug this guards: each half is not a valid code
		// point, and the result is mojibake with no error raised.
		FJsonValue FromEscapes;
		PRAX_CHECK(FJsonValue::Parse("\"\\ud83d\\ude00\"", FromEscapes, Error),
				   "a surrogate pair parses");
		PRAX_STR_EQ(FromEscapes.AsString(), Emoji, "and reassembles into one code point");
		PRAX_EQ(static_cast<long long>(FromEscapes.AsString().size()), 4LL,
				"the emoji is 4 UTF-8 bytes, not 6 and not 2");

		// An unpaired surrogate is malformed input. U+FFFD rather than a hard failure, so one bad
		// display name cannot make a whole page of rows unreadable.
		FJsonValue Lone;
		PRAX_CHECK(FJsonValue::Parse("\"\\ud83d\"", Lone, Error), "an unpaired high surrogate parses");
		PRAX_STR_EQ(Lone.AsString(), "\xEF\xBF\xBD", "and becomes U+FFFD rather than half a character");

		// Non-BMP via a longer string, and a 3-byte character, to catch off-by-one in AppendUtf8.
		FJsonValue Cjk;
		PRAX_CHECK(FJsonValue::Parse("\"\\u4e2d\\u6587\"", Cjk, Error), "3-byte escapes parse");
		PRAX_STR_EQ(Cjk.AsString(), "\xE4\xB8\xAD\xE6\x96\x87", "and encode as 6 UTF-8 bytes");
	}

	PraxTest::Section("JSON codec - maps are objects (contract: encoding.dictionarySerialization)");
	{
		FJsonObjectMap Fields;
		Fields["Slot"] = FJsonValue(static_cast<int64_t>(1));
		Fields["Level"] = FJsonValue(static_cast<int64_t>(12));
		const std::string Text = FJsonValue(Fields).ToString();

		// Keys are ordered because std::map is ordered, which makes this assertion exact rather than
		// a substring hunt.
		PRAX_STR_EQ(Text, "{\"Level\":12,\"Slot\":1}", "a map serialises as a JSON object");
		PRAX_CHECK(Text.find("\"Key\"") == std::string::npos, "and emits no \"Key\" field");
		PRAX_CHECK(Text.front() == '{', "and opens with a brace, not a bracket");

		PRAX_STR_EQ(FJsonValue::Object().ToString(), "{}", "an empty object is {}");
		PRAX_STR_EQ(FJsonValue::Array().ToString(), "[]", "an empty array is []");
		PRAX_CHECK(FJsonValue::Object().IsObject(), "an empty object is still an object");
		PRAX_CHECK(!FJsonValue::Object().IsArray(), "and is not an array");
	}

	PraxTest::Section("JSON codec - object storage is a sorted vector, not a map");
	{
		// Objects used to be a std::map member, which is undefined behaviour: only vector, list and
		// forward_list may be instantiated with an incomplete type, and FJsonValue is incomplete
		// inside its own definition. libstdc++ tolerated it; MSVC - the compiler Unreal actually
		// uses on Windows - is far less forgiving, so it was a build failure in the one environment
		// CI cannot reach. Storage is a sorted vector of members now, and these pin the behaviour
		// that change had to preserve.
		FJsonValue Value = FJsonValue::Object();
		Value.SetField("zebra", FJsonValue(1));
		Value.SetField("alpha", FJsonValue(2));
		Value.SetField("middle", FJsonValue(3));

		PRAX_STR_EQ(Value.ToString(), "{\"alpha\":2,\"middle\":3,\"zebra\":1}",
					"keys serialise in sorted order regardless of insertion order");

		// Sorted order is what makes serialisation deterministic, which is what lets every other
		// assertion here compare an exact string instead of hunting substrings.
		const auto& Members = Value.AsObject();
		PRAX_EQ(static_cast<long long>(Members.size()), 3LL, "three members stored");
		PRAX_STR_EQ(Members[0].Key, "alpha", "first member is the lowest key");
		PRAX_STR_EQ(Members[2].Key, "zebra", "last member is the highest key");

		// Setting an existing key must REPLACE it, which is what the map did. Appending instead
		// would emit a duplicate key - valid JSON that the gateway reads unpredictably.
		Value.SetField("alpha", FJsonValue(99));
		PRAX_EQ(static_cast<long long>(Value.AsObject().size()), 3LL,
				"re-setting a key does not add a second copy");
		PRAX_EQ(Value.Field("alpha").AsInt(), 99LL, "and the new value wins");

		// The same rule applies to duplicate keys arriving over the wire: last one wins.
		FJsonValue Parsed;
		std::string Error;
		PRAX_CHECK(FJsonValue::Parse("{\"a\":1,\"a\":2}", Parsed, Error), "a duplicate key parses");
		PRAX_EQ(static_cast<long long>(Parsed.AsObject().size()), 1LL, "and collapses to one member");
		PRAX_EQ(Parsed.Field("a").AsInt(), 2LL, "with the last value winning");

		// Lookup is a binary search over that vector, so a miss must not land on a neighbour.
		PRAX_CHECK(Value.Field("alph").IsNull(), "a key that is a prefix of a real one misses");
		PRAX_CHECK(Value.Field("alphaa").IsNull(), "a key that extends a real one misses");
		PRAX_CHECK(Value.Field("").IsNull(), "an empty key misses");
		PRAX_CHECK(!Value.HasField("zebrb"), "HasField agrees with Field on a near miss");

		// A value equal by content must compare equal whatever order it was built in, because
		// element-wise vector comparison is only order-independent while the sort holds.
		FJsonValue Other = FJsonValue::Object();
		Other.SetField("middle", FJsonValue(3));
		Other.SetField("zebra", FJsonValue(1));
		Other.SetField("alpha", FJsonValue(99));
		PRAX_CHECK(Value == Other, "two objects built in different orders compare equal");
	}

	PraxTest::Section("JSON codec - round trips");
	{
		PRAX_STR_EQ(RoundTrip("{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":null}}"),
					"{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":null}}", "nested structure round trips");
		PRAX_STR_EQ(RoundTrip("[]"), "[]", "empty array round trips");
		PRAX_STR_EQ(RoundTrip("{}"), "{}", "empty object round trips");
		PRAX_STR_EQ(RoundTrip("  { \"a\" : 1 }  "), "{\"a\":1}", "whitespace is ignored");
		PRAX_STR_EQ(RoundTrip("true"), "true", "a bare bool is a valid document");
	}

	PraxTest::Section("JSON codec - numbers keep their kind");
	{
		FJsonValue Value;
		std::string Error;

		PRAX_CHECK(FJsonValue::Parse("{\"n\":12}", Value, Error), "whole number parses");
		PRAX_CHECK(Value.Field("n").IsInt(), "a whole number is an Int, not a Double");
		PRAX_STR_EQ(Value.ToString(), "{\"n\":12}", "and survives without gaining a decimal point");

		PRAX_CHECK(FJsonValue::Parse("{\"n\":1.5}", Value, Error), "fractional number parses");
		PRAX_CHECK(Value.Field("n").IsDouble(), "a fractional number is a Double");

		PRAX_CHECK(FJsonValue::Parse("{\"n\":1e3}", Value, Error), "exponent notation parses");
		PRAX_CHECK(Value.Field("n").IsDouble(), "and is treated as a Double");
		PRAX_EQ(Value.Field("n").AsInt(), 1000LL, "with the right value");
	}

	PraxTest::Section("JSON codec - reading is total, never throwing");
	{
		FJsonValue Value;
		std::string Error;
		FJsonValue::Parse("{\"a\":{\"b\":1}}", Value, Error);

		// Every accessor has to be safe on the wrong type. This runs against a network response, and
		// a missing field is an ordinary runtime event that must not take down a game.
		PRAX_EQ(Value.Field("a").Field("b").AsInt(), 1LL, "a nested field reads");
		PRAX_CHECK(Value.Field("nope").IsNull(), "a missing field is null, not a crash");
		PRAX_CHECK(Value.Field("nope").Field("deeper").IsNull(), "and chains safely");
		PRAX_STR_EQ(Value.Field("a").AsString(), "", "AsString on an object is empty, not a throw");
		PRAX_EQ(Value.Field("a").AsInt(99), 99LL, "AsInt on an object returns the fallback");
		PRAX_CHECK(Value.AsArray().empty(), "AsArray on an object is empty");
		PRAX_CHECK(!Value.HasField("nope"), "HasField is false for a missing key");
		PRAX_CHECK(Value.HasField("a"), "HasField is true for a present key");
	}

	PraxTest::Section("JSON codec - malformed input is rejected");
	{
		FJsonValue Value;
		std::string Error;
		const char* Bad[] = {
			"{",  "}", "[", "]", "{\"a\"}", "{\"a\":}", "[1,]", "tru", "\"\\q\"", "",
			"{\"a\":1}trailing",
		};
		for (const char* Text : Bad)
		{
			const bool bParsed = FJsonValue::Parse(Text, Value, Error);
			PRAX_CHECK(!bParsed, (std::string("rejects: ") + Text).c_str());
		}
	}
}
