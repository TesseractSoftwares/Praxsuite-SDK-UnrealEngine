// The Event Bus wire format: SignalR's JSON hub protocol, version 1.
//
// Portable C++17, no Unreal types - see Core/PraxJson.h for why. Everything here is pure and
// synchronous, so the offline suite exercises the whole protocol with no engine and no socket,
// which is exactly where the shared conformance contract's rules live.
//
// The protocol is spoken directly. There is no SignalR client for Unreal, and the surface used
// here is four message types wide.

#pragma once

#include <string>
#include <vector>

namespace Prax
{
	namespace BusWire
	{
		/** ASCII record separator. SignalR terminates every frame with it. */
		constexpr char RecordSeparator = '\x1e';

		/**
		 * The handshake, byte for byte. SignalR compares it literally - a trailing newline or a
		 * space after a colon fails it, with an error that does not say so.
		 */
		constexpr const char* HandshakeFrame = "{\"protocol\":\"json\",\"version\":1}";

		/** The hub's path. There is no workspace segment: the workspace comes from the token. */
		constexpr const char* BusPath = "/hubs/event-bus";

		constexpr int MessageInvocation = 1;
		constexpr int MessageCompletion = 3;
		constexpr int MessagePing = 6;
		constexpr int MessageClose = 7;

		/** Whole frames, plus whatever trailing fragment was left unparsed. */
		struct FFrames
		{
			std::vector<std::string> Frames;
			std::string Remainder;
		};

		/**
		 * Splits a received buffer into whole frames.
		 *
		 * Two things go wrong without this. One physical message can carry SEVERAL frames, so
		 * parsing the whole buffer as JSON fails exactly when traffic picks up - the load the bus
		 * exists for. And a transport may split one frame across two reads, so the tail is kept
		 * rather than parsed or discarded.
		 */
		FFrames SplitFrames(const std::string& Buffer);

		/** Wraps a frame for sending. */
		std::string Frame(const std::string& Payload);

		/**
		 * Folds a bus key the way the server does: the TOPIC segment to lowercase, the instance
		 * untouched.
		 *
		 * BusAddress::ForCaller folds the topic both when it resolves the topic and when it builds
		 * the SignalR group name, so "Office:hq" and "office:hq" are one bus. Folding the whole key
		 * instead would merge "office:HQ" and "office:hq", which are two genuinely different buses.
		 * Fold the same half the server folds and neither mistake is possible.
		 */
		std::string NormalizeBusKey(const std::string& BusKey);

		/**
		 * Rejects keys the server would reject anyway, without spending a round trip on it.
		 *
		 * Returns true and writes the normalised key; on failure returns false and writes a reason.
		 * A bool-and-out rather than an exception, because the rest of this core reports failures
		 * that way and Unreal projects are frequently built with exceptions disabled.
		 */
		bool CheckBusKey(const std::string& BusKey, std::string& OutKey, std::string& OutError);

		/**
		 * Builds an invocation frame. InvocationId is a STRING - SignalR matches completions on it
		 * by value, and a numeric id never matches.
		 *
		 * ArgumentsJson is spliced in verbatim as the arguments array, so the caller controls how
		 * its own payload is encoded.
		 */
		std::string BuildInvocation(const std::string& InvocationId, const std::string& Target,
									const std::string& ArgumentsJson);

		/** One peer's last known state within a bus. */
		struct FPeerState
		{
			std::string UserId;
			std::string Event;
			/** UNTRUSTED, and unparsed: the raw JSON another user sent. */
			std::string PayloadJson;
		};

		/** What the hub returns from JoinBus, Publish and LeaveBus, in one shape. */
		struct FBusResult
		{
			/**
			 * False for a policy rejection. Rejections arrive INSIDE a successful completion,
			 * which is why this is a field rather than a thrown error.
			 */
			bool bOk = true;

			/** One of the hub's error codes, or empty. */
			std::string Error;

			/** JoinBus: every peer's last retained message. Empty when the topic does not retain. */
			std::vector<FPeerState> Peers;

			/**
			 * Publish: how many OTHER connections it reached. Zero is success, not failure - it
			 * means the message went out and nobody was joined.
			 */
			int Recipients = 0;

			/**
			 * True when SignalR itself failed the call - a server fault rather than a policy
			 * decision. Kept apart because the two want different handling.
			 */
			bool bIsTransportError = false;
		};

		/**
		 * Reads a completion frame.
		 *
		 * The trap this exists for: the hub answers a REJECTED call with a SUCCESSFUL completion
		 * whose result carries ok:false. Code that only inspects SignalR's "error" field reports
		 * every denied join as a success. And LeaveBus is void, so its result is literally null.
		 */
		FBusResult ParseBusResult(const std::string& FrameJson);

		/** Negotiate: a zero-length POST with the end-user JWT as a bearer token. */
		std::string NegotiateUrl(const std::string& Host);

		/**
		 * The WebSocket URL, with the session token in the query string.
		 *
		 * This is the one deliberate exception to "credentials never go in a URL" (Core/PraxRoutes.h):
		 * the hub accepts access_token precisely because a browser WebSocket cannot set a header,
		 * and keeping one placement across every SDK is what makes the transport identical
		 * everywhere. It is a short-lived end-user token, never an API key - an API key does not
		 * authenticate this hub at all.
		 */
		std::string SocketUrl(const std::string& Host, const std::string& AccessToken);

		/**
		 * Turns a hub error code into a sentence worth reading. The codes are stable and are what
		 * callers should branch on; these strings are not.
		 */
		std::string DescribeError(const std::string& Code);
	}
}
