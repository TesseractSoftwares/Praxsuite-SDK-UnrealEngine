// Error classification, credential handling, routes, and the three response envelopes.
//
// Most of what is asserted here is about telling apart two things that look identical on the wire.
// Quota and rate limit are both HTTP 429. A wrong host and a missing row are both 404. "Nobody asked
// for a count" and "the count is zero" are both a falsy number. In each case the wrong reading
// produces no error at all - just a wasted retry loop, an unexplained empty result, or a silently
// wrong total.

#include "PraxTestHarness.h"

#include "Core/PraxEnvelope.h"
#include "Core/PraxError.h"
#include "Core/PraxJson.h"
#include "Core/PraxKeyGuard.h"
#include "Core/PraxRoutes.h"

using Prax::EKeyKind;
using Prax::EPraxErrorCode;
using Prax::FJsonValue;
using Prax::FPraxError;
using Prax::FPraxPage;

namespace
{
	FJsonValue ParseOrDie(const std::string& Text)
	{
		FJsonValue Value;
		std::string Error;
		if (!FJsonValue::Parse(Text, Value, Error))
		{
			std::printf("   !! test fixture did not parse: %s\n", Error.c_str());
		}
		return Value;
	}

	const char* CodeName(EPraxErrorCode In) { return FPraxError::CodeToString(In); }

	/**
	 * Fake credentials, ASSEMBLED rather than written out whole.
	 *
	 * This repository is mirrored to a public GitHub repo, so CI greps every file for anything
	 * shaped like a live secret key. A test fixture written literally matches that pattern and fails
	 * the build - which it duly did. Weakening the gate to accommodate tests would be the wrong fix:
	 * it exists because a real key committed here reaches every consumer, and "except in tests" is
	 * exactly where someone would eventually paste one.
	 *
	 * Building the string from the SDK's own prefix constant also means these fixtures cannot drift
	 * out of step with the prefix the SDK actually checks for.
	 */
	std::string FakeSecret(const std::string& Body)
	{
		return std::string(Prax::KeyGuard::SecretPrefix) + Body;
	}
}

void TestErrors()
{
	PraxTest::Section("Errors - 429 means two opposite things");
	{
		// The distinction the whole taxonomy exists for. Same status, same shape, opposite handling.
		const FPraxError Rate = FPraxError::FromResponse(429,
			ParseOrDie("{\"error\":{\"code\":\"RATE_LIMIT_EXCEEDED\",\"message\":\"slow down\"}}"));
		PRAX_STR_EQ(CodeName(Rate.Code), "RATE_LIMIT_EXCEEDED", "rate limit is classified");
		PRAX_CHECK(Rate.IsRetryable(), "and IS retryable");

		const FPraxError Quota = FPraxError::FromResponse(429,
			ParseOrDie("{\"error\":{\"code\":\"QUOTA_EXCEEDED\",\"message\":\"out of calls\"}}"));
		PRAX_STR_EQ(CodeName(Quota.Code), "QUOTA_EXCEEDED", "quota is classified");
		PRAX_CHECK(!Quota.IsRetryable(),
				   "and is NOT retryable - retrying an exhausted allowance cannot succeed");

		const FPraxError Egress = FPraxError::FromResponse(429,
			ParseOrDie("{\"error\":{\"code\":\"EGRESS_LIMIT_EXCEEDED\",\"message\":\"too much data\"}}"));
		PRAX_CHECK(!Egress.IsRetryable(), "egress limit is not retryable either");

		PRAX_EQ(Quota.Status, 429, "the status is preserved");
		PRAX_STR_EQ(Quota.Message, "out of calls", "and so is the message");
	}

	PraxTest::Section("Errors - the rest of the taxonomy");
	{
		struct FCase { int Status; const char* Body; const char* Expect; bool bRetryable; };
		const FCase Cases[] = {
			{401, "{\"error\":{\"code\":\"UNAUTHORIZED\"}}",    "UNAUTHORIZED",    false},
			{403, "{\"error\":{\"code\":\"FORBIDDEN\"}}",       "FORBIDDEN",       false},
			{400, "{\"error\":{\"code\":\"INVALID_REQUEST\"}}", "INVALID_REQUEST", false},
			{400, "{\"error\":{\"code\":\"INVALID_REFS\"}}",    "INVALID_REFS",    false},
			{500, "{\"error\":{\"code\":\"\"}}",                "SERVER_ERROR",    true},
			{503, "{\"error\":{\"code\":\"\"}}",                "SERVER_ERROR",    true},
			{404, "{\"error\":{\"code\":\"\"}}",                "NOT_FOUND",       false},
		};
		for (const FCase& C : Cases)
		{
			const FPraxError Error = FPraxError::FromResponse(C.Status, ParseOrDie(C.Body));
			PRAX_STR_EQ(CodeName(Error.Code), C.Expect, "classifies by code or status");
			PRAX_EQ(Error.IsRetryable(), C.bRetryable, "and gets retryability right");
		}

		PRAX_CHECK(FPraxError::MakeNetwork("no route to host").IsRetryable(),
				   "a network failure is retryable");
		PRAX_CHECK(FPraxError::MakeTimeout("took too long").IsRetryable(), "a timeout is retryable");
		PRAX_CHECK(!FPraxError::MakeValidation("UNSCOPED_MUTATION", "no").IsRetryable(),
				   "an SDK-side validation failure is not retryable");

		// An unclassifiable failure must not be retried. Looping on something we could not identify
		// is how one bad request becomes sustained load against production.
		const FPraxError Weird = FPraxError::FromResponse(418, ParseOrDie("{}"));
		PRAX_STR_EQ(CodeName(Weird.Code), "UNKNOWN", "an unmapped status is Unknown");
		PRAX_CHECK(!Weird.IsRetryable(), "and is not retried");
	}

	PraxTest::Section("Errors - a 429 with no code resolves to the retryable reading");
	{
		// Deliberate direction. Guessing quota would make the SDK abandon a transient throttle and
		// report an unfixable error to a player; guessing rate limit costs a few backed-off retries.
		const FPraxError Bare = FPraxError::FromResponse(429, ParseOrDie("{}"));
		PRAX_STR_EQ(CodeName(Bare.Code), "RATE_LIMIT_EXCEEDED", "an uncoded 429 is a rate limit");
		PRAX_CHECK(Bare.IsRetryable(), "and therefore retryable");
	}

	PraxTest::Section("Errors - all three gateway error shapes are understood");
	{
		// /query - an object with code, message and details.
		const FPraxError FromQuery = FPraxError::FromResponse(400, ParseOrDie(
			"{\"error\":{\"code\":\"INVALID_REQUEST\",\"message\":\"bad op\","
			"\"details\":[\"operator 'startsWith' is not supported\"]}}"));
		PRAX_STR_EQ(FromQuery.Message, "bad op", "query shape: message read");
		PRAX_EQ(static_cast<long long>(FromQuery.Details.size()), 1LL, "query shape: details read");

		// /files and /endpoint - a BARE STRING under "error", not an object. Assuming the object
		// shape here reads an empty message and loses the only explanation.
		const FPraxError FromFiles = FPraxError::FromResponse(400,
			ParseOrDie("{\"error\":\"file too large\"}"));
		PRAX_STR_EQ(FromFiles.Message, "file too large", "bare-string shape: message read");

		// /auth/* - the platform envelope, with failures in "errors" rather than "details".
		const FPraxError FromAuth = FPraxError::FromResponse(400, ParseOrDie(
			"{\"isSuccess\":false,\"message\":\"login failed\","
			"\"errors\":[\"invalid credentials\"],\"data\":null,\"statusCode\":400}"));
		PRAX_STR_EQ(FromAuth.Message, "login failed", "auth shape: message read");
		PRAX_EQ(static_cast<long long>(FromAuth.Details.size()), 1LL, "auth shape: errors read");

		// An auth failure with no message must still say something useful.
		const FPraxError AuthNoMessage = FPraxError::FromResponse(400, ParseOrDie(
			"{\"isSuccess\":false,\"message\":\"\",\"errors\":[\"email already registered\"]}"));
		PRAX_STR_EQ(AuthNoMessage.Message, "email already registered",
					"falls back to the first error when the message is empty");

		// A failing status with a body in none of the shapes must keep the body - it is the only
		// evidence of what went wrong.
		const FPraxError Odd = FPraxError::FromResponse(500, ParseOrDie("{\"weird\":true}"));
		PRAX_CHECK(Odd.Message.find("weird") != std::string::npos,
				   "an unrecognised body is preserved rather than discarded");
	}

	PraxTest::Section("Errors - a non-JSON body");
	{
		// A proxy returning an HTML error page is the common case here.
		const FPraxError Html = FPraxError::FromUnparseableResponse(502,
			"<html><head><title>502 Bad Gateway</title></head>");
		PRAX_STR_EQ(CodeName(Html.Code), "SERVER_ERROR", "a 502 with HTML is still a server error");
		PRAX_CHECK(Html.IsRetryable(), "and retryable");

		// A 2xx whose body is not JSON is a BROKEN response, not a success. Reporting success and
		// handing back an empty result is the silent-wrong-data failure to avoid.
		const FPraxError OkButNotJson = FPraxError::FromUnparseableResponse(200, "not json at all");
		PRAX_CHECK(OkButNotJson.IsError(), "a 200 with an unparseable body is an error");
		PRAX_STR_EQ(CodeName(OkButNotJson.Code), "UNKNOWN", "classified as Unknown");

		// A huge body must not be pasted whole into a log line.
		const std::string Huge(4000, 'x');
		const FPraxError Big = FPraxError::FromUnparseableResponse(500, Huge);
		PRAX_CHECK(Big.Message.size() < 400, "an enormous body is truncated for the log");
	}

	PraxTest::Section("Errors - an unrecognised server code keeps its name");
	{
		// Flattening a new server-side code to Unknown and discarding the string would throw away
		// the only clue about a code this SDK predates.
		const FPraxError Future = FPraxError::FromResponse(400,
			ParseOrDie("{\"error\":{\"code\":\"SOME_NEW_CODE\",\"message\":\"hmm\"}}"));
		PRAX_STR_EQ(Future.ServerCode, "SOME_NEW_CODE", "the raw server code is retained");
		PRAX_CHECK(Future.ToString().find("SOME_NEW_CODE") != std::string::npos,
				   "and appears in the log line");
	}
}

void TestKeyGuard()
{
	using namespace Prax::KeyGuard;

	PraxTest::Section("Credentials - classification");
	{
		PRAX_CHECK(Classify("sk_live_abc123") == EKeyKind::Secret, "sk_live_ is a secret key");
		PRAX_CHECK(Classify("pk_live_abc123") == EKeyKind::Publishable, "pk_live_ is publishable");
		PRAX_CHECK(Classify("something-else") == EKeyKind::Unknown, "anything else is unknown");
		PRAX_CHECK(Classify("") == EKeyKind::Unknown, "an empty string is unknown");

		// Substring, not prefix - must not match.
		PRAX_CHECK(Classify("xsk_live_abc") == EKeyKind::Unknown,
				   "a key prefix in the middle of a string is not a key");
	}

	PraxTest::Section("Credentials - a secret key is refused client-side, with no override");
	{
		// A shipped game is a program on someone else's computer; a string inside it is readable.
		PRAX_CHECK(!IsAllowedClientSide("sk_live_abc123"),
				   "a secret key is refused for client-side use");
		PRAX_CHECK(IsAllowedClientSide("pk_live_abc123"), "a publishable key is allowed");

		// An unrecognised credential is allowed through: the gateway is the authority on validity,
		// and a 401 says so clearly. Only the recognisable-and-catastrophic case is decided locally.
		PRAX_CHECK(IsAllowedClientSide("some-custom-token"),
				   "an unrecognised credential is not blocked by guesswork");
	}

	PraxTest::Section("Credentials - scrubbing (a player's log ends up in public)");
	{
		// Keys reach logs indirectly far more often than directly - inside a serialised body, in an
		// error quoting a header, in a dump of a config struct.
		const std::string Scrubbed = Scrub("calling with key " + FakeSecret("abcdef0123456789") + " now");
		PRAX_CHECK(Scrubbed.find("abcdef0123456789") == std::string::npos,
				   "a secret key body is removed");
		PRAX_CHECK(Scrubbed.find("sk_live_") != std::string::npos,
				   "but the prefix stays, so the line still says which kind of key it was");

		PRAX_CHECK(Scrub("pk_live_zzzz1111").find("zzzz1111") == std::string::npos,
				   "a publishable key is scrubbed too");

		// Embedded in JSON, which is how a logged request body looks.
		const std::string Body = Scrub("{\"x-api-key\":\"" + FakeSecret("deadbeefdeadbeef") + "\"}");
		PRAX_CHECK(Body.find("deadbeef") == std::string::npos, "a key inside JSON is scrubbed");

		// Two keys in one line - the scan must not stop at the first.
		const std::string Two = Scrub("a sk_live_1111aaaa b sk_live_2222bbbb c");
		PRAX_CHECK(Two.find("1111aaaa") == std::string::npos, "the first of two keys is scrubbed");
		PRAX_CHECK(Two.find("2222bbbb") == std::string::npos, "and so is the second");

		// A bare prefix in prose must survive, or messages about key format become unreadable.
		PRAX_STR_EQ(Scrub("keys start with sk_live_ as a rule"),
					"keys start with sk_live_ as a rule",
					"a bare prefix with no body is left alone");

		// A session token impersonates a player, so it is as sensitive as the key.
		const std::string Jwt = Scrub("Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjMifQ.abcdefghij");
		PRAX_CHECK(Jwt.find("eyJzdWIiOiIxMjMifQ") == std::string::npos, "a JWT payload is scrubbed");
		PRAX_CHECK(Jwt.find("eyJ") != std::string::npos, "with the marker kept for legibility");

		// A word beginning "eyJ" that is not a token must survive.
		PRAX_STR_EQ(Scrub("eyJ alone"), "eyJ alone", "a short eyJ token is not mistaken for a JWT");

		// These carry no prefix at all, so only field-name matching catches them.
		PRAX_CHECK(Scrub("{\"password\":\"hunter2\"}").find("hunter2") == std::string::npos,
				   "a password field is scrubbed");
		PRAX_CHECK(Scrub("{\"refreshToken\":\"opaque-value-here\"}").find("opaque-value") == std::string::npos,
				   "a refresh token is scrubbed");
		PRAX_CHECK(Scrub("{\"accessToken\": \"spaced-out\"}").find("spaced-out") == std::string::npos,
				   "whitespace before the value does not defeat it");

		// An escaped quote inside the value must not end it early, or the tail leaks.
		const std::string Escaped = Scrub("{\"password\":\"ab\\\"cd\",\"next\":1}");
		PRAX_CHECK(Escaped.find("ab") == std::string::npos || Escaped.find("cd") == std::string::npos,
				   "an escaped quote inside a secret value does not truncate the redaction");

		// Nothing sensitive must be altered.
		PRAX_STR_EQ(Scrub("ordinary text, nothing secret"), "ordinary text, nothing secret",
					"ordinary text passes through unchanged");
	}

	PraxTest::Section("Credentials - fingerprinting");
	{
		const std::string Print = Fingerprint("sk_live_abcdefgh1234");
		PRAX_CHECK(Print.find("1234") != std::string::npos, "the last four characters are shown");
		PRAX_CHECK(Print.find("abcdefgh") == std::string::npos, "the body is not");
		PRAX_CHECK(Fingerprint("abc").find("...") != std::string::npos,
				   "a too-short credential shows nothing from itself");
	}
}

void TestRoutes()
{
	using namespace Prax::Routes;
	const std::string Host = "https://gateway.praxsuite.com";
	const std::string Ws = "00000000-0000-4000-8000-0000000000ff";

	PraxTest::Section("Routes - the short form");
	{
		PRAX_STR_EQ(Query(Host, Ws), Host + "/" + Ws + "/query", "query route");
		PRAX_STR_EQ(Schema(Host, Ws), Host + "/" + Ws + "/schema", "schema route");
		PRAX_STR_EQ(Files(Host, Ws), Host + "/" + Ws + "/files", "files route");
		PRAX_STR_EQ(Auth(Host, Ws, "login"), Host + "/" + Ws + "/auth/login", "auth route");

		// The path segment is the endpoint's GUID, not a readable slug, despite the parameter name
		// several SDKs use for it.
		PRAX_STR_EQ(Endpoint(Host, Ws, "abc-123"), Host + "/" + Ws + "/endpoint/abc-123",
					"endpoint route takes an id");
	}

	PraxTest::Section("Routes - slashes, because a double slash is an invisible 404");
	{
		// A host pasted from a browser has a trailing slash; a route written by hand has a leading
		// one. Left alone that produces //, which the gateway answers with a 404 that reads exactly
		// like a missing workspace.
		PRAX_STR_EQ(Query(Host + "/", Ws), Host + "/" + Ws + "/query",
					"a trailing slash on the host is absorbed");
		PRAX_STR_EQ(Query(Host + "///", Ws), Host + "/" + Ws + "/query", "several are too");
		PRAX_STR_EQ(Build(Host, Ws, "/query"), Host + "/" + Ws + "/query",
					"a leading slash on the route is absorbed");
		PRAX_STR_EQ(Build(Host, "/" + Ws + "/", "query"), Host + "/" + Ws + "/query",
					"slashes around the workspace id are absorbed");
		PRAX_STR_EQ(Auth(Host, Ws, "/login/"), Host + "/" + Ws + "/auth/login",
					"and around an auth action");
	}

	PraxTest::Section("Routes - no credential may appear in a URL");
	{
		// URLs are logged by proxies, kept in crash reports and pasted into bug threads; headers are
		// not. There is deliberately no function here that takes a credential, so the strongest
		// available assertion is that a built URL contains only what was put in it.
		const std::string Url = Query(Host, Ws);
		PRAX_CHECK(Url.find("sk_live_") == std::string::npos, "no secret key in a built URL");
		PRAX_CHECK(Url.find("pk_live_") == std::string::npos, "no publishable key either");
		PRAX_CHECK(Url.find('?') == std::string::npos, "and no query string at all");
	}
}

void TestEnvelopes()
{
	using namespace Prax::Envelope;

	PraxTest::Section("Envelopes - /query is bare, and the count field is 'total'");
	{
		FPraxPage Page;
		FPraxError Error;
		const bool bOk = ReadQuery(200, ParseOrDie(
			"{\"data\":[{\"a\":1},{\"a\":2}],"
			"\"meta\":{\"limit\":50,\"offset\":0,\"count\":2,\"total\":137,\"durationMs\":9}}"),
			Page, Error);

		PRAX_CHECK(bOk, "a well-formed query response reads");
		PRAX_EQ(static_cast<long long>(Page.Rows.size()), 2LL, "rows are read from .data");
		PRAX_EQ(Page.Total, 137LL, "meta.total is read");
		PRAX_CHECK(Page.bHasTotal, "and marked present");

		// meta.limit is the limit the SERVER applied. Assuming the requested one was honoured turns
		// pagination into an infinite loop when a table scope clamps it.
		PRAX_EQ(Page.Limit, 50LL, "meta.limit is the server's limit");
		PRAX_EQ(Page.Count, 2LL, "meta.count is this page's row count");
	}

	PraxTest::Section("Envelopes - 'nobody asked' is not 'zero'");
	{
		FPraxPage Page;
		FPraxError Error;
		// No total field, because includeTotalCount was not requested.
		ReadQuery(200, ParseOrDie("{\"data\":[],\"meta\":{\"limit\":50,\"offset\":0,\"count\":0}}"),
				  Page, Error);
		PRAX_CHECK(!Page.bHasTotal, "an absent total is reported as absent");
		PRAX_EQ(Page.Total, 0LL, "with the value left at zero");

		// A real zero, explicitly requested, must be distinguishable from the above. Collapsing the
		// two is a silently wrong answer - and reading a field name that does not exist is how a
		// sibling SDK's Count() returned 0 for months.
		ReadQuery(200, ParseOrDie("{\"data\":[],\"meta\":{\"count\":0,\"total\":0}}"), Page, Error);
		PRAX_CHECK(Page.bHasTotal, "an explicit zero total IS present");
		PRAX_EQ(Page.Total, 0LL, "and reads as zero");

		// The wrong names must not be read.
		ReadQuery(200, ParseOrDie("{\"data\":[],\"meta\":{\"totalCount\":99,\"totalRows\":99,\"rowCount\":99}}"),
				  Page, Error);
		PRAX_CHECK(!Page.bHasTotal, "totalCount, totalRows and rowCount are ignored");
		PRAX_EQ(Page.Total, 0LL, "and do not become the total");
	}

	PraxTest::Section("Envelopes - a malformed 200 is an error, not an empty page");
	{
		FPraxPage Page;
		FPraxError Error;
		PRAX_CHECK(!ReadQuery(200, ParseOrDie("{\"meta\":{}}"), Page, Error),
				   "a 200 with no data array fails");
		PRAX_CHECK(Error.IsError(), "and reports an error rather than 'no rows matched'");

		PRAX_CHECK(!ReadQuery(429, ParseOrDie("{\"error\":{\"code\":\"QUOTA_EXCEEDED\"}}"), Page, Error),
				   "a failing status fails");
		PRAX_STR_EQ(CodeName(Error.Code), "QUOTA_EXCEEDED", "with the code classified");
	}

	PraxTest::Section("Envelopes - /auth/* IS unwrapped from .data");
	{
		FJsonValue Data;
		FPraxError Error;
		const bool bOk = ReadAuth(200, ParseOrDie(
			"{\"isSuccess\":true,\"message\":\"ok\",\"errors\":[],"
			"\"data\":{\"accessToken\":\"t\",\"userId\":\"u\"},\"statusCode\":200}"), Data, Error);

		PRAX_CHECK(bOk, "an auth response reads");
		PRAX_STR_EQ(Data.Field("userId").AsString(), "u", "the payload comes from under .data");
		PRAX_CHECK(!Data.HasField("isSuccess"), "and the envelope itself is not returned");

		// A 200 with isSuccess false is a failure. Checking only the status would wave it through
		// and hand the caller an empty payload as if it had worked.
		FJsonValue Ignored;
		PRAX_CHECK(!ReadAuth(200, ParseOrDie(
			"{\"isSuccess\":false,\"message\":\"bad password\",\"errors\":[],\"data\":null}"),
			Ignored, Error), "a 200 with isSuccess false is a failure");
		PRAX_STR_EQ(Error.Message, "bad password", "and the message survives");
	}

	PraxTest::Section("Envelopes - /endpoint is RAW (the regression two SDKs shipped)");
	{
		FJsonValue Result;
		FPraxError Error;

		// The case that matters. An automation returning a top-level "data" object is entirely
		// ordinary, and an SDK that unwraps here hands back the inner object while silently
		// discarding "ok" and everything else beside it. No error - just less data than was sent.
		const bool bOk = ReadEndpoint(200,
			ParseOrDie("{\"ok\":true,\"data\":{\"inner\":1},\"extra\":\"kept\"}"), Result, Error);

		PRAX_CHECK(bOk, "an endpoint response reads");
		PRAX_CHECK(Result.HasField("ok"), "the ok field survives");
		PRAX_CHECK(Result.HasField("extra"), "and so does everything else beside data");
		PRAX_STR_EQ(Result.Field("extra").AsString(), "kept", "with its value intact");
		PRAX_CHECK(Result.Field("data").IsObject(), "data is still nested, NOT hoisted");
		PRAX_EQ(Result.Field("data").Field("inner").AsInt(), 1LL, "and readable where it belongs");

		// A payload with no envelope-looking fields at all is the common case.
		ReadEndpoint(200, ParseOrDie("{\"kpis\":{\"players\":3},\"funnel\":[]}"), Result, Error);
		PRAX_CHECK(Result.HasField("kpis"), "an ordinary automation payload passes through whole");

		// A non-object payload must survive too - an automation may return an array or a scalar.
		ReadEndpoint(200, ParseOrDie("[1,2,3]"), Result, Error);
		PRAX_CHECK(Result.IsArray(), "an array payload is returned as an array");
		PRAX_EQ(static_cast<long long>(Result.AsArray().size()), 3LL, "with its items intact");
	}

	PraxTest::Section("Envelopes - /files errors are a bare string");
	{
		FJsonValue Result;
		FPraxError Error;
		PRAX_CHECK(!ReadFiles(413, ParseOrDie("{\"error\":\"file exceeds the limit\"}"), Result, Error),
				   "a files failure fails");
		PRAX_STR_EQ(Error.Message, "file exceeds the limit",
					"and the bare-string message is read, not lost to an object lookup");
	}
}
