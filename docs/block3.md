# Block 3 — Wire shared runtime ownership and HTTP routes

Execute only this third block of six, then update the handoff below.
This document contains its implementation requirements; no earlier conversation
or separate design/protocol reading is required.

## Goal and boundaries

Expose the existing C++ OpenAI connection through CHA's shared
`ApplicationRuntime` so Linux console `chaweb` and the macOS wrapper use the
same owner, storage, and HTTP API. Leave the connection page and Mac startup
changes to subsequent blocks.

This is a personal app: one account and one pending login per workspace, one
owner/mutex, browser-driven polling. No per-browser attempt ownership, public
attempt IDs, auth SSE channel, extra worker, second lock file, or new lifecycle
framework.

## Input conditions and session setup

1. Read applicable `AGENTS.md`/`CLAUDE.md` from the repository root, run
   `git status --short`, and preserve unrelated work.
2. Blocks 1–2 must provide the tested `OpenAiOAuth`, subscription-aware
   `ProviderClient`, and successful live login/refresh/model checks. Inspect
   those headers/tests and the Handoffs in `docs/block1.md` and
   `docs/block2.md` for actual interfaces/evidence.
   Missing notes are not proof of failure or completion; verify current code
   and tests and report any still-unmet prerequisite.
3. No live account is required for this block's automated tests. Use only
   synthetic credentials and disposable database/file paths.
4. Stay below 300K session tokens, targeting a checkpoint before 200K.
   Read only relevant files/tests and persist incomplete work here.

## Existing component contract

`OpenAiOAuth` offers safe status, start, poll once, disconnect, and consistent
request access-token/account-ID copies. It owns all tokens and one pending
device attempt. Start while waiting returns the same attempt; a connected
account must be disconnected before another login.

The owner loads credentials once. Missing/invalid data leaves signed-out state;
invalid data produces a sanitized diagnostic, not fatal startup. An expired
bundle is retained for first-use renewal. No upstream call occurs on startup
or status GET.

The owner enforces a 15-minute pending deadline and next-poll time. Early
polls return a snapshot without upstream work. One eligible poll performs at
most one upstream poll and, if approved, the final exchange. Network work has
one overall 15-second deadline. The owner serializes exchanges, saving, and
disconnect with one mutex; model streaming occurs after releasing it.

Renewal occurs only on model use with five minutes left. Once sent, renewal
finishes/saves even if generation is canceled. Renewal/save failure clears
memory and attempts file removal; reconnect is manual. Removal failures must
not be presented as successful deletion. An old model 401 does not mutate
shared credentials or trigger a retry. Do not change these policies here.

## Files to inspect or change

- `src/web/application_runtime.h/.cpp`.
- `src/workspace/workspace_config_store.h/.cpp`, chiefly to reuse the
  normalized database path and existing lease/lifecycle.
- `src/web/lobby_routes.h/.cpp` and `src/web/route_support.h/.cpp` for route
  registration, JSON checks, origin checks, errors, and current conventions.
- Add `src/web/openai_auth_routes.h/.cpp` and register sources/tests in
  `CMakeLists.txt`.
- `resources/cha.yaml` and generated `webapp/src/api/schema.d.ts`.
- Add `tests/web/unit_openai_auth_routes.cpp`; extend
  `tests/web/unit_application_runtime.cpp` and relevant workspace-store tests.
  Reuse existing test helpers rather than creating another server harness.

## Ownership and storage requirements

Create the owner after the workspace store normalizes the database path and
acquires the existing exclusive lease. Append `.openai-auth.json` to that
absolute database path, not the working directory or temporary imported tree.

For `/srv/cha/cha.sqlite3`, use
`/srv/cha/cha.sqlite3.openai-auth.json`. On macOS the equivalent file lives
beside the database in Application Support, outside the replaceable app bundle.
The private JSON contains only `access_token`, `refresh_token`,
`expires_at` (Unix seconds), and `account_id`, and uses existing atomic
private-file handling with mode 0600.

Share exactly this owner with the HTTP routes and the existing provider
factory. Keep it alive until all HTTP handlers and provider workers finish
through the existing shutdown sequence. Workspace maintenance/republication
must not create another owner, reload credentials, or disconnect the account.

The sibling file is not workspace configuration, SQLite data, a transcript,
or a transferable database asset. Exclude it from imports/exports, R2 database
transfers, transcript mirrors, and packaging. No extra credential lock is needed.

## Public HTTP contract

All endpoints are root-scoped, not session- or character-scoped:

| Method and route | Action |
| --- | --- |
| `GET /api/v1/openai/auth` | Return displayable status only |
| `POST /api/v1/openai/auth/login` | Start login or return the current pending attempt |
| `POST /api/v1/openai/auth/poll` | Perform at most one eligible owner poll |
| `POST /api/v1/openai/auth/disconnect` | Cancel pending login or disconnect/remove credentials |

POST bodies are `{}`. Apply existing JSON/content-type/body/origin rules.
Keep the macOS `CHA_RUNTIME` cookie protection; adding OAuth does not replace
it or add authentication to publicly exposed Linux `chaweb`.

Use a minimal snapshot with status values `signed_out`, `waiting`, and
`connected`. Only waiting includes a user code, verification URL
(`https://auth.openai.com/codex/device`), attempt expiry, and next-poll delay.
Choose simple field names consistent with current API conventions and specify
timing units explicitly in OpenAPI. Record exact synthetic examples below.
Do not expose email/profile data, account ID, access/refresh tokens, private
device ID, authorization code, or verifier.

Use existing error envelopes with sanitized messages. Send
`Cache-Control: no-store` on all auth responses, including error responses.
Do not serialize the owner's internal credential object or raw upstream body.
There are no extra recovery/storage-error states.

## Implementation steps

1. Trace runtime construction, store normalization/lease, provider factory,
   handler registration, maintenance, and shutdown. Make the smallest lifetime
   change that introduces one shared auth owner. Do not add new shutdown
   threads or an independently owned owner in each handler.
2. Supply the owner to subscription-aware provider clients through the existing
   factory. Ordinary providers retain their current behavior and must not
   require a connected account to load or run.
3. Implement the four routes using the contracts above. Each handler calls one
   owner operation; do not port Pi's long-running device polling loop into an
   HTTP request. Status reads do not call upstream, and repeated/early requests
   rely on the owner's shared state/timing.
4. Define/document the public snapshot and error responses in OpenAPI. Use
   timing units that the browser can schedule directly or trivially convert.
   Status on page re-entry must let the client resume an unexpired attempt.
5. Apply no-store and existing validation/cookie protection, including error
   paths. Test that shared ownership means one browser can cancel a login
   started by another; no instant cross-tab synchronization is required.
6. Verify import/export, R2 transfer, and workspace maintenance paths leave
   the auth owner/file alone. Add focused regression tests using synthetic
   secrets and the existing facilities; avoid a new transfer framework.
7. Regenerate API types. Make only compatibility changes needed to keep the
   current frontend compiling; adding client methods/navigation/page is block 4.

## Completion verification

From the repository root:

```sh
cmake --preset ninja
cmake --build build/ninja --target cha_tests cha_web_tests
./build/ninja/cha_tests --gtest_list_tests
./build/ninja/cha_web_tests --gtest_list_tests
./build/ninja/cha_tests
./build/ninja/cha_web_tests
```

From `webapp`, use `npm ci` if dependencies are missing, then:

```sh
npm run api-types
npm run check
```

Do not manually edit generated `schema.d.ts`. Focused filters are fine while
editing, but must match actual tests. Before handoff verify:

- Four route contracts, malformed requests/content types/origins, sanitized
  errors, no-store on success/errors, and no upstream work on status GET.
- Safe signed-out/waiting/connected snapshots contain no private fields.
  Early polls and multiple browsers do not multiply upstream polls.
- One shared owner, normalized sibling path, signed-out startup, cookie gate,
  maintenance preservation, and shutdown during a bounded auth operation.
- Synthetic credentials do not appear in responses, logs, SQLite, exports,
  R2 transfers, or mirrors. File creation still uses private atomic handling.
- C++ and frontend checks pass. Record concrete API examples sufficient for
  the next session to implement the UI without this session's conversation.

## Handoff

Persist the actual interface and verification here, not in duplicate overview
notes. Stop before implementing the page or macOS startup.

- Status: not started.
- Changed files and runtime/factory/lifetime arrangement: none yet.
- Exact response fields and timing units: not implemented.
- Synthetic examples for signed out, waiting, connected, and errors: not yet.
- Route method names/behavior and generated type names: not implemented.
- Commands, test counts, and results: not run.
- Remaining work, limitations, or unavailable checks: none recorded.
- Ready for block 4: no; requires the working shared API, generated types,
  safe response examples, and passing checks.
