#include "Core/PraxKeyGuard.h"

#include <cctype>
#include <cstddef>
#include <string>

namespace Prax
{
	namespace
	{
		bool StartsWith(const std::string& Text, const char* Prefix)
		{
			const std::string P(Prefix);
			return Text.size() >= P.size() && Text.compare(0, P.size(), P) == 0;
		}

		bool IsKeyBodyChar(char C)
		{
			const unsigned char U = static_cast<unsigned char>(C);
			return std::isalnum(U) != 0 || C == '_' || C == '-';
		}

		bool IsJwtChar(char C)
		{
			const unsigned char U = static_cast<unsigned char>(C);
			return std::isalnum(U) != 0 || C == '_' || C == '-' || C == '.';
		}

		/**
		 * Replaces the body of every occurrence of Prefix with "[redacted]", keeping the prefix.
		 *
		 * Scanning for the prefix anywhere in the text rather than matching a whole-token pattern:
		 * keys turn up embedded in JSON bodies, inside quotes, after an equals sign and in the middle
		 * of prose, and a pattern anchored to token boundaries misses most of those.
		 */
		void ScrubPrefixed(std::string& Text, const char* Prefix)
		{
			const std::string P(Prefix);
			size_t At = 0;
			while ((At = Text.find(P, At)) != std::string::npos)
			{
				size_t End = At + P.size();
				while (End < Text.size() && IsKeyBodyChar(Text[End]))
				{
					++End;
				}
				// Only redact if there is actually a body. "pk_live_" alone is a prefix in prose or
				// documentation, and mangling it makes error messages about key format unreadable.
				if (End > At + P.size())
				{
					Text.replace(At + P.size(), End - (At + P.size()), "[redacted]");
					At = At + P.size() + 10; // past "[redacted]"
				}
				else
				{
					At = End;
				}
			}
		}

		/**
		 * Redacts JWTs, which start "eyJ" - the base64 of '{"'.
		 *
		 * Session tokens are as sensitive as the key: one identifies the workspace, the other
		 * impersonates a player. Both end up in the same logs.
		 */
		void ScrubJwts(std::string& Text)
		{
			const std::string Marker = "eyJ";
			size_t At = 0;
			while ((At = Text.find(Marker, At)) != std::string::npos)
			{
				// Must be at a token boundary, or every word containing "eyj" would match.
				if (At > 0 && IsJwtChar(Text[At - 1]))
				{
					At += Marker.size();
					continue;
				}
				size_t End = At;
				int Dots = 0;
				while (End < Text.size() && IsJwtChar(Text[End]))
				{
					if (Text[End] == '.') { ++Dots; }
					++End;
				}
				// A JWT has three dot-separated parts. Requiring at least one dot and reasonable
				// length avoids redacting an ordinary word that happens to begin "eyJ".
				if (Dots >= 1 && (End - At) >= 16)
				{
					Text.replace(At, End - At, "eyJ[redacted]");
					At += 13;
				}
				else
				{
					At = End > At ? End : At + Marker.size();
				}
			}
		}

		/**
		 * Redacts the value of a sensitive JSON field, e.g. "password":"hunter2".
		 *
		 * Needed because these carry no recognisable prefix - a password is just a string, and a
		 * refresh token is opaque. Without this, logging a request body leaks them in full.
		 */
		void ScrubJsonField(std::string& Text, const char* FieldName)
		{
			const std::string Needle = std::string("\"") + FieldName + "\"";
			size_t At = 0;
			while ((At = Text.find(Needle, At)) != std::string::npos)
			{
				size_t Cursor = At + Needle.size();
				while (Cursor < Text.size() && (Text[Cursor] == ' ' || Text[Cursor] == '\t')) { ++Cursor; }
				if (Cursor >= Text.size() || Text[Cursor] != ':')
				{
					At = Cursor;
					continue;
				}
				++Cursor;
				while (Cursor < Text.size() && (Text[Cursor] == ' ' || Text[Cursor] == '\t')) { ++Cursor; }
				if (Cursor >= Text.size() || Text[Cursor] != '"')
				{
					// A non-string value (null, a number) carries nothing worth hiding.
					At = Cursor;
					continue;
				}
				const size_t ValueStart = Cursor + 1;
				size_t ValueEnd = ValueStart;
				while (ValueEnd < Text.size() && Text[ValueEnd] != '"')
				{
					// Skip an escaped character so a \" inside the value does not end it early.
					if (Text[ValueEnd] == '\\' && ValueEnd + 1 < Text.size()) { ++ValueEnd; }
					++ValueEnd;
				}
				if (ValueEnd >= Text.size())
				{
					break;
				}
				Text.replace(ValueStart, ValueEnd - ValueStart, "[redacted]");
				At = ValueStart + 10;
			}
		}
	}

	namespace KeyGuard
	{
		EKeyKind Classify(const std::string& Credential)
		{
			if (StartsWith(Credential, SecretPrefix))      { return EKeyKind::Secret; }
			if (StartsWith(Credential, PublishablePrefix)) { return EKeyKind::Publishable; }
			return EKeyKind::Unknown;
		}

		bool IsSecret(const std::string& Credential)
		{
			return Classify(Credential) == EKeyKind::Secret;
		}

		bool IsPublishable(const std::string& Credential)
		{
			return Classify(Credential) == EKeyKind::Publishable;
		}

		bool IsAllowedClientSide(const std::string& Credential)
		{
			// Only a secret key is refused. An unrecognised string is allowed through so that a
			// custom or future credential format is not blocked by this SDK guessing - the gateway
			// is the authority on whether a credential is valid, and a 401 says so clearly. A secret
			// key, by contrast, is recognisable and catastrophic, which is why it is the one case
			// decided locally.
			return !IsSecret(Credential);
		}

		std::string Scrub(const std::string& Text)
		{
			std::string Out = Text;
			ScrubPrefixed(Out, SecretPrefix);
			ScrubPrefixed(Out, PublishablePrefix);
			ScrubJwts(Out);
			for (const char* Field : {"password", "newPassword", "currentPassword",
									  "accessToken", "refreshToken", "sessionToken",
									  "apiKey", "credential", "x-api-key", "Authorization"})
			{
				ScrubJsonField(Out, Field);
			}
			return Out;
		}

		std::string Fingerprint(const std::string& Credential)
		{
			const EKeyKind Kind = Classify(Credential);
			const char* Prefix = Kind == EKeyKind::Secret ? SecretPrefix
							   : Kind == EKeyKind::Publishable ? PublishablePrefix
							   : "";
			if (Credential.size() < 4)
			{
				// Too short to show anything from without effectively showing all of it.
				return std::string(Prefix) + "...";
			}
			return std::string(Prefix) + "..." + Credential.substr(Credential.size() - 4);
		}
	}
}
