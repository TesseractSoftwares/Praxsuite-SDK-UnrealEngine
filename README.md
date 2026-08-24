# Praxsuite SDK for Unreal Engine

Backend for your Unreal game — player accounts, saves, leaderboards, inventories and
server-authoritative logic. Blueprint and C++, Unreal 5.3+.

```cpp
UPraxsuiteSubsystem* Prax = GetGameInstance()->GetSubsystem<UPraxsuiteSubsystem>();

FPraxQueryDelegate OnDone;
OnDone.BindUFunction(this, FName("HandleLeaderboard"));

Prax->QueryTable(TEXT("Leaderboard"), {TEXT("Player"), TEXT("Score")},
                 TEXT("Score"), /*bDescending=*/true,
                 /*Limit=*/10, /*Offset=*/0, /*bIncludeTotalCount=*/false, OnDone);
```

In Blueprint: **Get Game Instance → Get Subsystem (Praxsuite) → Query Table**, with a delegate for
the result. Nothing here needs C++.

---

## What is tested, and what isn't

Stated first because it changes how you should read everything below.

An Unreal plugin can only be compiled by a licensed engine install of around 100 GB, and Epic's
container images require account linkage, so **no CI system here can build an Unreal plugin.** An SDK
written entirely against `FString` and `TArray` would therefore be unverifiable — readable, but never
executed.

So this SDK is split in two:

| | Compiled by CI | What it holds |
|---|---|---|
| `Source/Praxsuite/*/Core/` | **Yes** — two compilers, `-Werror`, ASan and UBSan, every commit | JSON codec, PraxQL builder, filters, error classification, credential handling, route building, response envelopes |
| Everything else | **No** | HTTP transport, Blueprint types, the subsystem |

**336 checks** run over the core on every commit. Everything that could be got *quietly* wrong lives
there. The engine layer is deliberately thin — it builds requests, sends them, converts types, and
decides nothing — and its honest gate is a human opening the plugin in Unreal.

If that split looks unusual for an Unreal plugin, it is. It is also why the core's behaviour is
something we can state rather than hope.

## Install

1. Copy this repository into your project's `Plugins/Praxsuite/` folder, or download the zip from
   [Releases](https://github.com/TesseractSoftwares/Praxsuite-SDK-UnrealEngine/releases/latest).
2. Restart the editor. It will offer to rebuild.
3. **Edit → Project Settings → Plugins → Praxsuite** and fill in the three connection fields.

Unreal **5.3 or newer**. Win64, Mac, Linux, Android and iOS.

## Configure

**Project Settings → Plugins → Praxsuite:**

| Field | Value |
|---|---|
| Workspace Id | from your workspace's **API Gateway** screen |
| Publishable Key | a `pk_live_...` key |
| Gateway Host | the host for **your tier** — see below |
| Client Side | leave **on** unless this build never reaches a player |

Or at runtime, for a game that fetches configuration at launch:

```cpp
Prax->Configure(WorkspaceId, Key, Host, /*bClientSide=*/true);
```

### The gateway host is not a formality

Praxsuite runs several independent tiers and your workspace exists on exactly one. **Pointing at the
wrong tier returns a plain 404** — indistinguishable from a missing table. If every call 404s, check
this field before anything else.

### Which key belongs in a game

**A publishable key. `pk_live_...`** A packaged game is a program on somebody else's computer, and
every string inside it is readable with a hex editor. A secret key in a shipped build is a published
secret. With **Client Side** on, the SDK refuses one outright — there is no override flag, because an
override is a slower route to the same leak.

Two things to know even with a publishable key:

**Every credential carries both halves.** There is no publishable-only credential, so whatever table
scopes you grant a key are reachable by anyone holding it. Scope narrowly.

**`/{workspaceId}/auth/config` is unauthenticated,** so a workspace id alone yields the publishable
key. A workspace id is not a secret — but it is not harmless to publish either.

---

## Reading data

```cpp
Prax->QueryTable(TEXT("Saves"), {}, TEXT("UpdatedDate"), true, 20, 0, false, OnDone);
```

```cpp
void AMyActor::HandleSaves(FPraxResult Result, FPraxPageResult Page)
{
    if (!Result.bSuccess)
    {
        UE_LOG(LogTemp, Warning, TEXT("%s"),
               *UPraxsuiteFunctionLibrary::DescribeResult(Result));
        return;
    }
    for (const FPraxRow& Row : Page.Rows)
    {
        const int32 Level = UPraxsuiteFunctionLibrary::GetInt(Row, TEXT("Level"));
    }
}
```

**Page.Limit is the limit the *server* applied,** which may be lower than the one you asked for — a
table scope can clamp it. Page against `Page.Limit`, not against your own request, or a clamped limit
turns pagination into an infinite loop.

**`Page.Total` is only meaningful when `Page.bHasTotal` is true.** Without `bIncludeTotalCount`, Total
is 0 because nothing was asked for — not because nothing matched. Reading it without checking turns
"nobody asked" into "zero rows", which is a wrong answer with no error attached.

**A limit of 0 does not mean "no rows".** The gateway clamps limit up to 1, so asking for zero returns
one row. To count, set `bIncludeTotalCount` and accept the single row.

### Rows in Blueprint are strings

A Blueprint map cannot hold a heterogeneous value, so `FPraxRow` carries strings and you convert on
read via `UPraxsuiteFunctionLibrary`. The typed getters return **the default you pass** for an absent
column rather than 0, so a graph can distinguish "no score" from "a score of zero".

Nested objects and arrays arrive as their JSON text rather than being flattened — flattening would
invent field names your table does not have. For typed access, use the C++ core directly:

```cpp
#include "Core/PraxQuery.h"   // Prax::FQueryBuilder, the full filter set, typed FJsonValue
```

## Writing data

```cpp
TMap<FString, FString> Values;
Values.Add(TEXT("Slot"), TEXT("1"));
Values.Add(TEXT("Level"), TEXT("12"));

Prax->InsertRow(TEXT("Saves"), Values, OnWritten);
Prax->UpdateRowById(TEXT("Saves"), RowId, Values, OnWritten);
Prax->DeleteRowById(TEXT("Saves"), RowId, OnWritten);
Prax->UpdateRowsWhereEquals(TEXT("Saves"), TEXT("Slot"), TEXT("1"), Values, OnWritten);
```

**There is no "update everything" node,** deliberately. An unscoped update rewrites every row in the
table and there is no undo, so the SDK refuses one — and rather than ship a node that always fails,
the scoped version takes the scope as arguments.

Blueprint string values are inferred to numbers and booleans where they look like one; without that,
an integer column would reject every write from Blueprint. A leading zero is kept as text, since
`007` is a chosen name or a padded id far more often than it is seven.

**Writes are never retried.** A failed insert may already have been applied — there is no way to tell
from the client — and retrying is how one purchase becomes two rows. Reads are retried with backoff.

## Automations

```cpp
Prax->CallEndpoint(EndpointId, TEXT("{\"score\":4500}"), OnEndpointDone);
```

`EndpointId` is the endpoint's **GUID** from the API Gateway screen, not a readable name.

The response is the automation's own payload **exactly as it produced it**, as JSON text. Nothing is
unwrapped — an automation returning `{"ok":true,"data":{...}}` reaches you whole.

The default timeout is 100 seconds, against 30 for everything else, because a Sync endpoint holds the
connection while its automation runs. Abandoning is not cancelling: the automation finishes anyway.

### An endpoint does not authenticate its caller

**A POST with no credential at all returns 200 and runs the automation.** That follows from what an
endpoint is — a webhook receiver, and Stripe cannot hold your workspace credential — but it means
putting logic in an endpoint makes it server-**executed**, not automatically
server-**authoritative**.

The authority comes from the endpoint verifying who called: a signature secret configured on it, or
the automation checking a verified claim from the session token this SDK attaches. **Design
accordingly for anything a cheater profits from.** "It runs on the server" is not the same as "a
player cannot forge it".

## Players

```cpp
Prax->Login(Email, Password, OnAuthDone);
Prax->Register(Email, Password, OnAuthDone);
Prax->Logout();
```

The session token is **not** exposed to Blueprint, only a `bHasToken` flag — a token in a Blueprint
variable gets printed by the next person debugging, and a player's log routinely ends up in a public
bug report. The subsystem attaches it to requests itself.

## Errors

Every call reports an `FPraxResult`. Branch on `Result.Error`, never on message text:

```cpp
switch (Result.Error)
{
case EPraxError::RateLimit:   // transient — back off and retry
case EPraxError::Quota:       // NOT transient — the workspace owner must upgrade
case EPraxError::Forbidden:   // a permissions problem; rewording the query will not help
default: break;
}
```

**Rate limit and quota both arrive as HTTP 429 and mean opposite things.** Retrying a rate limit is
correct; retrying an exhausted monthly quota cannot succeed and only drains a player's battery. That
is why they are separate values, and `Result.bRetryable` already encodes the difference.

A refusal by the SDK — an unscoped write, an empty `in` list, an operator that does not exist —
arrives as `EPraxError::Validation` **through the same delegate**, before anything is sent. A
guardrail that refused silently would look like a hang.

## Logging

`LogPraxsuite`, with credentials, JWTs and password fields scrubbed from every line. Turn on
**Verbose Logging** in Project Settings to see request and response bodies; they are scrubbed too.

Scrubbing is deliberately broad because keys reach logs indirectly far more often than directly —
inside a serialised body, in an error quoting a header, in a dump of a settings object.

---

## Per-user isolation needs TWO settings

The most damaging misconfiguration in the platform, and the one most likely to bite a game, so it is
worth stating plainly.

| Setting | Where | Value | Covers |
|---|---|---|---|
| Row filter | the role's **table** scope | `__SELF__` | select, update, delete |
| Default value template | the ownership **column**'s scope | `{{claim:sub}}` | insert |

The row filter cannot cover inserts, because an insert has no WHERE clause to constrain. Configure
only the row filter and **inserts succeed with a null owner, which the filter then hides** — the
player saves their progress and cannot read it back, with no error raised anywhere.

The default value template also blocks the client from setting the column at all. **That rejection is
the anti-tamper guarantee** — it is what stops one player writing a row owned by another. Do not work
around it by sending an owner id yourself; the SDK deliberately offers no way to.

---

## Conformance is the law

Praxsuite has SDKs in several languages. Where they touch the gateway they do **not** get to disagree.
A single normative contract defines the shared behaviour, and every SDK implements it identically:

1. **The contract is normative.** Where this SDK and the contract differ, this SDK is wrong.
2. **Every rule cites the backend source it derives from.** No rule rests on memory.
3. **Every rule exists because getting it wrong fails silently.** Wrong data, not an error.
4. **A behaviour change is a contract change first.** Not an implementation detail.

The contract is internal and deliberately has no public repository. Its value is that it is
authoritative for us, not that it is browsable — and it cites backend internals that are not ours to
publish. Everything a consumer needs is in this README.

What it pins down, and why each earned its place:

- **Thirteen operators.** Only the ones the parser accepts. A friendlier name is a runtime 400, so
  `StartsWith` and `IsNull` exist as helpers that compile down to `like` and `is`.
- **`meta.total`, never `totalCount`.** Reading the wrong name returns nothing and reports zero,
  silently, forever. One SDK shipped that for months.
- **Three response envelopes.** `/query` is bare, `/auth/{action}` nests under `.data`, `/files` errors
  are a bare string. Assuming one shape mis-parses the others.
- **Endpoint responses are raw.** Two sibling SDKs unwrap `.data` there, which silently discards
  everything beside it the moment an automation returns a `data` key.
- **`limit` is clamped up to a minimum of 1.** A zero-row count request quietly returns a row.
- **Unscoped updates and deletes refused before sending, synchronously.**
- **No client-supplied identity parameter.** The server ignores it, so it would read as a security
  boundary while being decorative.

Run the suite yourself — no engine, no workspace, no network, no credentials:

```bash
make -C Tests
```

336 checks. `make -C Tests sanitize` re-runs them under ASan and UBSan.

## API surface

| | |
|---|---|
| `UPraxsuiteSubsystem` | `Configure` `IsConfigured` `QueryTable` `QueryTableWhereEquals` `InsertRow` `UpdateRowById` `DeleteRowById` `UpdateRowsWhereEquals` `CallEndpoint` `Login` `Register` `Logout` `GetSession` `IsLoggedIn` |
| `UPraxsuiteFunctionLibrary` | `HasColumn` `GetString` `GetInt` `GetFloat` `GetBool` `GetRowId` `GetColumnNames` `DescribeResult` `ShouldRetry` |
| `Prax::FQueryBuilder` (C++) | `Select` `Where` `WhereEquals` `OrderBy` `GroupBy` `Having` `Limit` `Offset` `WithTotalCount` `Build` `EffectiveLimit` |
| `Prax::Filters` (C++) | `Eq Neq Gt Gte Lt Lte Like ILike Contains TextSearch StartsWith EndsWith IsNull IsNotNull In Between AnyOf AllOf EscapeLikeValue` |
| `Prax::Mutations` (C++) | `Insert` `InsertMany` `Update` `Delete` `UpdateById` `DeleteById` |

## Licence

[Praxsuite Open SDK Licence](LICENSE) — source-available, not OSI open source.

Free to use in anything you build, including games you sell. Free to fork, modify and publish your
changes. Not free to resell as an SDK, or to point at a competing backend.

Derived from the Praxsuite SDK — <https://praxsuite.com>
