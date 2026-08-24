#include "Core/PraxJson.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace Prax
{
	namespace
	{
		const FJsonValue NullValue{};
		const std::string EmptyString{};
		const FJsonArray EmptyArray{};
		const FJsonObjectMap EmptyObject{};

		/** Appends a code point as UTF-8. */
		void AppendUtf8(std::string& Out, uint32_t CodePoint)
		{
			if (CodePoint <= 0x7F)
			{
				Out.push_back(static_cast<char>(CodePoint));
			}
			else if (CodePoint <= 0x7FF)
			{
				Out.push_back(static_cast<char>(0xC0 | (CodePoint >> 6)));
				Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
			}
			else if (CodePoint <= 0xFFFF)
			{
				Out.push_back(static_cast<char>(0xE0 | (CodePoint >> 12)));
				Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F)));
				Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
			}
			else
			{
				Out.push_back(static_cast<char>(0xF0 | (CodePoint >> 18)));
				Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 12) & 0x3F)));
				Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F)));
				Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
			}
		}

		bool ParseHex4(const std::string& Text, size_t Pos, uint32_t& Out)
		{
			if (Pos + 4 > Text.size())
			{
				return false;
			}
			Out = 0;
			for (size_t i = 0; i < 4; ++i)
			{
				const char C = Text[Pos + i];
				Out <<= 4;
				if (C >= '0' && C <= '9') { Out |= static_cast<uint32_t>(C - '0'); }
				else if (C >= 'a' && C <= 'f') { Out |= static_cast<uint32_t>(C - 'a' + 10); }
				else if (C >= 'A' && C <= 'F') { Out |= static_cast<uint32_t>(C - 'A' + 10); }
				else { return false; }
			}
			return true;
		}

		struct FParser
		{
			const std::string& Text;
			size_t Pos = 0;
			std::string Error;

			explicit FParser(const std::string& InText) : Text(InText) {}

			void SkipWhitespace()
			{
				while (Pos < Text.size())
				{
					const char C = Text[Pos];
					if (C == ' ' || C == '\t' || C == '\n' || C == '\r') { ++Pos; }
					else { break; }
				}
			}

			bool Fail(const char* Message)
			{
				if (Error.empty())
				{
					Error = std::string(Message) + " at offset " + std::to_string(Pos);
				}
				return false;
			}

			bool ParseValue(FJsonValue& Out, int Depth)
			{
				// A bound on nesting rather than unbounded recursion: this parses network input, and
				// a deeply nested body would otherwise be a stack overflow rather than an error.
				if (Depth > 128)
				{
					return Fail("JSON nested too deeply");
				}

				SkipWhitespace();
				if (Pos >= Text.size())
				{
					return Fail("unexpected end of input");
				}

				switch (Text[Pos])
				{
				case 'n': return ParseLiteral("null", FJsonValue(nullptr), Out);
				case 't': return ParseLiteral("true", FJsonValue(true), Out);
				case 'f': return ParseLiteral("false", FJsonValue(false), Out);
				case '"': return ParseString(Out);
				case '[': return ParseArray(Out, Depth);
				case '{': return ParseObject(Out, Depth);
				default:  return ParseNumber(Out);
				}
			}

			bool ParseLiteral(const char* Literal, FJsonValue Value, FJsonValue& Out)
			{
				const size_t Length = std::string(Literal).size();
				if (Text.compare(Pos, Length, Literal) != 0)
				{
					return Fail("invalid literal");
				}
				Pos += Length;
				Out = std::move(Value);
				return true;
			}

			bool ParseString(FJsonValue& Out)
			{
				std::string Result;
				if (!ParseRawString(Result))
				{
					return false;
				}
				Out = FJsonValue(std::move(Result));
				return true;
			}

			bool ParseRawString(std::string& Out)
			{
				if (Pos >= Text.size() || Text[Pos] != '"')
				{
					return Fail("expected a string");
				}
				++Pos;

				while (Pos < Text.size())
				{
					const unsigned char C = static_cast<unsigned char>(Text[Pos]);
					if (C == '"')
					{
						++Pos;
						return true;
					}
					if (C == '\\')
					{
						++Pos;
						if (Pos >= Text.size())
						{
							return Fail("truncated escape");
						}
						const char E = Text[Pos++];
						switch (E)
						{
						case '"':  Out.push_back('"');  break;
						case '\\': Out.push_back('\\'); break;
						case '/':  Out.push_back('/');  break;
						case 'b':  Out.push_back('\b'); break;
						case 'f':  Out.push_back('\f'); break;
						case 'n':  Out.push_back('\n'); break;
						case 'r':  Out.push_back('\r'); break;
						case 't':  Out.push_back('\t'); break;
						case 'u':
						{
							uint32_t Unit = 0;
							if (!ParseHex4(Text, Pos, Unit))
							{
								return Fail("malformed \\u escape");
							}
							Pos += 4;

							// A high surrogate must be followed by its low partner. Emitting the two
							// halves separately is how emoji get corrupted: each half is an invalid
							// code point on its own, and the result is mojibake with no error.
							if (Unit >= 0xD800 && Unit <= 0xDBFF)
							{
								if (Pos + 1 < Text.size() && Text[Pos] == '\\' && Text[Pos + 1] == 'u')
								{
									uint32_t Low = 0;
									if (ParseHex4(Text, Pos + 2, Low) && Low >= 0xDC00 && Low <= 0xDFFF)
									{
										Pos += 6;
										const uint32_t CodePoint =
											0x10000u + ((Unit - 0xD800u) << 10) + (Low - 0xDC00u);
										AppendUtf8(Out, CodePoint);
										break;
									}
								}
								// An unpaired high surrogate is malformed input. U+FFFD rather than a
								// hard failure, so one bad display name cannot make a whole page of
								// rows unreadable.
								AppendUtf8(Out, 0xFFFD);
								break;
							}
							if (Unit >= 0xDC00 && Unit <= 0xDFFF)
							{
								AppendUtf8(Out, 0xFFFD);
								break;
							}
							AppendUtf8(Out, Unit);
							break;
						}
						default:
							return Fail("unknown escape");
						}
						continue;
					}
					if (C < 0x20)
					{
						return Fail("unescaped control character in string");
					}
					// Raw UTF-8 bytes pass through untouched, which is the other half of the emoji
					// requirement: the gateway sends raw UTF-8, not escapes.
					Out.push_back(static_cast<char>(C));
					++Pos;
				}
				return Fail("unterminated string");
			}

			bool ParseNumber(FJsonValue& Out)
			{
				const size_t Start = Pos;
				if (Pos < Text.size() && (Text[Pos] == '-' || Text[Pos] == '+')) { ++Pos; }

				bool bAnyDigits = false;
				while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9') { ++Pos; bAnyDigits = true; }

				bool bFractional = false;
				if (Pos < Text.size() && Text[Pos] == '.')
				{
					bFractional = true;
					++Pos;
					while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9') { ++Pos; bAnyDigits = true; }
				}
				if (Pos < Text.size() && (Text[Pos] == 'e' || Text[Pos] == 'E'))
				{
					bFractional = true;
					++Pos;
					if (Pos < Text.size() && (Text[Pos] == '-' || Text[Pos] == '+')) { ++Pos; }
					while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9') { ++Pos; }
				}

				if (!bAnyDigits)
				{
					return Fail("expected a number");
				}

				const std::string Token = Text.substr(Start, Pos - Start);
				if (bFractional)
				{
					Out = FJsonValue(std::strtod(Token.c_str(), nullptr));
				}
				else
				{
					// An integer too large for int64 becomes a double rather than wrapping. A silently
					// wrapped id is worse than a slightly imprecise one.
					errno = 0;
					const long long Parsed = std::strtoll(Token.c_str(), nullptr, 10);
					if (errno == ERANGE)
					{
						Out = FJsonValue(std::strtod(Token.c_str(), nullptr));
					}
					else
					{
						Out = FJsonValue(static_cast<int64_t>(Parsed));
					}
				}
				return true;
			}

			bool ParseArray(FJsonValue& Out, int Depth)
			{
				++Pos; // '['
				FJsonArray Items;
				SkipWhitespace();
				if (Pos < Text.size() && Text[Pos] == ']')
				{
					++Pos;
					Out = FJsonValue(std::move(Items));
					return true;
				}
				while (true)
				{
					FJsonValue Item;
					if (!ParseValue(Item, Depth + 1))
					{
						return false;
					}
					Items.push_back(std::move(Item));
					SkipWhitespace();
					if (Pos >= Text.size())
					{
						return Fail("unterminated array");
					}
					if (Text[Pos] == ',') { ++Pos; continue; }
					if (Text[Pos] == ']') { ++Pos; break; }
					return Fail("expected ',' or ']'");
				}
				Out = FJsonValue(std::move(Items));
				return true;
			}

			bool ParseObject(FJsonValue& Out, int Depth)
			{
				++Pos; // '{'
				FJsonObjectMap Fields;
				SkipWhitespace();
				if (Pos < Text.size() && Text[Pos] == '}')
				{
					++Pos;
					Out = FJsonValue(std::move(Fields));
					return true;
				}
				while (true)
				{
					SkipWhitespace();
					std::string Key;
					if (!ParseRawString(Key))
					{
						return false;
					}
					SkipWhitespace();
					if (Pos >= Text.size() || Text[Pos] != ':')
					{
						return Fail("expected ':'");
					}
					++Pos;

					FJsonValue Value;
					if (!ParseValue(Value, Depth + 1))
					{
						return false;
					}
					Fields[std::move(Key)] = std::move(Value);

					SkipWhitespace();
					if (Pos >= Text.size())
					{
						return Fail("unterminated object");
					}
					if (Text[Pos] == ',') { ++Pos; continue; }
					if (Text[Pos] == '}') { ++Pos; break; }
					return Fail("expected ',' or '}'");
				}
				Out = FJsonValue(std::move(Fields));
				return true;
			}
		};
	}

	std::string EscapeJsonString(const std::string& In)
	{
		std::string Out;
		Out.reserve(In.size() + 2);
		Out.push_back('"');
		for (const char Raw : In)
		{
			const unsigned char C = static_cast<unsigned char>(Raw);
			switch (C)
			{
			case '"':  Out += "\\\""; break;
			case '\\': Out += "\\\\"; break;
			case '\b': Out += "\\b";  break;
			case '\f': Out += "\\f";  break;
			case '\n': Out += "\\n";  break;
			case '\r': Out += "\\r";  break;
			case '\t': Out += "\\t";  break;
			default:
				if (C < 0x20)
				{
					char Buffer[7];
					std::snprintf(Buffer, sizeof(Buffer), "\\u%04x", C);
					Out += Buffer;
				}
				else
				{
					// Multi-byte UTF-8 is emitted as-is rather than re-escaped. Escaping would mean
					// decoding to code points and re-encoding surrogate pairs - two more places for
					// an emoji to get mangled, for no benefit: the gateway accepts raw UTF-8.
					Out.push_back(Raw);
				}
				break;
			}
		}
		Out.push_back('"');
		return Out;
	}

	int64_t FJsonValue::AsInt(int64_t Fallback) const
	{
		if (Type == EType::Int) { return IntValue; }
		if (Type == EType::Double) { return static_cast<int64_t>(DoubleValue); }
		return Fallback;
	}

	double FJsonValue::AsDouble(double Fallback) const
	{
		if (Type == EType::Double) { return DoubleValue; }
		if (Type == EType::Int) { return static_cast<double>(IntValue); }
		return Fallback;
	}

	const std::string& FJsonValue::AsString() const
	{
		return Type == EType::String ? StringValue : EmptyString;
	}

	const FJsonArray& FJsonValue::AsArray() const
	{
		return Type == EType::Array ? ArrayValue : EmptyArray;
	}

	const FJsonObjectMap& FJsonValue::AsObject() const
	{
		return Type == EType::Object ? ObjectValue : EmptyObject;
	}

	FJsonArray& FJsonValue::MutableArray()
	{
		if (Type != EType::Array)
		{
			*this = FJsonValue(FJsonArray{});
		}
		return ArrayValue;
	}

	FJsonObjectMap& FJsonValue::MutableObject()
	{
		if (Type != EType::Object)
		{
			*this = FJsonValue(FJsonObjectMap{});
		}
		return ObjectValue;
	}

	const FJsonValue& FJsonValue::Field(const std::string& Key) const
	{
		if (Type != EType::Object)
		{
			return NullValue;
		}
		const auto It = ObjectValue.find(Key);
		return It == ObjectValue.end() ? NullValue : It->second;
	}

	bool FJsonValue::HasField(const std::string& Key) const
	{
		return Type == EType::Object && ObjectValue.find(Key) != ObjectValue.end();
	}

	void FJsonValue::SetField(const std::string& Key, FJsonValue Value)
	{
		MutableObject()[Key] = std::move(Value);
	}

	void FJsonValue::Push(FJsonValue Value)
	{
		MutableArray().push_back(std::move(Value));
	}

	bool FJsonValue::operator==(const FJsonValue& Other) const
	{
		if (Type != Other.Type)
		{
			return false;
		}
		switch (Type)
		{
		case EType::Null:   return true;
		case EType::Bool:   return BoolValue == Other.BoolValue;
		case EType::Int:    return IntValue == Other.IntValue;
		case EType::Double: return DoubleValue == Other.DoubleValue;
		case EType::String: return StringValue == Other.StringValue;
		case EType::Array:  return ArrayValue == Other.ArrayValue;
		case EType::Object: return ObjectValue == Other.ObjectValue;
		}
		return false;
	}

	void FJsonValue::Serialise(std::string& Out) const
	{
		switch (Type)
		{
		case EType::Null:
			Out += "null";
			break;
		case EType::Bool:
			Out += BoolValue ? "true" : "false";
			break;
		case EType::Int:
			Out += std::to_string(IntValue);
			break;
		case EType::Double:
		{
			if (!std::isfinite(DoubleValue))
			{
				// JSON has no NaN or Infinity. Writing one produces a body the gateway rejects with
				// a parse error that names no field, so null is the less confusing failure.
				Out += "null";
				break;
			}
			// A whole double is written without a trailing ".0". The gateway distinguishes 12 from
			// 12.0 on the wire, and a row id or a level written as a decimal is a different request.
			if (DoubleValue == static_cast<double>(static_cast<int64_t>(DoubleValue)))
			{
				Out += std::to_string(static_cast<int64_t>(DoubleValue));
				break;
			}
			char Buffer[40];
			std::snprintf(Buffer, sizeof(Buffer), "%.17g", DoubleValue);
			Out += Buffer;
			break;
		}
		case EType::String:
			Out += EscapeJsonString(StringValue);
			break;
		case EType::Array:
		{
			Out.push_back('[');
			bool bFirst = true;
			for (const FJsonValue& Item : ArrayValue)
			{
				if (!bFirst) { Out.push_back(','); }
				bFirst = false;
				Item.Serialise(Out);
			}
			Out.push_back(']');
			break;
		}
		case EType::Object:
		{
			// An object, always - never an array of {"Key":..,"Value":..} pairs. std::map is a
			// sequence of pairs as well as a mapping, which is exactly the ambiguity that produces
			// that bug in other languages, so this is spelled out rather than delegated.
			Out.push_back('{');
			bool bFirst = true;
			for (const auto& Pair : ObjectValue)
			{
				if (!bFirst) { Out.push_back(','); }
				bFirst = false;
				Out += EscapeJsonString(Pair.first);
				Out.push_back(':');
				Pair.second.Serialise(Out);
			}
			Out.push_back('}');
			break;
		}
		}
	}

	std::string FJsonValue::ToString() const
	{
		std::string Out;
		Serialise(Out);
		return Out;
	}

	bool FJsonValue::Parse(const std::string& Text, FJsonValue& OutValue, std::string& OutError)
	{
		FParser Parser(Text);
		FJsonValue Value;
		if (!Parser.ParseValue(Value, 0))
		{
			OutError = Parser.Error;
			return false;
		}
		Parser.SkipWhitespace();
		if (Parser.Pos != Text.size())
		{
			OutError = "trailing content after JSON value at offset " + std::to_string(Parser.Pos);
			return false;
		}
		OutValue = std::move(Value);
		OutError.clear();
		return true;
	}
}
