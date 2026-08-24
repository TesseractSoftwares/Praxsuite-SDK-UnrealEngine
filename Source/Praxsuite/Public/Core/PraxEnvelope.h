// Response unwrapping.
//
// Portable C++17, no Unreal types - see Core/PraxJson.h for why.
//
// The gateway does not use one envelope. It uses THREE, and an SDK that assumes a single shape
// mis-parses two of them:
//
//   POST /{ws}/query        the body IS the result. {"data":[...],"meta":{...}}. Nothing to unwrap.
//   POST /{ws}/auth/*       a platform envelope. The payload is under .data, alongside isSuccess,
//                           message, errors and statusCode.
//   /{ws}/files/*           errors are a BARE STRING: {"error":"..."} rather than an object.
//
// And a fourth case that is not an envelope at all:
//
//   POST /{ws}/endpoint/{id}  returns the automation's own payload, RAW. Nothing is unwrapped.
//
// That last one has already bitten two sibling SDKs, which call an unwrap helper on endpoint
// responses. It is harmless only while an automation's payload happens to have no top-level "data"
// key - and {"ok":true,"data":{...}} is an entirely ordinary thing for an automation to return. The
// moment one does, those SDKs silently hand back the inner object and discard everything beside it.
// No error, just less data than the automation sent. Hence separate functions per route shape rather
// than one clever Unwrap() that guesses.

#pragma once

#include <string>
#include <vector>

#include "Core/PraxError.h"
#include "Core/PraxJson.h"

namespace Prax
{
	/** One page of query results. */
	struct FPraxPage
	{
		std::vector<FJsonValue> Rows;

		/** The limit the SERVER applied, which may be lower than the one requested. */
		int64_t Limit = 0;
		int64_t Offset = 0;

		/** Rows in this page. */
		int64_t Count = 0;

		/**
		 * Total matching rows - but ONLY when includeTotalCount was requested.
		 *
		 * bHasTotal distinguishes "nobody asked" from "zero matched". Collapsing those to a plain 0
		 * is a silent wrong answer, and reading the wrong field name is how a sibling SDK's Count()
		 * returned 0 for months without anyone noticing.
		 */
		int64_t Total = 0;
		bool bHasTotal = false;

		int64_t DurationMs = 0;
	};

	namespace Envelope
	{
		/**
		 * Reads a /query response.
		 *
		 * The body is the result - there is no envelope to strip. Reads meta.total, which is the
		 * field's real name; totalCount, totalRows and rowCount do not exist and reading one returns
		 * nothing while reporting zero.
		 */
		bool ReadQuery(int Status, const FJsonValue& Body, FPraxPage& OutPage, FPraxError& OutError);

		/**
		 * Reads an /auth/* response, returning the payload from under .data.
		 *
		 * This is the ONE route family whose payload is nested. Applying it elsewhere is the bug
		 * described at the top of this file.
		 */
		bool ReadAuth(int Status, const FJsonValue& Body, FJsonValue& OutData, FPraxError& OutError);

		/**
		 * Reads an /endpoint/{id} response.
		 *
		 * Returns the body EXACTLY as the automation produced it. Deliberately does not look for a
		 * .data field to unwrap, and this is the function whose whole job is to not do that.
		 */
		bool ReadEndpoint(int Status, const FJsonValue& Body, FJsonValue& OutResult,
						  FPraxError& OutError);

		/** Reads a /files response, whose error shape is a bare string. */
		bool ReadFiles(int Status, const FJsonValue& Body, FJsonValue& OutResult,
					   FPraxError& OutError);
	}
}
