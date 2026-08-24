// Praxsuite SDK for Unreal - JSON value type and codec.
//
// This file, and everything under Core/, is PORTABLE C++17. It uses no Unreal types, includes no
// Unreal headers, and can be compiled by any conforming compiler. That is deliberate and it is the
// single most important design decision in this SDK.
//
// The reason is verification. An Unreal plugin can only be compiled by a licensed ~100 GB engine
// install, which means an SDK written entirely against FString and TArray cannot be tested by CI -
// it can only be read and hoped over. Every rule in the Praxsuite conformance contract exists
// because getting it wrong fails SILENTLY, so "looks right" is not a standard worth shipping. By
// keeping the query building, filtering, error classification, key handling and this codec free of
// engine types, the whole of that logic is compiled and executed on every commit.
//
// The engine-facing layer (UPraxsuiteSubsystem) converts at the boundary, so Blueprint users and
// ordinary gameplay C++ never see std::string. If you are writing C++ against the core directly,
// you will - and that is the trade.
//
// Why a bundled codec rather than Unreal's FJsonObject: FJsonObject lives in the Json module and
// would drag the engine into Core, defeating the above. It is also not obliged to preserve the two
// properties the gateway depends on, both of which are asserted in the test suite:
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
#include <memory>
#include <string>
#include <vector>

namespace Prax
{
	class FJsonValue;

	using FJsonObjectMap = std::map<std::string, FJsonValue>;
	using FJsonArray = std::vector<FJsonValue>;

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

		FJsonValue() : Type(EType::Null) {}
		FJsonValue(std::nullptr_t) : Type(EType::Null) {}
		FJsonValue(bool In) : Type(EType::Bool), BoolValue(In) {}
		FJsonValue(int32_t In) : Type(EType::Int), IntValue(In) {}
		FJsonValue(int64_t In) : Type(EType::Int), IntValue(In) {}
		FJsonValue(double In) : Type(EType::Double), DoubleValue(In) {}
		FJsonValue(const char* In) : Type(EType::String), StringValue(In ? In : "") {}
		FJsonValue(std::string In) : Type(EType::String), StringValue(std::move(In)) {}
		FJsonValue(FJsonArray In) : Type(EType::Array), ArrayValue(std::move(In)) {}
		FJsonValue(FJsonObjectMap In) : Type(EType::Object), ObjectValue(std::move(In)) {}

		static FJsonValue Object() { return FJsonValue(FJsonObjectMap{}); }
		static FJsonValue Array() { return FJsonValue(FJsonArray{}); }

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
		const FJsonObjectMap& AsObject() const;

		/** Mutable accessors. Calling these converts the value to the matching type if it is not one. */
		FJsonArray& MutableArray();
		FJsonObjectMap& MutableObject();

		/** Reads a field, or a null value when absent. Never throws, never inserts. */
		const FJsonValue& Field(const std::string& Key) const;
		bool HasField(const std::string& Key) const;

		void SetField(const std::string& Key, FJsonValue Value);
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
		FJsonArray ArrayValue;
		FJsonObjectMap ObjectValue;

		void Serialise(std::string& Out) const;
	};

	/** Escapes a string into a JSON string literal, including the surrounding quotes. */
	std::string EscapeJsonString(const std::string& In);
}
