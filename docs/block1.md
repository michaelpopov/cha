# Block 1: ChaWeb daemon API

Status: implementation instructions; the work is not yet complete.
This block implements steps 2–4 of the ChaWeb plan. It delivers the complete
text API and its daemon tests. nginx integration is block 2, the browser is
blocks 3–4, and packaging is block 5. No earlier implementation block is needed.

This document contains the requirements for this block. The broader context is
in [chaweb.md](chaweb.md) and [chaweb-plan.md](chaweb-plan.md).

## Result and boundaries

Add a small adapter under `/api/cha/v1/` to the existing `cha-daemon`. It calls
public `app::Application` operations for existing forums and stored sessions.
It returns finite JSON responses or an empty success response. Runtime/provider
workers continue accepted generation between requests.

Keep the existing serial accept/read/handle/write/close loop. Preserve the
OpenAI adapter, its `/v1/` protocol, and its disconnect-cancels-turn behavior.
ChaWeb requests must not acquire output subscriptions or cancel accepted work
when their connection closes. Concurrent OpenAI and ChaWeb use for one user is
outside stage 1; a long OpenAI request can delay all ChaWeb requests.

Do not add threads, SSE, WebSockets, runtime commands, configuration writes,
session renaming, media endpoints, context headers, API-version wrappers, or
schema changes. Use the existing DTOs and error codes. Unused voice settings
must not block valid text operations; retain warning logging where applicable.

## Files and preparation

| File | Work |
| --- | --- |
| `src/daemon/chaweb_adapter.h` and `.cpp` | Add the route handler, validation, and application-to-HTTP mapping. |
| `src/daemon/main.cpp` | Dispatch ChaWeb paths before the existing OpenAI handler. |
| `src/daemon/scgi.h` and `.cpp` | Capture `CONTENT_TYPE` and support the required CGI statuses. |
| `CMakeLists.txt` | Compile the adapter into the daemon and daemon test targets. |
| `tests/daemon/unit_chaweb_adapter.cpp` | Add adapter tests using existing application fixtures. |
| `tests/daemon/unit_scgi.cpp` | Extend framing/header/status coverage. |
| `tests/daemon/unit_daemon_process.cpp` | Verify real-process routing and shutdown. |

Read `src/app/application.h`, `src/runtime/protocol.h` and `.cpp`, and
`src/daemon/openai_adapter.cpp` for existing operations and result handling.
Reuse the workspace setup in `tests/daemon/unit_openai_adapter.cpp` and
`tests/support/test_workspace.h`; keep OpenAI tests intact. Use
`resources/dto.yaml` and the existing wire fixtures as the JSON contract.

Use the repository's normal Linux C++ build prerequisites, CMake, and Ninja.
Install nginx and Python 3 for the next block. Use Node/npm versions pinned in
`webapp/package.json` for the baseline frontend checks. From the repository root:

```sh
npm --prefix webapp ci
cmake --preset ninja
cmake --build --preset ninja --target cha-daemon cha_daemon_tests cha_prepare_test_vault
make test
make web-check
```

Record baseline failures separately. All new tests use temporary vaults and
local/fake providers, never personal data or paid provider requests.

## Wire contract

Base path: `/api/cha/v1`. In the table, `S` is
`/forums/{forum_id}/sessions/{session_id}` below that base.

| Request | Body | Success |
| --- | --- | --- |
| `GET /bootstrap` | None | `200`, existing `Bootstrap` directly. |
| `GET /forums/{forum_id}/sessions` | None | `200`, existing `SessionListing[]`. |
| `POST /forums/{forum_id}/sessions` | `{ "text": "..." }` | `201`, existing `CreateSessionResult`: `{ "id": "...", "label": "..." }`. |
| `GET S` | None | `200`, existing `SessionSnapshot`. |
| `POST S/input` | `{ "text": "..." }` | `204`, no body. |
| `POST S/stop` | `{}` | `204`, no body. |

Match complete method/path pairs. Unsupported pairs, including `OPTIONS`, return
`404`, with no `Allow` or CORS permission headers. Reject malformed identifiers;
use IDs as opaque values, never as filesystem paths. Verify forum existence and
session membership through the application boundary. A missing resource or a
session outside the requested forum returns `404`. Match the route prefix at
its boundary so unrelated paths do not enter the adapter.

For every supported POST, require `Content-Type: application/json` before JSON
parsing or any application operation. Compare the media type case-insensitively,
allow surrounding whitespace and parameters such as `charset=utf-8`, and reject
prefix/substring lookalikes. A missing or unsupported type returns `415` with
`invalid_argument`. This prevents simple cross-origin POSTs from other websites;
do not approve their preflight requests. GET and OpenAI requests keep their
existing content-type behavior.

Require a JSON object, a string `text` for creation/input, and no unknown fields.
Stop accepts an empty object. Malformed JSON or invalid shape returns `400`.
Check decoded input size in UTF-8 bytes against
`application.settings().prompt_limit` before creation or submission. Its current
default is 32768 bytes; use the setting, not a second hard-coded limit. Leave
CHA's command parsing and semantic input acceptance to the raw input path.

Return UTF-8 JSON with its JSON content type. Error bodies use the existing
C++ `Error` serializer and `ErrorResponse` shape:

```json
{"error":{"code":"invalid_argument","message":"The input was rejected."}}
```

| HTTP status | Use |
| --- | --- |
| `400` | Invalid request or oversized prompt; preserve the applicable existing error code. |
| `404` | Unknown method/path or missing forum/session. |
| `415` | Missing/unsupported POST media type; `invalid_argument`. |
| `422` | Definitively rejected input; `invalid_argument` and CHA's safe notice. |
| `500` | Other application failures, including command timeout and cleanup failure. |
| `503` | Daemon/application stopping. |

Use safe messages and existing error codes; log unexpected exception details
instead of returning them. Do not copy the OpenAI error envelope. The global
SCGI size-limit response remains `413` outside this mapping. nginx can later
produce non-JSON `413`, `502`, or `504` errors. Cache headers belong in nginx,
not in the shared CGI writer.

## Application operations

Capture `application.context_epoch()` once per request and use that value for
all operations, including cleanup. Keep application admission checks; the epoch
is internal and never supplied by the browser. Handle both returned failure
variants and `app::ApplicationError` without leaking exception text.

For bootstrap, obtain `application.bootstrap()`, check its application state,
and serialize only `.presentation`. Keep all existing bootstrap metadata,
including Entrance, initial IDs, vault names, and character voice metadata.
Filtering displayed navigation belongs in the browser. Do not serialize the
native envelope, application capabilities, or context epoch.

For listing, validate the forum and call `list_sessions(forum_id, epoch)`.
Serialize the existing vector. An unknown forum must not look like an empty
valid forum.

Every named-session request first calls
`open_session(forum_id, session_id, epoch)`, then its operation. Reuse a live
controller or load a retired session. Return an opening or operation error
directly, without an internal retry or a replacement session.

- Snapshot: call `snapshot()` and serialize its owning `SessionSnapshot` value.
  Preserve all existing fields, including reasoning, cached-audio flags,
  `recent_pending`, and `discardable`. No web projection is needed.
- Input: call `submit(..., RawCommand{text}, epoch)`. Preserve raw text,
  `@Name`, `/mcast`, `@-`, Jev, saved defaults, and self-notes.
- Stop: call `stop()`. Loading a retired session is intentional. Successful
  Stop, including an already idle session, returns `204`; the next snapshot
  supplies its state and notice. Unknown sessions return `404` from opening.

Input is accepted only when `CommandResult.session.input_consumed` is true.
`clear_input` is not an acceptance flag: parser rejection can set it. Accepted
self-notes also succeed, even without a character reply. Rejection returns
`422 invalid_argument` with the nonempty safe notice, or the fallback message
`The input was rejected.` Classify other operation failures by the table above.

Creation follows this sequence:

1. Validate content type, JSON, forum, and input size before storing a session.
2. Call `create_session(forum_id, "", epoch)` and retain its result and ID.
3. Open that session, then submit its raw first input.
4. On acceptance, return the existing creation result with `201` immediately.
   Existing classification/naming can delay acceptance, but add no wait for a
   complete character reply or final title.
5. On definitive input rejection, delete the new session using normal
   `delete_session(..., epoch)` before returning `422`. Do not use
   `only_if_unused`: raw input can retain a session before rejecting it.
6. Also clean up a definitive failure before submission. Treat `not_found`
   during cleanup as success. Log any other cleanup failure and return `500`,
   because the session may remain.
7. Retain the session if submission timed out or its outcome is unknown.
   Retain it after acceptance, even if the response write or later generation
   fails. A lost response is neither rollback nor permission to resubmit.

Do not call `subscribe()`, `take_output()`, or `acknowledge_output()`. Do not
close sessions, stop generation, or stop the daemon merely because a ChaWeb
caller disconnects or an acknowledgement is lost. Keep these rules separate
from the OpenAI adapter's existing disconnect handling.

## SCGI and lifecycle

Add a `content_type` string to `ScgiRequest`, populated from `CONTENT_TYPE`.
Absence is valid framing; only the ChaWeb POST handler requires it. Reject
duplicate recognized fields, including this one, while ignoring unrelated SCGI
environment variables. Keep framing/content-length validation, the 64 KiB
header bound, and the 16 MiB body bound. No query parameters or arbitrary
request-header transport are needed.

Add CGI reason phrases for `201`, `204`, `415`, `422`, and `503`; preserve
existing phrases and OpenAI statuses. `204` has no body. Write CGI headers and
body bytes, not HTTP chunk framing. Closing the socket finishes the response.
Keep `write_cgi()`'s header interface unchanged.

Keep current socket cancellation, open/command deadlines, and shutdown grace
and forced-exit behavior. Add no daemon I/O deadlines or connection workers.
The serial handler waits for acceptance, so queued Stop cannot interrupt its
Jev/naming work. Current defaults include a 10-second open deadline and a
30-second command deadline; use existing settings. Provider generation runs
outside the request loop and remains observable by subsequent snapshots.

## Verification and completion

Add meaningful tests at the affected boundary:

- SCGI: split/malformed framing, absent/present/duplicate `CONTENT_TYPE`, body
  limit, CGI status phrases, bodyless `204`, and existing shutdown cancellation.
- Adapter: all six operations; unknown routes/methods; malformed requests;
  unknown fields; forum/session mismatch; prompt-byte limits before creation.
- Media type: all three POST routes reject missing and unsupported types before
  mutation; accept case/whitespace/charset variants; reject lookalikes; malformed
  JSON with a valid type returns `400`.
- DTOs: direct bootstrap/list/create/snapshot JSON matches existing serializers;
  no runtime snapshot, schema, enum, generated-type, or native-client changes.
- Lifecycle: self-note acceptance, parser rejection despite `clear_input`,
  rejection cleanup, cleanup failure, unknown submission outcome, retired-session
  loading, unknown/idle Stop, and no automatic operation retry.
- Process: dispatch through real SCGI, finite responses while generation remains
  active, persistence after request closure, and bounded existing shutdown.
  Use existing controlled provider fixtures for timing-sensitive cases.

Run the focused daemon test binary while developing, then:

```sh
make test
make web-check
```

Completion requires passing new tests and existing OpenAI/DTO checks, with any
baseline failures reported separately. The handoff is the six-operation API in
the real serial daemon. nginx end-to-end tests belong to block 2; frontend
assets and their build dependency must not be required yet.
