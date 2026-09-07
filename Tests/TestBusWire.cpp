// The Event Bus half of the shared conformance contract.
//
// Every case here was either measured against the live hub on 2026-09-07 or exists because getting
// it wrong produces silently wrong behaviour rather than a crash - a rejection read as a success, a
// message delivered to the wrong bus, two peers who never see each other.
//
// Offline, like the rest of the suite: no engine, no socket, no workspace.

#include "PraxTestHarness.h"

#include "Core/PraxBusWire.h"

#include <string>

using Prax::BusWire::FBusResult;
using Prax::BusWire::FFrames;

namespace
{
	/** The record separator, as a string, so frames read naturally in the cases below. */
	const std::string RS(1, Prax::BusWire::RecordSeparator);

	std::string Normalize(const char* Key)
	{
		return Prax::BusWire::NormalizeBusKey(Key);
	}
}

void TestBusWire()
{
	PraxTest::Section("event bus - bus keys");
	{
		// Fold the whole key and office:HQ merges with office:hq, two genuinely different buses.
		// Fold neither and two peers resolve the same topic, are both admitted, and silently never
		// see each other - a failure indistinguishable from a client bug.
		PRAX_STR_EQ(Normalize("Office:HQ"), "office:HQ",
					"the topic folds to lowercase and the instance does not");
		PRAX_STR_EQ(Normalize("channel:9f1c0f2e"), "channel:9f1c0f2e",
					"an already-lowercase key is unchanged");
		PRAX_STR_EQ(Normalize("LOBBY"), "lobby", "a key with no instance still folds");
		PRAX_STR_EQ(Normalize("  office:hq  "), "office:hq", "surrounding whitespace is trimmed");
		PRAX_STR_EQ(Normalize("user:self"), "user:self", "user:self passes through untouched");

		std::string Key;
		std::string Error;
		PRAX_CHECK(!Prax::BusWire::CheckBusKey("   ", Key, Error),
				   "an empty key is refused before the round trip");
		PRAX_CHECK(!Prax::BusWire::CheckBusKey("x:ws:something", Key, Error),
				   "a key containing ws: is refused");
		PRAX_CHECK(Prax::BusWire::CheckBusKey("office:hq", Key, Error) && Key == "office:hq",
				   "a valid key passes and comes back normalised");
	}

	PraxTest::Section("event bus - frames");
	{
		// SignalR compares the handshake literally: a space after a colon fails it, with an error
		// that does not say so.
		PRAX_STR_EQ(std::string(Prax::BusWire::HandshakeFrame),
					"{\"protocol\":\"json\",\"version\":1}", "the handshake is byte-exact");

		const FFrames One = Prax::BusWire::SplitFrames("{\"type\":6}" + RS);
		PRAX_EQ(One.Frames.size(), static_cast<size_t>(1), "a single frame splits to one message");
		PRAX_CHECK(One.Remainder.empty(), "a complete buffer leaves no remainder");

		// One physical message can carry several frames. Parsing the whole buffer breaks under
		// exactly the load the bus exists for.
		const FFrames Two = Prax::BusWire::SplitFrames(
			"{\"type\":6}" + RS + "{\"type\":3,\"invocationId\":\"1\",\"result\":null}" + RS);
		PRAX_EQ(Two.Frames.size(), static_cast<size_t>(2), "two coalesced frames split into two");
		PRAX_STR_EQ(Two.Frames[0], "{\"type\":6}", "the first coalesced frame is intact");

		const FFrames Partial =
			Prax::BusWire::SplitFrames("{\"type\":6}" + RS + "{\"type\":3,\"invoca");
		PRAX_EQ(Partial.Frames.size(), static_cast<size_t>(1),
				"a trailing partial frame is not parsed");
		PRAX_STR_EQ(Partial.Remainder, "{\"type\":3,\"invoca",
					"a trailing partial frame is buffered rather than discarded");

		const FFrames Empty = Prax::BusWire::SplitFrames(RS + "{\"type\":6}" + RS);
		PRAX_EQ(Empty.Frames.size(), static_cast<size_t>(1), "an empty segment is dropped");

		PRAX_STR_EQ(Prax::BusWire::Frame("x"), "x" + RS, "a sent frame carries the separator");
	}

	PraxTest::Section("event bus - invocations");
	{
		const std::string Join =
			Prax::BusWire::BuildInvocation("1", "JoinBus", "[\"office:hq\",null]");

		// invocationId is a STRING: SignalR matches completions on it by value, and a numeric id
		// never matches.
		PRAX_STR_EQ(Join,
					"{\"type\":1,\"invocationId\":\"1\",\"target\":\"JoinBus\","
					"\"arguments\":[\"office:hq\",null]}",
					"join sends an explicit null ticket, with a string invocation id");

		PRAX_STR_EQ(Prax::BusWire::BuildInvocation("2", "LeaveBus", "[\"office:hq\"]"),
					"{\"type\":1,\"invocationId\":\"2\",\"target\":\"LeaveBus\","
					"\"arguments\":[\"office:hq\"]}",
					"leave carries only the key");

		PRAX_STR_EQ(Prax::BusWire::BuildInvocation("3", "Publish", ""),
					"{\"type\":1,\"invocationId\":\"3\",\"target\":\"Publish\",\"arguments\":[]}",
					"an absent argument list serialises as an empty array, not as null");
	}

	PraxTest::Section("event bus - completions");
	{
		const FBusResult Joined = Prax::BusWire::ParseBusResult(
			"{\"type\":3,\"invocationId\":\"1\",\"result\":{\"ok\":true,\"error\":null,"
			"\"peers\":[{\"userId\":\"u1\",\"event\":\"move\",\"payload\":{\"x\":1}}]}}");
		PRAX_CHECK(Joined.bOk, "a join completion is ok");
		PRAX_EQ(Joined.Peers.size(), static_cast<size_t>(1),
				"a join carries every peer's retained state");
		PRAX_STR_EQ(Joined.Peers[0].Event, "move", "a retained peer keeps its event name");
		PRAX_STR_EQ(Joined.Peers[0].PayloadJson, "{\"x\":1}",
					"a retained payload is kept as raw JSON, unparsed and untrusted");

		// A REJECTION arrives inside a SUCCESSFUL completion. Code that only inspects SignalR's
		// own error field reports every denied join as a success.
		const FBusResult Denied = Prax::BusWire::ParseBusResult(
			"{\"type\":3,\"invocationId\":\"3\",\"result\":{\"ok\":false,"
			"\"error\":\"unknown_topic\",\"peers\":[]}}");
		PRAX_CHECK(!Denied.bOk, "a rejection is not reported as a success");
		PRAX_STR_EQ(Denied.Error, "unknown_topic", "the rejection carries the hub's code");
		PRAX_CHECK(!Denied.bIsTransportError,
				   "a policy rejection is not mistaken for a server fault");

		// Zero recipients means it went out and nobody was joined. Treating it as a failure makes
		// every empty room look broken.
		const FBusResult Published = Prax::BusWire::ParseBusResult(
			"{\"type\":3,\"invocationId\":\"2\",\"result\":{\"ok\":true,\"error\":null,"
			"\"recipients\":0}}");
		PRAX_CHECK(Published.bOk && Published.Recipients == 0,
				   "zero recipients is success, not failure");

		// LeaveBus is void: its result is literally null.
		const FBusResult Void =
			Prax::BusWire::ParseBusResult("{\"type\":3,\"invocationId\":\"7\",\"result\":null}");
		PRAX_CHECK(Void.bOk && Void.Peers.empty(),
				   "a void result is ok rather than a failed field read");

		const FBusResult Fault = Prax::BusWire::ParseBusResult(
			"{\"type\":3,\"invocationId\":\"9\",\"error\":\"An unexpected error occurred.\"}");
		PRAX_CHECK(!Fault.bOk && Fault.bIsTransportError,
				   "a hub fault is kept apart from a policy rejection");

		// The codec reports malformed input rather than throwing, and so must this.
		const FBusResult Malformed = Prax::BusWire::ParseBusResult("{not json");
		PRAX_CHECK(!Malformed.bOk && Malformed.bIsTransportError,
				   "a malformed frame is a transport failure, not a crash");
	}

	PraxTest::Section("event bus - urls");
	{
		// The workspace comes from the token; adding a segment to this path 404s.
		PRAX_STR_EQ(Prax::BusWire::NegotiateUrl("https://gw.test"),
					"https://gw.test/hubs/event-bus/negotiate?negotiateVersion=1",
					"the hub has no workspace segment");
		PRAX_STR_EQ(Prax::BusWire::NegotiateUrl("https://gw.test/"),
					"https://gw.test/hubs/event-bus/negotiate?negotiateVersion=1",
					"a trailing slash on the host does not produce a double slash");

		PRAX_STR_EQ(Prax::BusWire::SocketUrl("https://gw.test", "abc.def"),
					"wss://gw.test/hubs/event-bus?access_token=abc.def",
					"the socket url upgrades the scheme and carries the token");
		PRAX_STR_EQ(Prax::BusWire::SocketUrl("http://localhost:5000", "t"),
					"ws://localhost:5000/hubs/event-bus?access_token=t",
					"a plaintext host produces a plaintext socket url");
		PRAX_STR_EQ(Prax::BusWire::SocketUrl("https://gw.test", "a+b/c"),
					"wss://gw.test/hubs/event-bus?access_token=a%2Bb%2Fc",
					"a token is percent-encoded, so a JWT's padding cannot break the query");
	}

	PraxTest::Section("event bus - error descriptions");
	{
		const char* Codes[] = {
			"invalid_bus_key", "unknown_topic", "denied", "invalid_ticket",
			"bus_full_or_too_many_buses", "not_a_member", "invalid_event_name",
			"payload_too_large", "rate_limited",
		};

		bool bAllDescribed = true;
		for (const char* Code : Codes)
		{
			if (Prax::BusWire::DescribeError(Code).size() <= 20)
			{
				bAllDescribed = false;
			}
		}
		PRAX_CHECK(bAllDescribed, "every hub error code has a sentence worth reading");
		PRAX_STR_EQ(Prax::BusWire::DescribeError("something_new"), "something_new",
					"an unrecognised code is passed through rather than swallowed");
	}
}
