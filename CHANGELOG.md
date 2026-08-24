# Changelog

## 1.0.0 — 2026-08-24

First release. Unreal Engine 5.3+, Blueprint and C++, Win64 / Mac / Linux / Android / iOS.

### What it does

- **Data** — read with a chained query builder, insert, update and delete, with row-level permissions
  enforced server-side.
- **Players** — email and password registration and login; the session token is held by the subsystem
  and attached to requests, never exposed to Blueprint.
- **Automations** — call a server-side endpoint and receive its payload unchanged.

### How it is built, and why that is unusual

The SDK is split into a **portable C++17 core** with no engine types, and a thin Unreal layer over it.

That is not a stylistic choice. An Unreal plugin can only be compiled by a licensed engine install of
around 100 GB, so CI cannot build one — meaning an SDK written entirely against engine types could
never be executed by an automated check, only read. Every rule in the Praxsuite conformance contract
exists because breaking it fails *silently*, which makes "it looks right" an unusable standard.

So the core carries everything that decides anything — the JSON codec, the PraxQL builder, filters,
error classification, credential handling, route construction, response envelopes — and **336 checks
run over it on every commit, under two compilers, with warnings as errors, then again under
AddressSanitizer and UndefinedBehaviorSanitizer.** A CI gate fails the build if an engine header or
type appears in the core, because the day that happens CI quietly stops being able to compile it.

The Unreal layer builds requests, sends them and converts types. Its honest gate is a human opening
the plugin in the editor, and the README says so rather than implying the whole plugin is tested.

### Behaviour worth knowing before you ship

- **Writes and endpoint calls are never retried.** A failed write may already have been applied, and
  retrying is how one purchase becomes two rows. Reads are retried with backoff.
- **Unscoped updates and deletes are refused before sending** — but the refusal still fires the
  completion delegate, because a Blueprint pin that never fires looks like a frozen game.
- **A secret key is refused in a client build,** with no override. Any string in a packaged game is
  readable.
- **The gateway host is required configuration.** Praxsuite runs several tiers and the wrong one
  returns a plain 404 that looks exactly like a missing table.
- **`Page.Total` is only meaningful when `Page.bHasTotal` is true** — otherwise 0 means "nobody
  asked", not "nothing matched".
- **An endpoint does not authenticate its caller.** Putting logic there makes it server-*executed*,
  not automatically server-*authoritative*.
- **Per-user isolation needs two workspace settings, not one.** Configuring only the row filter lets
  inserts succeed with a null owner that the filter then hides — the player saves and cannot read it
  back, with no error anywhere.

### Known gaps

- The Unreal layer has never been compiled by CI, for the reason above. Treat the first editor build
  of a release as the acceptance test.
- Blueprint rows carry string values, because a Blueprint map cannot hold a heterogeneous type. Use
  the C++ core for typed access.
- File upload and download are not exposed yet.
