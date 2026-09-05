# OpenAI subscription protocol reference

Status: source-inspected on 2026-09-05. Device login, refresh, and a reduced
streamed model request were live-verified in CHA on 2026-09-05.
This records the installed Pi implementation so each implementation session
can start from concrete inputs. See the [design](oauth-design.md) for CHA's
ownership/error policy and the [plan](oauth-plan.md) for implementation checks.

## Source

Inspected `@earendil-works/pi-ai@0.85.1`, installed beneath the global
`@earendil-works/pi-coding-agent@0.85.1/node_modules/` directory. Locate the
global package root with `npm root -g`. Relevant paths inside `pi-ai`:

- `dist/auth/oauth/openai-codex.js`: client constants, device flow, token parser.
- `dist/auth/oauth/device-code.js`: polling intervals and timeout.
- `dist/auth/resolve.js`: refresh margin and locking.
- `dist/api/openai-codex-responses.js`: request body, URL, and headers.

The coding-agent package's `dist/core/auth-storage.js` handles file storage.
Do not inspect either application's saved credentials to learn this protocol.
Pi is a source reference only; CHA must not depend on its installation.

## Device login and refresh

Fixed auth origin: `https://auth.openai.com`.
Public client ID: `app_EMoamEEZ73f0CkXaXp7hrann`.
Verification link shown to the user: `https://auth.openai.com/codex/device`.

All three endpoints below use POST. JSON requests use `application/json`;
token exchanges use `application/x-www-form-urlencoded`, with proper encoding.

| Operation | Path | Body fields | Successful result used by Pi |
| --- | --- | --- | --- |
| Start device login | `/api/accounts/deviceauth/usercode` | JSON: `client_id` | `device_auth_id`, `user_code`, `interval` |
| Poll once | `/api/accounts/deviceauth/token` | JSON: `device_auth_id`, `user_code` | `authorization_code`, `code_verifier` |
| Finish login | `/oauth/token` | Form: `grant_type=authorization_code`, `client_id`, `code`, `code_verifier`, `redirect_uri` | Token bundle |
| Refresh | `/oauth/token` | Form: `grant_type=refresh_token`, `client_id`, `refresh_token` | Token bundle |

For the final exchange, `code` is the poll result's `authorization_code`, and
`redirect_uri` is `https://auth.openai.com/deviceauth/callback`. This is an
exchange parameter, not a callback hosted by CHA. The device path needs no
local callback server, locally generated PKCE pair, or browser-code input.

Pi accepts `interval` as a finite nonnegative number or numeric string, converts
seconds to milliseconds, and clamps the polling interval to at least 1 second.
It sets a local 15-minute login deadline; this is not a server-provided expiry.
CHA keeps that deadline and next-poll time in C++, but the browser schedules
individual polls. Unlike Pi's immediate first poll, CHA may wait the interval
before its first poll, keeping one scheduling rule for all requests.

Polling response handling, in Pi's order:

- Success: require the authorization code and verifier, then exchange them.
- HTTP `403` or `404`: pending **on the poll endpoint only**.
- Otherwise parse `error` as a string or `error.code` as a string.
  `deviceauth_authorization_pending` means pending; `slow_down` increases the
  interval by 5 seconds. All other failures end the attempt.

A `404` while starting login is an error, not pending. Do not apply the poll
exception to token exchange or model requests. Denial/other error responses
need no extra UI state: discard the attempt and show a sanitized error.

Synthetic fixture shapes, not real credentials or live observations:

```jsonl
{"device_auth_id":"fixture-device","user_code":"TEST-ONLY","interval":"5"}
{"authorization_code":"fixture-code","code_verifier":"fixture-verifier"}
{"error":"deviceauth_authorization_pending"}
{"error":{"code":"slow_down"}}
{"error":"fixture-denial"}
{"access_token":"fixture-access","refresh_token":"fixture-refresh","expires_in":3600}
```

Tests needing an account claim must construct a synthetic JWT-shaped access
token rather than use the placeholder above.

## Token bundle

Pi requires `access_token`, `refresh_token`, and numeric `expires_in` on both
exchange and refresh. CHA will additionally validate nonempty strings and a
finite positive lifetime. There is no omitted-refresh-token fallback.

Pi stores expiry in milliseconds; CHA stores `expires_at` in Unix **seconds**:
current Unix seconds plus `expires_in`. Extract `account_id` from the access
token's base64url-decoded JSON payload at
`["https://api.openai.com/auth"]["chatgpt_account_id"]`. This is metadata
extraction, not JWT signature validation or an authentication decision.

Store only access token, refresh token, expiry, and account ID. No email,
ID token, account lookup, or alternative expiry source. Pi refreshes with
five minutes remaining and uses a 15-second refresh timeout; use those constants.
Save the entire replacement bundle before making it available to requests.
The parser expects a returned refresh token; actual rotation behavior remains
part of the live check, not a fact established by reading the parser.

## Model request baseline

Pi resolves the SSE URL to `https://chatgpt.com/backend-api/codex/responses`.
For CHA, start with these headers:

- `Authorization: Bearer <access token>` and matching `chatgpt-account-id`.
- `Content-Type: application/json`, `Accept: text/event-stream`.
- `OpenAI-Beta: responses=experimental`.
- `originator: cha` and a CHA user agent; Pi uses its own identity here.

Keep the body small: selected `model`, full `input`, nonempty `instructions`,
`stream: true`, and `store: false`. If there are no instructions, use
`You are a helpful assistant.` Pi uses this fallback too. Reuse CHA's existing
Responses input encoding, decoder, and optional reasoning-effort handling.

Pi also sends verbosity, encrypted-reasoning inclusion, tool settings, and
optional cache/session metadata, temperature, and service tier. Their presence
does not prove they are required, or accepted by every model. CHA initially
omits those extras and `max_output_tokens`; verify the reduced body and CHA
originator with one live streamed request. Add only a demonstrated requirement,
not Pi's WebSocket/compression/cache machinery.

## Live verification record

- CHA fresh device login and refresh: **passed on 2026-09-05**. Start,
  poll, authorization-code exchange, and refresh all succeeded against
  `https://auth.openai.com` with Codex's public client ID. The stored bundle
  had `access_token`, `refresh_token`, Unix-second `expires_at`, and
  `account_id`. A near-expiry refresh succeeded, and the returned
  `refresh_token` **did rotate**. Raw bodies, tokens, device codes, and
  account values were not retained. The live start response's `interval`
  JSON type (number vs string) was not recorded; the parser accepts both.
- CHA reduced SSE request, selected model, and completion/usage events:
  **passed on 2026-09-05**. Model `gpt-5.6-terra` against
  `https://chatgpt.com/backend-api/codex/responses` with headers
  `Authorization`, `chatgpt-account-id`, `Content-Type`, `Accept`,
  `OpenAI-Beta: responses=experimental`, `originator: cha`, and
  `User-Agent: cha`. Body fields used: `model`, full conversation `input`,
  nonempty `instructions` (fallback `You are a helpful assistant.`),
  `stream: true`, `store: false`. Temperature, max output tokens, tools,
  cache/session metadata, verbosity, and encrypted-reasoning were omitted
  and were not required. The stream completed and reported usage
  (`input_tokens=19`, `output_tokens=18`). No protocol correction.

Those blocks replace these entries with dates, outcomes, tested model/request
shape, and any protocol differences. Keep source observations distinct from
live results. Never record tokens, actual device codes, account details, or
raw authentication responses here; synthetic fixtures belong in tests.
