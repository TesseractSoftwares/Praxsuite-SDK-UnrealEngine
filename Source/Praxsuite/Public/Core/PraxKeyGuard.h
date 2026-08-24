// Credential handling and log scrubbing.
//
// Portable C++17, no Unreal types - see Core/PraxJson.h for why.
//
// A game client is the hardest case for credentials in the whole platform, which is why this is its
// own file with its own tests rather than a couple of ifs inside the transport.
//
// A shipped game is a program on someone else's computer. Strings inside it are readable with a hex
// editor, so a secret key in a packaged build is a published secret. The SDK therefore refuses one
// outright when configured client-side, with no override flag - an override is just a slower way of
// arriving at the same leak, and the person who sets it is never the person who regrets it.
//
// Two facts that follow from how the platform works, both of which have to be documented rather than
// hidden, because a reader who assumes otherwise designs the wrong thing:
//
//   Every credential carries BOTH halves. There is no publishable-only credential. Whatever table
//   scopes you grant a publishable key are reachable by anybody who has it.
//
//   GET /{workspaceId}/auth/config is unauthenticated, so a workspace id alone yields that
//   workspace's publishable key. A workspace id is therefore not a secret - but it is also not
//   harmless to publish, since it hands over the publishable key and everything scoped to it.

#pragma once

#include <string>

namespace Prax
{
	/** Which half of a credential a string looks like. */
	enum class EKeyKind
	{
		/** Not a Praxsuite key at all. */
		Unknown,

		/** pk_live_... - safe to ship in a client, within the limits above. */
		Publishable,

		/** sk_live_... - a server credential. Never in a shipped game. */
		Secret,
	};

	namespace KeyGuard
	{
		constexpr const char* PublishablePrefix = "pk_live_";
		constexpr const char* SecretPrefix = "sk_live_";

		EKeyKind Classify(const std::string& Credential);

		bool IsSecret(const std::string& Credential);
		bool IsPublishable(const std::string& Credential);

		/**
		 * Whether this credential may be used with bClientSide.
		 *
		 * Returns false for a secret key, and there is deliberately no way to say "yes I know".
		 */
		bool IsAllowedClientSide(const std::string& Credential);

		/**
		 * Redacts credentials, tokens and passwords from arbitrary text.
		 *
		 * Applied to every log line the SDK emits. The reason it is this broad rather than "do not
		 * log the key" is that keys arrive in logs indirectly and constantly: inside a serialised
		 * request body, in an error message quoting the offending header, in a crash dump of a
		 * config struct. Scrubbing at the point of output is the only place that catches all of them.
		 *
		 * A player's log file routinely ends up in a public bug report or a Discord channel, so this
		 * is not a theoretical exposure.
		 *
		 * Prefixes are kept and the remainder replaced, so a redacted line still says WHICH kind of
		 * credential was involved - which is most of the diagnostic value, with none of the secret.
		 */
		std::string Scrub(const std::string& Text);

		/**
		 * A short, safe identifier for a credential: its prefix plus the last four characters.
		 *
		 * For telling two configured keys apart in a log without printing either.
		 */
		std::string Fingerprint(const std::string& Credential);
	}
}
