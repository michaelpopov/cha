# Block 1 — Implement the C++ OpenAI authentication owner

Execute this block only, then leave the handoff below. This is the first of six
consecutive implementation sessions for CHA's OpenAI subscription support.
The requirements and wire protocol needed here are included in this document.
No earlier conversation or separate design reading is required.

## Goal and boundaries

Implement and test one small C++ component that performs fresh device login,
stores CHA-owned credentials, renews them on demand, and disconnects. Verify a
real login and refresh before the next block adds model requests.

CHA is a personal toy application. Use one account per workspace, one owner,
and one mutex. Do not add a general OAuth framework, Node dependency, callback
server, authentication CLI, background worker, cross-process lock, recovery
queue, or token-generation counters. Linux's browser can be on another machine;
macOS will use the same C++ owner and web UI.

## Input conditions and session setup

1. Work from the CHA repository root; read applicable `AGENTS.md` and
   `CLAUDE.md`. Run `git status --short` and preserve unrelated changes.
2. Inspect the files below and confirm the existing C++ tests can build.
   Search for symbols rather than loading the whole repository.
3. The device protocol was inspected in Pi 0.85.1 on 2026-09-05, but has not
   yet been live-verified in CHA. Use Codex CLI's public client ID given below;
   do not reopen client registration or require a separately registered CHA
   client. The gate is whether the flow works, not whether CHA is sanctioned.
   This is an accepted compatibility dependency, not a promised versioned API.
4. A live check needs network access and the user's approval in a browser.
   Obtain fresh credentials for disposable CHA data. Never read, copy, or
   modify Pi's or Codex's credential files. If approval/network is unavailable,
   finish safe implementation and synthetic tests, then record the live check
   as outstanding, not passed.
5. Keep this session below 300K tokens, aiming to checkpoint before 200K,
   including instructions, source, logs, and conversation. Persist incomplete
   work in this document rather than depending on a session summary.

## Files and existing pieces

- Add `src/providers/openai_oauth.h`, `src/providers/openai_oauth.cpp`, and
  `tests/providers/unit_openai_oauth.cpp`; register them in `CMakeLists.txt`.
- Update `.gitignore` to ignore `*.openai-auth.json`.
- Read/reuse `src/util/private_filesystem.h/.cpp`,
  `tests/util/unit_private_filesystem.cpp`, and
  `tests/support/mock_http_server.h`.
- Read `src/providers/provider_client.cpp` for libcurl initialization and
  local conventions only. Its private `ProviderClient::CurlEasyHandle` is
  streaming-specific: leave that file and wrapper unchanged in this block.

## Protocol to implement

Fixed authentication origin: `https://auth.openai.com`.
Public client ID: `app_EMoamEEZ73f0CkXaXp7hrann`.
Public verification URL: `https://auth.openai.com/codex/device`.

All operations are POSTs. Send JSON with `application/json`; token exchanges
use properly encoded `application/x-www-form-urlencoded`.

| Operation | Path | Request fields | Required success fields |
| --- | --- | --- | --- |
| Start | `/api/accounts/deviceauth/usercode` | JSON `client_id` | `device_auth_id`, `user_code`, `interval` |
| Poll once | `/api/accounts/deviceauth/token` | JSON `device_auth_id`, `user_code` | `authorization_code`, `code_verifier` |
| Exchange approval | `/oauth/token` | Form `grant_type=authorization_code`, `client_id`, `code`, `code_verifier`, `redirect_uri` | Token bundle |
| Refresh | `/oauth/token` | Form `grant_type=refresh_token`, `client_id`, `refresh_token` | Token bundle |

For approval exchange, use the poll's authorization code as `code` and
`https://auth.openai.com/deviceauth/callback` as `redirect_uri`.
CHA does not host that callback. No locally generated PKCE pair or pasted
authorization code is needed; the poll supplies the verifier.

Start's interval is seconds, either a finite nonnegative number or numeric
string. Convert to milliseconds and clamp to at least one second. Set a local
15-minute attempt deadline. Waiting one interval before the first poll is fine.
Keep the interval, deadline, and next allowed poll time in the owner.

Handle poll results in this order:

1. A successful response must contain the authorization code and verifier;
   exchange them within the same operation deadline.
2. HTTP 403 or 404 means pending on this poll endpoint only.
3. Otherwise, read an error string from `error` or `error.code`.
   `deviceauth_authorization_pending` means pending; `slow_down` adds five
   seconds to the interval. Other failures end the attempt with a safe error.

A 404 on start or an error on exchange/refresh is not pending. Expired,
malformed, or failed attempts are discarded. Raw upstream bodies are never
user-visible or logged.

Both login and refresh require nonempty `access_token` and `refresh_token`,
and numeric, finite, positive `expires_in`. Set expiry to current Unix seconds
plus that lifetime. Extract a nonempty account ID from the access token's
base64url-decoded JWT payload:

```text
payload["https://api.openai.com/auth"]["chatgpt_account_id"]
```

This is metadata extraction, not JWT signature verification or a separate
authentication decision. Do not add email lookup, ID-token storage, alternate
expiry sources, or an omitted-refresh-token fallback.

These are synthetic fixture shapes, not live observations:

```jsonl
{"device_auth_id":"fixture-device","user_code":"TEST-ONLY","interval":"5"}
{"authorization_code":"fixture-code","code_verifier":"fixture-verifier"}
{"error":"deviceauth_authorization_pending"}
{"error":{"code":"slow_down"}}
{"error":"fixture-denial"}
{"access_token":"fixture-access","refresh_token":"fixture-refresh","expires_in":3600}
```

For parser tests, replace the placeholder access token with a synthetic
JWT-shaped string containing the required claim. Do not put real tokens in tests.

## Component and storage contract

`OpenAiOAuth` owns the supplied credential-file path, current bundle, optional
pending attempt, and one mutex. Expose small operations for safe status,
start, poll once, disconnect, and obtaining credentials for a model request.
Choose plain names and document the actual signatures in the handoff.

Public status has three states: `signed_out`, `waiting`, `connected`.
Only waiting exposes the user code, fixed verification URL, attempt expiry,
and next-poll delay. No access/refresh token, account ID, private device ID,
authorization code, or verifier belongs in the public snapshot.

The runtime added later supplies
`<normalized absolute database path>.openai-auth.json`; for example,
`/srv/cha/cha.sqlite3.openai-auth.json`. This block accepts the file path
directly rather than building runtime ownership.

Store exactly these fields:

```json
{
  "access_token": "<secret>",
  "refresh_token": "<secret>",
  "expires_at": 1800000000,
  "account_id": "<account>"
}
```

`expires_at` is Unix seconds. Reuse `create_private_file()` for atomic
replacement, mode 0600, and regular-file safety checks. Load once at owner
construction: a missing file is signed out; invalid data gives a sanitized
diagnostic and signed-out state, not fatal startup. Retain a valid expired
bundle so first use can refresh it. Do not refresh during startup.

Credentials stay outside SQLite, configuration exports, R2 transfers, mirrors,
browser state, and packages. The database's existing exclusive lease will
protect ownership; no extra auth-file lock is needed.

## Implementation steps

1. Add a small file-local plain `curl_easy_*` POST helper with RAII cleanup,
   response collection, status handling, and JSON/form encoding. Initialize
   curl safely for process lifetime even if no provider has ever run.
   Keep TLS verification enabled and fixed production destinations; do not
   forward credentials through redirects.
2. Bound each auth operation's network work by one overall 15-second deadline.
   Pass the remaining time to each request, including poll's final exchange;
   do not allocate a fresh 15 seconds to each leg. Use existing HTTP/provider
   worker threads later, not a new worker or long-running poll loop.
3. Implement loading and the owner operations. Serialize state, exchanges, and
   saving with the single mutex. Repeated start while waiting returns the
   same attempt. While connected, require disconnect before another login.
   Status never performs upstream network work.
4. Implement server-enforced poll timing. An early poll returns a snapshot
   without network work. Each eligible call polls at most once, possibly
   followed by the approval exchange. Save the full bundle before publishing
   connected state. A restarted process does not retain pending login.
5. Implement request credential resolution. Under the mutex, require a bundle;
   refresh when expiry is within five minutes, save the full returned bundle,
   then return a consistent access-token/account-ID copy. Release the mutex
   before any model streaming. Waiting callers reuse the refreshed bundle.
6. Once refresh is sent, finish and save its result even if the triggering
   generation is canceled. The provider will check cancellation before calling
   the owner and after it returns. Do not add auth cancellation machinery.
7. Disconnect clears pending/connected memory and removes the file under the
   same mutex. Renewal or save failure also clears memory and attempts removal.
   Report removal failure honestly: an undeleted file might reload on restart.
   A serialized disconnect cannot be undone by a late poll/refresh completion.
   Manual reconnect and a brief serialized wait are accepted limitations.
8. Add the smallest transport/time seam needed for deterministic tests. Keep
   fake endpoints out of production configuration. Test with synthetic data.
9. Use a temporary driver around the owner for fresh device login and one
   refresh. Let the user approve in their browser. Do not print token responses
   or add a permanent CLI. Record success/failure, date, response field shapes,
   and whether the returned refresh token actually changed in
   `docs/oauth-protocol.md` and summarize the outcome below. A parser accepting
   a refresh token is not proof that it rotated in the live check.

## Completion verification

From the repository root:

```sh
cmake --preset ninja
cmake --build build/ninja --target cha_tests
./build/ninja/cha_tests --gtest_list_tests
./build/ninja/cha_tests
```

While editing, use discovered GoogleTest filters; zero matched tests is not a
pass. The final core run must include the new tests. Verify:

- Start/repeated start, interval strings/numbers, pending statuses and error
  shapes, slowdown, early polls, approval exchange, deadline, and cancellation.
- Missing/invalid/expired files, restart loading, required token fields/account
  claim, Unix-second expiry, private atomic writes, save/removal failures,
  and sanitized errors.
- Concurrent near-expiry requests produce one successful renewal and persist
  its replacement bundle. Failed renewal leaves waiters signed out without
  another refresh. Disconnect after an in-flight exchange remains effective.
- The owner works before constructing a `ProviderClient`; its streaming
  wrapper was not changed. Check the diff for unintended scope expansion.
- Fresh CHA login and refresh actually work. Unavailable approval/network or a
  failing endpoint remains an explicit incomplete live gate.

## Handoff

Update this section before stopping, including if the block is incomplete.
This file is the authoritative progress record for block 1, not the chat or
the overview plan. Do not claim tests that were not run.

- Status: complete.
- Changed files and actual owner/test-seam interfaces:
  - `src/providers/openai_oauth.h`, `src/providers/openai_oauth.cpp`
  - `tests/providers/unit_openai_oauth.cpp`
  - `CMakeLists.txt` (core + `cha_tests` sources)
  - `.gitignore` (`*.openai-auth.json`)
  - `docs/oauth-protocol.md` (live record only)
  - `ProviderClient` was not changed.
  - Owner: `OpenAiOAuth(path)` or `OpenAiOAuth(path, transport, clock)`.
    `status()`, `start()`, `poll()`, `disconnect()` return
    `OpenAiOAuthSnapshot` (`signed_out` / `waiting` / `connected`; waiting
    includes `user_code`, `verification_url`, `attempt_expires_at`,
    `next_poll_delay_ms`; optional sanitized `error`).
    `credentials()` returns `{access_token, account_id}` or throws.
  - Test seam: injected `OpenAiOAuthTransport` POST (`url`, `content_type`,
    `body`, remaining `timeout`) and `OpenAiOAuthClock`. Production always
    posts to `https://auth.openai.com`.
- Commands, test counts, and results:
  - `cmake --preset ninja`
  - `cmake --build build/ninja --target cha_tests`
  - `./build/ninja/cha_tests --gtest_list_tests`: 417 tests; includes
    `OpenAiOAuthTest` (25) and `OpenAiOAuthLive.LoginAndRefresh` (1).
  - `./build/ninja/cha_tests`: 416 passed, 1 skipped (live, env not set).
  - `./build/ninja/cha_tests --gtest_filter='OpenAiOAuth*'`: 25 passed,
    1 skipped.
  - `CHA_OPENAI_OAUTH_LIVE=1 ./build/ninja/cha_tests --gtest_filter='OpenAiOAuthLive.LoginAndRefresh'`:
    passed (289s).
- Live login/refresh date, outcome, rotation observation: 2026-09-05,
  passed; returned refresh token rotated.
- Temporary driver/credential location for block 2, if deliberately retained:
  none. Live credentials were written under a temp directory and removed
  after the check. Re-run the env-gated live test, or sign in again in
  block 2.
- Remaining work or deviations from the embedded protocol: none. The live
  start response's `interval` JSON type was not recorded. Stop here; do not
  implement model requests.
- Ready for block 2: yes.
