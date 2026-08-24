// Praxsuite SDK for Unreal - JSON value type and codec.
//
// This file, and everything under Core/, is PORTABLE C++17. It uses no Unreal types, includes no
// Unreal headers, and can be compiled by any conforming compiler. That is deliberate and it is the
// single most important design decision in this SDK.
//
// The reason is verification. An Unreal plugin can only be compiled by a licensed engine install of
// around 100 GB, which means an SDK written entirely against the engine's own string and container
// types cannot be tested by CI - it can only be read and hoped over. Every rule in the Praxsuite
// conformance contract exists because getting it wrong fails SILENTLY, so "looks right" is not a
// standard worth shipping. By keeping the query building, filtering, error classification, key
// handling and this codec free of engine types, the whole of that logic is compiled and executed on
// every commit, under two compilers, with warnings as errors and sanitizers on.
//
// The engine-facing layer converts at the boundary, so Blueprint users and ordinary gameplay code
// never see std::string. If you are writing C++ against the core directly, you will - that is the
// trade, and it buys a tested core.
//
// Why a bundled codec rather than the engine's JSON module: that module would drag the engine into
// Core and defeat all of the above. It is also not obliged to preserve the two properties the
// gateway depends on, both of which are asserted in the test suite:
//
//   1. Astral-plane characters (emoji) survive a round trip, as raw UTF-8 AND as paired \uXXXX
//      surrogate escapes. Display names contain emoji constantly and a codec that mishandles
//      surrogate pairs corrupts them with no error.
//   2. A string-keyed map serialises as a JSON object, never as an array of key/value pairs. In
//      several languages a map also satisfies the sequence interface, and an ordering mistake in a
//      type switch emits [{"Key":..,"Value":..}], which the gateway rejects for every mutation.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Prax
{
	class FJsonValue;

	/** One key/value pair of a JSON object. Defined after FJsonValue, which it holds by value. */
	struct FJsonMember;

	using FJsonArray = std::vector<FJsonValue>;

	/**
	 * A convenience type for BUILDING an object, and nothing more - it is accepted by a constructor
	 * and never stored.
	 *
	 * FJsonValue used to hold one of these as a member, which is undefined behaviour: only vector,
	 * list and forward_list may be instantiated with an incomplete type, and FJsonValue is plainly
	 * incomplete inside its own definition. libstdc++ tolerates it, so it compiled and passed
	 * everywhere CI can reach - but MSVC's tree containers are far less forgiving, and MSVC is the
	 * compiler Unreal actually uses on Windows. That made it a build failure in the one environment
	 * this repository cannot test. Objects are stored as a sorted vector of FJsonMember instead.
	 */
	using FJsonObjectMap = std::map<std::string, FJsonValue>;

	/**
	 * A JSON value.
	 *
	 * Numbers are split into integer and double rather than kept as one double, because the gateway
	 * distinguishes them on the wire and a row id or a level number written as "12.0" is not the
	 * same request. A whole double is written without a trailing ".0" for the same reason.
	 */
	class FJsonValue
	{
	public:
		enum class EType : uint8_t
		{
			Null,
			Bool,
			Int,
			Double,
			String,
			Array,
			Object,
		};

		FJsonValue();
		FJsonValue(std::nullptr_t);
		FJsonValue(bool In);
		FJsonValue(int32_t In);
		FJsonValue(int64_t In);
		FJsonValue(double In);
		FJsonValue(const char* In);
		FJsonValue(std::string In);
		FJsonValue(FJsonArray In);

		/** Builds an object from a map. The map is consumed here and not retained - see above. */
		FJsonValue(const FJsonObjectMap& In);

		FJsonValue(const FJsonValue& Other);
		FJsonValue(FJsonValue&& Other) noexcept;
		FJsonValue& operator=(const FJsonValue& Other);
		FJsonValue& operator=(FJsonValue&& Other) noexcept;
		~FJsonValue();

		static FJsonValue Object();
		static FJsonValue Array();

		EType GetType() const { return Type; }
		bool IsNull() const { return Type == EType::Null; }
		bool IsBool() const { return Type == EType::Bool; }
		bool IsNumber() const { return Type == EType::Int || Type == EType::Double; }
		bool IsInt() const { return Type == EType::Int; }
		bool IsDouble() const { return Type == EType::Double; }
		bool IsString() const { return Type == EType::String; }
		bool IsArray() const { return Type == EType::Array; }
		bool IsObject() const { return Type == EType::Object; }

		bool AsBool(bool Fallback = false) const { return IsBool() ? BoolValue : Fallback; }
		int64_t AsInt(int64_t Fallback = 0) const;
		double AsDouble(double Fallback = 0.0) const;
		const std::string& AsString() const;
		const FJsonArray& AsArray() const;

		/**
		 * An object's members, sorted by key.
		 *
		 * Sorted rather than insertion-ordered so serialisation is deterministic - which is what lets
		 * the test suite assert an exact string rather than hunt for substrings.
		 */
		const std::vector<FJsonMember>& AsObject() const;

		/** Converts the value to an array if it is not one, then returns it for mutation. */
		FJsonArray& MutableArray();

		/** Reads a field, or a null value when absent. Never throws, never inserts. */
		const FJsonValue& Field(const std::string& Key) const;
		bool HasField(const std::string& Key) const;

		/** Sets a field, converting this value to an object if it is not one already. */
		void SetField(const std::string& Key, FJsonValue Value);

		/** Appends to an array, converting this value to an array if it is not one already. */
		void Push(FJsonValue Value);

		bool operator==(const FJsonValue& Other) const;
		bool operator!=(const FJsonValue& Other) const { return !(*this == Other); }

		/** Compact serialisation. No spaces, no trailing newline. */
		std::string ToString() const;

		/**
		 * Parses JSON text.
		 *
		 * Returns false and fills OutError on malformed input rather than throwing, because this
		 * runs on responses from a network and a malformed body is an ordinary runtime event, not a
		 * programming error.
		 */
		static bool Parse(const std::string& Text, FJsonValue& OutValue, std::string& OutError);

	private:
		EType Type;
		bool BoolValue = false;
		int64_t IntValue = 0;
		double DoubleValue = 0.0;
		std::string StringValue;

		// Both are vectors, which C++17 explicitly permits with an incomplete element type. That is
		// the whole reason objects are not stored in a std::map - see FJsonObjectMap above.
		FJsonArray ArrayValue;
		std::vector<FJsonMember> ObjectMembers;

		void Serialise(std::string& Out) const;
	};

	struct FJsonMember
	{
		std::string Key;
		FJsonValue Value;

		bool operator==(const FJsonMember& Other) const
		{
			return Key == Other.Key && Value == Other.Value;
		}
	};

	/** Escapes a string into a JSON string literal, including the surrounding quotes. */
	std::string EscapeJsonString(const std::string& In);
}
