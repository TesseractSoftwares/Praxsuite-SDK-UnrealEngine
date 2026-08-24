// URL construction.
//
// Portable C++17, no Unreal types - see Core/PraxJson.h for why.
//
// Two things this file exists to get right.
//
// THE HOST IS NOT A CONSTANT. Praxsuite runs several independent tiers and a workspace lives on
// exactly one of them. Pointing at the wrong tier returns 404 - not "wrong host", not "unknown
// workspace", just a plain 404 that reads exactly like a missing table. A sibling SDK hardcoded the
// cloud host as its only default, so every dedicated-tier workspace using it 404s with no clue why.
// The host is therefore a required part of configuration, and the docs say what a 404 usually means.
//
// CREDENTIALS NEVER GO IN A URL. Not as a query parameter, not as a path segment. URLs are logged by
// proxies, kept in crash reports and pasted into bug threads, none of which is true of a header.
// There is deliberately no function here that accepts a credential.

#pragma once

#include <string>

namespace Prax
{
	namespace Routes
	{
		/** The multi-tenant cloud tier. A dedicated tier has its own host and this will 404. */
		constexpr const char* CloudHost = "https://gateway.praxsuite.com";

		/**
		 * Builds {host}/{workspaceId}/{route}.
		 *
		 * The short form, which the FrontDoor rewrites to the long /api/v1/gateway/... form itself.
		 * Trailing slashes on the host and leading slashes on the route are both tolerated, because
		 * a host pasted from a browser usually has one and a route written by hand usually has the
		 * other, and producing a double slash from that is a 404 nobody can see.
		 */
		std::string Build(const std::string& Host, const std::string& WorkspaceId,
						  const std::string& Route);

		std::string Query(const std::string& Host, const std::string& WorkspaceId);
		std::string Schema(const std::string& Host, const std::string& WorkspaceId);
		std::string Files(const std::string& Host, const std::string& WorkspaceId);

		/** /auth/{action} - e.g. "login", "register", "refresh", "config". */
		std::string Auth(const std::string& Host, const std::string& WorkspaceId,
						 const std::string& Action);

		/**
		 * /endpoint/{endpointId}.
		 *
		 * The path segment is the endpoint's GUID, taken from the workspace's API Gateway screen -
		 * not a human-readable slug, despite what several SDKs call the parameter.
		 */
		std::string Endpoint(const std::string& Host, const std::string& WorkspaceId,
							 const std::string& EndpointId);
	}
}
