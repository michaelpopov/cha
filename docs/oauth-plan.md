# OpenAI subscription implementation plan

Status: not implemented. Follow the [design](oauth-design.md) and the
[Pi-derived protocol reference](oauth-protocol.md). The installed Pi 0.85.1
code supplies the wire protocol; a separate discovery session is no longer
needed. Live login/refresh verification belongs to block 1, and the first live
model request to block 2.

## Session boundaries and handoffs

Use six fresh Codex sessions, in order. Each block leaves a buildable checkpoint
and has explicit inputs, work, and completion checks.

| Block | Outcome | Prerequisite |
| --- | --- | --- |
| [1](block1.md) | Small C++ auth owner; login and refresh verified | Embedded design and Pi protocol |
| [2](block2.md) | Subscription provider; streamed request verified | Block 1 |
| [3](block3.md) | Shared runtime ownership and HTTP API | Blocks 1–2 |
| [4](block4.md) | Browser connection page | Block 3 |
| [5](block5.md) | macOS startup and shared package seed | Blocks 1–4 |
| [6](block6.md) | Packaged Linux and macOS verification | Blocks 1–5 |

Give each fresh session the corresponding `docs/blockN.md`. These standalone
execution briefs include the requirements needed for that block; the session
does not need the earlier conversation or separate design/protocol reading.
Existing source, tests, and prior blocks' persisted handoffs supply the actual
implementation state. The sections below remain the overview.

Keep each session below **300K tokens**, targeting at most 200K including
instructions, source reads, tool output, and conversation. These are scoped
work units, not measured token predictions. Read the current execution brief,
repository instructions, and only the current block's files and relevant tests.
Search for symbols before reading large files; do not load all Pi dependencies
or repeatedly dump complete build logs. Run focused tests while editing.

Start with `git status --short` and preserve unrelated changes. Before ending a
block or checkpointing near 200K, update the **Handoff** section in its
`docs/blockN.md`: status, changed files/interfaces, exact checks and results,
remaining work, and next-block prerequisites. A fresh session reads those
persisted notes, not the previous conversation. Do not duplicate progress in
this overview. Record live protocol findings in `docs/oauth-protocol.md`; keep
it separate from implementation progress.
Never mark an unavailable test or unfinished block complete.

## Keep the implementation small

- One C++ owner, one mutex, one account per workspace. Reuse the database lease.
- Device login only; no callback server, pasted authorization code, email lookup,
  ID-token storage, Node/Pi/Codex runtime, or general OAuth framework.
- Browser-driven one-step polling; refresh on request with five minutes left.
  Hold the mutex through auth and saving, never through model streaming.
- A 15-second overall network deadline per auth operation, including a final
  exchange. Once refresh starts, finish/save it before honoring generation
  cancellation. No background auth workers or renewal timers.
- Existing atomic private-file helper, sanitized errors, TLS verification, fixed
  destinations. No credentials in browser responses/storage, logs, SQLite,
  exports, R2 transfers, transcript mirrors, fixtures, or package assets.
- Existing SSE provider transport only. No automatic model retry after `401`,
  billing fallback, WebSockets, compression, or Pi's cross-process auth framework.
- Keep the design's manual-reconnect policy after renewal/save failures.
  Do not port Pi's raw-response errors or direct credential-file writes.

## Block 1 — Port the small C++ authentication owner

### Input conditions

- Read the design and `docs/oauth-protocol.md`; the client ID and device exchange
  are already recorded. Do not reopen client-registration research.
- C++ tests build, network access is available, and the user can approve a fresh
  login. Use disposable CHA data, never Pi's or Codex's stored credentials.

### Files to read or change

Add `src/providers/openai_oauth.h/.cpp` and
`tests/providers/unit_openai_oauth.cpp`; update `CMakeLists.txt` and `.gitignore`.
Reuse `src/util/private_filesystem.h/.cpp` and the existing private-file/mock
HTTP tests. `src/providers/provider_client.cpp` is read-only in this block.

### What to do

1. Write a small file-local blocking POST helper using plain `curl_easy_*`,
   JSON/form encoding, response collection, HTTP status checks, and local RAII
   cleanup. Pass the remaining operation deadline to each POST. Follow the
   process-lifetime curl initialization pattern without relying on a provider
   having run first. Do not extract its private `CurlEasyHandle` wrapper.
2. Add `OpenAiOAuth`: credentials, one pending attempt, supplied file path, one
   mutex. Expose safe status, start, poll-once, disconnect, and request-credential
   operations. Model callers receive only a consistent access token/account ID.
3. Load the file once. Missing/invalid data leaves CHA signed out; invalid data
   gets a sanitized diagnostic, not a startup failure. A valid bundle with an
   expired access token remains loadable for refresh on first use.
4. Port Pi's device exchange from the protocol notes. Store the 15-minute
   attempt deadline and next-poll time; early polls return state without network
   work. Handle interval strings, pending `403`/`404` on the poll endpoint,
   slowdown, approval/final exchange, and expiry/failure. Repeated start returns
   the pending attempt; changing a connected account requires disconnect first.
5. Parse the complete token response: access token, refresh token, and lifetime;
   extract account ID from the access-token payload. Store exactly the four
   design fields with Unix-second expiry using `create_private_file()`.
   Save before publishing connected state; ignore `*.openai-auth.json` in Git.
   No email, alternate expiry source, or omitted-refresh-token fallback.
6. Under the mutex, refresh when expiry is within five minutes, save the returned
   bundle, and copy request credentials. Waiting callers reuse the fresh bundle.
   Disconnect clears pending/connected state and removes the file under the same
   mutex. Renewal/save failure clears memory and attempts file removal; report
   removal failure honestly. No recovery queue or backup credential store.
7. Add a minimal transport/time test seam, not configurable production URLs.
   With a temporary driver around the owner, complete a fresh device login and
   one refresh. Record results in the protocol notes; keep any temporary
   credential path, but never its contents, in the handoff if needed by block 2.
   No permanent authentication CLI is needed.

### Completion verification

- Tests cover start/repeated start, early/pending polls, slowdown, final exchange,
  expiry/failure, and cancel/disconnect using synthetic values.
- File tests cover missing/invalid/expired bundles, restart loading, `0600`,
  atomic saving, write/removal failures, and token-free diagnostics.
- Two concurrent near-expiry callers produce one successful refresh and save
  the replacement bundle; failed renewal does not make waiting callers retry.
  Disconnect after a blocked poll/refresh cannot be undone by late completion.
- `cha_tests` and focused tests pass; auth works before any `ProviderClient`
  exists, and its source file remains unchanged.
- Fresh login and refresh succeed without Pi/Codex credentials. Record actual
  failures as functional blockers, not a missing CHA registration. Do not claim
  the C++ owner is live-verified merely because Pi's source implements the flow.

## Block 2 — Integrate the subscription provider

### Input conditions

Block 1 passed. Read its persisted handoff and protocol verification results.
Keep existing API-key and anonymous/test-provider behavior unchanged.

### Files to read or change

`src/characters/character_config.h/.cpp`, `src/workspace/workspace.cpp`,
`src/providers/provider_client.h/.cpp`, and the Responses builder only as needed.
Extend `tests/providers/unit_provider_client.cpp`, `unit_responses_api.cpp`,
and `tests/application/unit_workspace.cpp`.

### What to do

1. Add a small typed `auth = "openai_subscription"` option. Omission preserves
   current behavior; unknown values fail. Require the fixed HTTPS destination,
   port/base path, net mode, Responses streaming, and absence of `api_key_env`.
2. Enforce the first-version limits: no explicit temperature/max tokens, enabled
   web search, or non-off cache retention, including effective overrides.
   These are CHA scope limits, not universal backend restrictions. Set
   `cache_retention = "off"` explicitly; the general default is `short`.
3. Update both web-search checks in `workspace.cpp`: `load_provider()`'s inline
   guard (currently line 341) and `provider_supports_web_search()` (line 527).
   The helper must reject subscription providers in character/assistant and
   settings-update paths; the provider-file check alone is insufficient.
4. Remove only the environment-key availability check from workspace loading.
   Retain structural validation and environment loading. Missing credentials
   fail when that provider is used, allowing the user to open CHA and connect.
5. Let the existing provider factory supply the auth owner to `ProviderClient`.
   Preserve construction without it for ordinary providers; subscription use
   without it gives an actionable error. Resolve credentials in `perform()`,
   with cancellation checks before auth and after successful refresh/save.
6. Add the small subscription request branch from the protocol notes: exact
   `/backend-api/codex/responses` URL, Bearer/account and SSE headers, CHA caller
   identity, full conversation, nonempty instructions, `stream: true`, and
   `store: false`. Omit deferred options; reuse existing decoding, reasoning,
   usage, and streaming cancellation. Do not forward tokens across redirects.
7. A model `401` ends the request with a reconnect message, without retrying or
   mutating shared credentials. Preserve existing reporting for other errors.
   Use the new owner/provider with disposable data to make one live streamed
   request; record the tested model, reduced body, headers, and completion/usage
   results in the protocol notes. Remove disposable credentials when finished.

### Completion verification

- Config/override tests cover fixed destination and first-version limits.
  A missing API key no longer prevents loading/import but fails on provider use.
- Request tests assert the exact URL, matching headers, body omissions, full
  input, instruction fallback, streamed output, and usage. Use synthetic
  credentials; do not weaken production URL validation for a mock server.
- Already-canceled requests do not start auth. Cancellation during refresh
  preserves the saved replacement bundle and prevents model dispatch.
- `401` causes one model request and cannot erase a later login. Existing API-key,
  test-provider, and stream-cancellation tests pass.
- Provider/workspace suites and the live reduced-request smoke test pass.
  Only a demonstrated compatibility requirement justifies expanding the body.

## Block 3 — Wire the shared runtime and HTTP API

### Input conditions

Blocks 1–2 passed, including live protocol checks. Read their handoffs; the
owner/provider interfaces are ready for Linux and macOS's shared runtime.

### Files to read or change

`src/web/application_runtime.h/.cpp`, `src/workspace/workspace_config_store.*`,
existing `lobby_routes.*`/`route_support.*` patterns, `resources/cha.yaml`,
and `CMakeLists.txt`. Add `src/web/openai_auth_routes.h/.cpp` and route tests;
extend `tests/web/unit_application_runtime.cpp`. Regenerate
`webapp/src/api/schema.d.ts`, never edit it manually.

### What to do

1. Create one owner after the workspace store normalizes the database path and
   acquires its lease. Append `.openai-auth.json` to that absolute path.
   Share the owner with routes and the provider factory; keep it alive until
   existing HTTP/provider shutdown finishes. No new shutdown subsystem.
2. Add the four design routes under `/api/v1/openai/auth`: status GET and
   login/poll/disconnect POSTs with `{}` bodies. Poll invokes one owner operation,
   not a loop. No per-browser attempts, public login IDs, or auth event stream.
3. Define a minimal public snapshot: `signed_out`, `waiting`, or `connected`.
   Waiting includes code, verification URL, expiry, and next-poll delay.
   Specify timing units in OpenAPI. No email, account lookup, or token fields.
4. Reuse JSON/body/origin checks, error envelopes, and the macOS cookie gate.
   Add `Cache-Control: no-store` to auth responses, including route errors.
   Never serialize an internal credential object or raw upstream error body.
5. Keep this owner and its sibling file outside database import/export, R2
   transfers, and workspace reopen/maintenance. Do not create a second owner
   when the store republishes the workspace.
6. Regenerate API types with `npm run api-types`. Make only compatibility edits
   needed to keep the current frontend building; the page belongs to block 4.

### Completion verification

- Route tests cover all four endpoints, invalid bodies/origins/content types,
  no-store, safe errors, and no network request on status GET.
- Runtime tests cover shared ownership, normalized path, signed-out startup,
  cookie protection, and shutdown with a bounded auth operation in flight.
- Synthetic secrets do not appear in responses/logs/transfers; import/export
  and maintenance preserve the local connection.
- `cha_web_tests`, affected core tests, and `npm run check` pass. Persist concrete
  public response examples for the UI session.

## Block 4 — Add the browser connection page

### Input conditions

Block 3 passed. Read its API examples/handoff and generated types. Automated UI
work uses synthetic responses, not a live subscription.

### Files to read or change

`webapp/src/api/client.ts`, `state/view.ts`, route helpers as needed,
`components/Sidebar.tsx` and `App.tsx`, plus one connection component.
Extend client/component/navigation tests and shared `src/test/fixtures.ts`.

### What to do

1. Add the four typed client methods and update affected `ChaClient` mocks.
2. Add an **OpenAI** sidebar destination. Fetch status on entry without losing
   chat/session state. Reuse existing layout, buttons, loading/error handling.
3. Show **Connect ChatGPT**, waiting link/code/**Cancel**, or **Connected to
   ChatGPT**/**Disconnect**. No account-profile UI. Open verification only on
   user action using a normal external HTTPS link with new-tab protections.
4. Schedule one poll from server timing and wait for its response before the
   next. Stop on success, error, cancel, disconnect, or leaving the page; clear
   timers and ignore stale completions after unmount. Re-entering resumes an
   unexpired server-side attempt. No global poller or cross-tab coordinator.
5. Serialize this page's mutations and disable relevant buttons in flight.
   Cancel/disconnect share the endpoint. Other tabs control the same connection;
   immediate cross-tab synchronization is unnecessary.
6. Keep credentials out of browser storage and URLs. Errors invite explicit
   retry/reconnect, never silently replay model requests.

### Completion verification

- Tests cover all three states, start/cancel/disconnect, errors, and resuming
  pending login after navigation.
- Fake timers prove no overlapping polls or rescheduling after terminal state
  or unmount. Client tests assert routes and empty JSON POST bodies.
- `npm run check`, `npm run build`, and a browser smoke test pass; existing chat,
  navigation, and character-settings behavior remains intact.

## Block 5 — Finish macOS startup and package defaults

### Input conditions

Blocks 1–4 passed. macOS build tools and user approval for a login smoke test
are available. Use disposable application data, not the user's active profile.

### Files to read or change

`packaging/macos/main.swift`, `runtime_bridge.cpp`, `runtime-smoke.c`,
`package.sh`; shared `packaging/linux/import-seed/`; workspace import tests;
and existing Linux package/upgrade checks where seed assumptions change.

### What to do

1. Remove the mandatory API-key prompt from normal Mac launch.
   `prepareApplicationData()` and initial database import must work without a
   key. Start the common runtime/UI; do not add a Swift token bridge.
2. Preserve inherited/saved keys, environment loading, and **Change API Key…**.
   Do not erase `.env` or change existing databases/provider selections.
3. Open the verification link via `NSWorkspace`, handling its actual navigation
   or new-window action. Keep `WKWebView` on CHA and retain cookie/download
   behavior. There is no OAuth callback listener.
4. Add a separately named subscription provider to the shared seed using block
   2's tested model and valid first-version settings. Select it for fresh seed
   characters/assistant; retain the API-key provider. Existing users use current
   configuration import and character settings, not a migration or new wizard.
5. Adjust affected seed/package tests. Keep deterministic test providers in
   browser tests, and exclude credentials and Pi/Node/Codex from runtime assets.

### Completion verification

- Mac wrapper/runtime smoke tests pass. Fresh launch without an API key opens
  the UI; device login uses the system browser and finishes on the common page.
- Credentials live beside the database in Application Support, not in the bundle.
- Seed/import tests pass. Existing data, saved keys, and **Change API Key…**
  still work; no Linux success is inferred from a Mac build.

## Block 6 — Verify packaged Linux and macOS

### Input conditions

Blocks 1–5 passed. Linux, supported macOS, a remote browser for Linux, and user
login approval are available. Use disposable databases/test profiles and fresh
package output directories; never overwrite the user's real data for testing.

### What to do

1. Run the full commands below. Build with `scripts/package-linux.sh` and
   `packaging/macos/package.sh` on their respective platforms, using their
   version/output-parent arguments. Reuse existing package and upgrade checks.
2. Start packaged Linux `chaweb` in console mode without an API key, Pi, or
   Codex. Connect from another machine through the existing private deployment
   or SSH tunnel. No server-side browser, stdin prompt, or OAuth callback.
3. On Linux/remote-browser and packaged Mac, verify fresh login, streamed
   generation with conversation history, restart using saved credentials,
   refresh, disconnect, and refusal to generate until reconnect. For renewal,
   wait or adjust only saved expiry in disposable data before restarting;
   do not alter the system clock or token. Verify the rotated bundle survives
   another restart.
4. Check leaving/resuming a pending page and canceling login. The lower-level
   tests already cover refresh/disconnect races; no new concurrency harness.
5. With synthetic markers, check browser/log/SQLite/export/R2/mirror/package
   outputs for leakage and file permissions. A database transferred to a fresh
   installation must not carry login credentials. Recheck API-key generation
   and an existing database across a package upgrade. Fix only discovered
   integration bugs, adding focused regression tests.

### Completion verification

- Core, web, browser, package, and affected upgrade checks pass.
- Both packaged platforms pass login/refresh/generation/restart/disconnect
  without a Pi/Codex runtime or API key; the API-key path also still works.
- Handoff names tests actually run and outstanding checks. An unavailable
  platform or live login remains unverified, not passed. Keep manual reconnect
  and brief serialized auth waits as accepted limitations.

## Verification commands

From the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure
```

During each block, build `cha_tests`/`cha_web_tests` and run affected GoogleTest
suites. Discover names with `--gtest_list_tests`; zero matched tests is not a
pass. Run the full suite before final handoff.

From `webapp`, use `npm ci` if dependencies are missing, then:

```sh
npm run check
npm run build
npm run e2e
```

Run `npm run api-types` when OpenAPI changes. E2E needs the built `chaweb` and
Playwright browser dependencies; the existing harness supplies disposable data
and a mock model. Automated tests must never require real subscription tokens.
