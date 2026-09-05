# Block 2 — Integrate and verify the subscription provider

Execute this block only, then update the handoff below. This is the second of
six consecutive CHA implementation sessions. Requirements for this block are
included here; previous conversation or separate design reading is unnecessary.

## Goal and boundaries

Add `auth = "openai_subscription"` to CHA providers and make a real streamed
request using CHA-owned OAuth credentials. Preserve existing API-key,
anonymous, and test providers. Authentication is shared C++; the UI and runtime
routes are later blocks.

Keep this personal application small: reuse the current Responses transport,
decoder, events, reasoning display, usage reporting, and cancellation.
No Node/Pi/Codex runtime, model discovery, automatic retries, billing fallback,
WebSockets, compression, tool/image support, or generalized auth framework.

## Input conditions and session setup

1. From the repository root, read applicable `AGENTS.md`/`CLAUDE.md`, run
   `git status --short`, and preserve unrelated changes.
2. Block 1 must have provided a tested `OpenAiOAuth` and successful fresh
   login/refresh. Inspect its current header/tests and the Handoff in
   `docs/block1.md` for actual signatures and live results. The required
   contract is repeated below; no previous session summary is needed.
3. If prerequisite evidence is missing, inspect code/run relevant tests to
   establish state. Do not silently invent missing functionality or call an
   unverified live prerequisite passed. Report a remaining blocker explicitly.
4. Live generation needs user-approved CHA credentials and network access.
   Use disposable data, never Pi/Codex credential files or the active CHA
   profile. If fresh login is needed, use block 1's owner/temporary driver.
5. Stay below 300K session tokens, targeting a checkpoint before 200K.
   Read only relevant source/tests, use focused test output, and persist
   incomplete work here before stopping.

## Existing authentication contract

`src/providers/openai_oauth.h/.cpp` owns one account, pending login, a file
path, and one mutex. Its request operation returns a consistent access-token/
account-ID pair. Credentials are refreshed on demand with five minutes left;
the full returned bundle is saved before being published.

The private file is `<normalized absolute database path>.openai-auth.json`,
mode 0600, with only `access_token`, `refresh_token`, `expires_at` (Unix
seconds), and `account_id`. Expired credentials may load for renewal.
Renewal/save failure clears memory and attempts file removal; manual reconnect
is the recovery policy. Removal failure is reported, not disguised as success.

One mutex covers auth exchange and saving, never model streaming. Auth network
work has a 15-second overall deadline. Once a refresh starts it finishes/saves
even if generation is canceled. The provider must check cancellation both
before requesting credentials and after that call returns.

CHA obtained these credentials independently using the device flow with public
client ID `app_EMoamEEZ73f0CkXaXp7hrann`. Do not reopen client-registration
research. Compatibility is verified functionally, not by a CHA registration.
Do not change the small owner into a multi-provider credential service.

## Files to inspect or change

- `src/characters/character_config.h/.cpp`: provider configuration types.
- `src/workspace/workspace.cpp`: parsing and effective-setting validation.
- `src/providers/provider_client.h/.cpp`: construction, dispatch, credentials,
  URL, request headers, and failure handling.
- `src/providers/responses_api.h/.cpp`: body/decoding only where needed.
- `tests/providers/unit_provider_client.cpp`,
  `tests/providers/unit_responses_api.cpp`, and
  `tests/application/unit_workspace.cpp`; existing mock/test helpers.
- Read the owner from block 1; change it only for a concrete integration defect
  and add a focused regression test. Do not refactor its curl helper.

## Configuration contract

Add a small typed auth choice; omission preserves today's behavior. Reject
unknown explicit auth values. A subscription provider has these exact connection
and first-version settings:

```toml
auth = "openai_subscription"
host = "chatgpt.com"
port = 443
https = true
base_path = "/backend-api/codex"
mode = "net"
api = "responses"
model = "<model verified in the live check>"
stream = true
web_search = "off"
cache_retention = "off"
```

The model above is a placeholder, not a literal model name to ship. Select an
available model for the live check and record it for the package-seed session.
Require no `api_key_env`, no explicit temperature/max-output-token setting,
no enabled web search, and no non-off cache retention. Check effective
character/assistant/settings overrides as well as provider-file values.
The general cache default is `short`, so subscription configuration must
explicitly select `off`. These restrictions keep CHA's first implementation
small; they are not claims that the upstream backend rejects every such option.

Remove the workspace-load check that requires an environment API key to be
present. Keep structural configuration validation and existing environment
loading. A missing key must fail when its provider is used, allowing startup
and access to the connection page.

## Model wire contract

The Pi 0.85.1 source supplied this SSE baseline; it still needs a live CHA
request using the reduced body and CHA identity.

Exact URL: `https://chatgpt.com/backend-api/codex/responses`.
Do not append the ordinary provider's `/v1/responses` suffix.

Send these headers:

- `Authorization: Bearer <access token>`.
- `chatgpt-account-id: <matching account ID>`.
- `Content-Type: application/json`, `Accept: text/event-stream`.
- `OpenAI-Beta: responses=experimental`.
- `originator: cha` and a CHA user agent, not Pi's identity.

Send selected `model`, full conversation `input`, nonempty `instructions`,
`stream: true`, and `store: false`. If no instructions were supplied, use
`You are a helpful assistant.` Reuse existing Responses input encoding and
optional reasoning-effort handling.

Initially omit `max_output_tokens`, temperature, web-search/tools, cache or
session metadata, verbosity, encrypted-reasoning inclusion, and service-tier
extras. Add only a demonstrated upstream requirement, documenting its evidence.
Do not copy Pi's larger body or alternative transport machinery wholesale.

## Implementation steps

1. Add the auth type/parsing and validation above. Find symbols rather than
   relying on line numbers. In `workspace.cpp`, update both
   `load_provider()`'s inline web-search guard (originally around line 341)
   and `provider_supports_web_search()` (around line 527). Ensure character,
   assistant, and settings-update paths cannot re-enable unsupported options.
2. Remove only load-time environment-key availability rejection. Add a
   regression test that a keyless workspace loads/imports and an attempted
   API-key request still produces an actionable missing-key error.
3. Let `ProviderClient` accept the auth owner through the existing
   `ProviderClientFactory` arrangement. Preserve construction without an owner
   for ordinary providers. Subscription use without an owner must fail clearly,
   not dereference null or fall back to another billing method.
   Runtime creation/injection is block 3, so tests may supply the owner directly.
4. Resolve subscription credentials inside `ProviderClient::perform()`, where
   cancellation is available, rather than in construction/preparation. Check
   cancellation, obtain credentials, then check it again before dispatch.
   Never hold the owner's mutex while consuming the model stream.
5. Add the small subscription URL/header/body branch. Keep TLS verification
   and the fixed production destination. Do not forward tokens across
   redirects or permit a configurable auth/model host just for testing.
   Use the existing mock seams or a minimal request-level test seam.
6. Feed the response into the current SSE decoder and normal generation events.
   Preserve full conversation history, reasoning/usage data, and streaming
   cancellation. Do not add a separate model backend framework.
7. An unexpected model HTTP 401 ends this request with a reconnect message.
   Do not retry it, refresh in response to it, or mutate shared credentials:
   an older request must not erase a newly connected account. Other model,
   quota, and network failures use existing safe error reporting.
   Disconnect need not cancel requests that already copied their credentials.
8. Add focused configuration/request/cancellation tests with synthetic tokens.
   Then use the owner/provider and disposable data for a real streamed request.
   Record the tested model, safe request shape/header names, completion and
   usage results, and any required protocol correction in
   `docs/oauth-protocol.md` and this handoff. Never record header secret values,
   raw auth responses, account details, or actual tokens.
9. Remove the specific disposable credential file/driver when no longer needed;
   do not delete unrelated profile data. Report any deliberately retained test
   location without its contents.

## Completion verification

From the repository root:

```sh
cmake --preset ninja
cmake --build build/ninja --target cha_tests
./build/ninja/cha_tests --gtest_list_tests
./build/ninja/cha_tests
```

Use discovered filters during editing; zero matched tests is not a pass.
Before handoff, confirm:

- All fixed destination/mode/API/stream/auth rules and effective option
  restrictions have positive and negative tests.
- Omitted auth behaves as before; anonymous/test providers remain usable.
  Missing API keys no longer block loading but fail on request.
- Synthetic request assertions cover exact URL, consistent Bearer/account
  headers, CHA identity, full input, fallback instructions, required booleans,
  omitted fields, decoded output, and reported usage.
- A request canceled before auth sends nothing. Cancellation during refresh
  still saves the replacement bundle but prevents model dispatch.
- Model 401 results in one request, no retry, and no shared credential mutation,
  including when an older request fails after a later login.
- Existing API-key and stream-cancellation tests pass.
- The reduced live request actually streams and reports completion/usage for
  the named model. If unavailable or failing, record an incomplete live gate.

## Handoff

Update this file, not a transient summary or duplicate overview progress.
Leave a buildable checkpoint and stop before implementing runtime HTTP routes.

- Status: complete.
- Changed files and actual provider/factory/owner interfaces:
  - `src/characters/character_config.h/.cpp`: `ProviderAuth` on
    `ModelBackendConfig` (`none` by omission, `openai_subscription` explicit).
    `provider_endpoint()` returns the fixed Codex URL for subscription.
  - `src/workspace/workspace.cpp`: parse `auth`, reject unknown values and
    invalid subscription settings, reject web-search overrides, drop load-time
    API-key presence check.
  - `src/providers/responses_api.cpp`: subscription body uses fallback
    instructions and omits temperature, max tokens, tools, and cache fields.
  - `src/providers/provider_client.h/.cpp`:
    `ProviderClient(definition)`,
    `ProviderClient(definition, OpenAiOAuth*)`,
    `ProviderClient(definition, OpenAiOAuth*, ProviderHttpTransport)`.
    Owner is non-owning. Construction without an owner remains valid for
    ordinary providers; subscription without an owner throws. Credentials are
    copied in `perform()`; the owner's mutex is not held while streaming.
    Test seam: `ProviderHttpRequest` / `ProviderHttpResponse` /
    `ProviderHttpTransport`. Factory injection is left for block 3.
  - Tests: `tests/application/unit_workspace.cpp`,
    `tests/application/unit_workspace_config_store.cpp`,
    `tests/providers/unit_responses_api.cpp`,
    `tests/providers/unit_provider_client.cpp`,
    `tests/support/test_workspace.cpp`.
  - `docs/oauth-protocol.md` (live record only).
- Commands, test counts, and results:
  - `cmake --preset ninja`
  - `cmake --build build/ninja --target cha_tests`
  - `./build/ninja/cha_tests --gtest_list_tests`: 431 tests.
  - `./build/ninja/cha_tests`: 429 passed, 2 skipped (live, env not set).
  - `CHA_OPENAI_OAUTH_LIVE=1 ./build/ninja/cha_tests --gtest_filter='ProviderClientLive.SubscriptionStreamedRequest'`:
    passed (276s).
- Live request date, tested model, body/header requirements, completion/usage:
  2026-09-05, model `gpt-5.6-terra`. URL
  `https://chatgpt.com/backend-api/codex/responses`. Headers:
  `Authorization: Bearer …`, `chatgpt-account-id`, `Content-Type: application/json`,
  `Accept: text/event-stream`, `OpenAI-Beta: responses=experimental`,
  `originator: cha`, `User-Agent: cha`. Body: `model`, full `input`,
  `instructions` (fallback `You are a helpful assistant.` when empty),
  `stream: true`, `store: false`. No extra fields were required. Outcome
  completed; answer 4 bytes; usage input_tokens=19, output_tokens=18.
  Package-seed session should use `gpt-5.6-terra`.
- Disposable credential cleanup or retained path: live credentials were
  written under a temp directory and removed after the check. None retained.
- Remaining work, compatibility differences, or unavailable checks: none.
  Reduced CHA body and originator were accepted; no protocol correction.
- Ready for block 3: yes. Stop here; do not implement runtime HTTP routes.
