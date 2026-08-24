// Log category.
//
// Part of the Unreal layer - CI cannot compile this. See PraxsuiteTypes.h.
//
// Every line the SDK logs goes through Prax::KeyGuard::Scrub first. That is not belt-and-braces: a
// player's log file routinely ends up attached to a bug report or pasted into a Discord channel, and
// credentials reach logs indirectly far more often than directly - inside a serialised request body,
// in an error message quoting the offending header, in a dump of a settings object. Scrubbing at the
// point of output is the only place that catches all of those.

#pragma once

#include "CoreMinimal.h"

PRAXSUITE_API DECLARE_LOG_CATEGORY_EXTERN(LogPraxsuite, Log, All);
