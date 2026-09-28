# Stage 2 of 2: Chat turns, output and final validation

## Assignment

Implement this stage in a fresh Grok coding-agent context after
[block1.md](block1.md) passes its exit criteria. Continue from its repository
state and execution record. Complete the daemon described by
[headless.md](headless.md) and the revised [head-plan.md](head-plan.md).

This stage covers plan steps 4–5, active-request shutdown and remaining checks
from step 6, and final deployment/client validation from step 7. It delivers
both streaming and non-streaming Chat Completions, session continuation,
correct error handling and cancellation, and the final Linux deployment.
No third implementation stage is planned.

The revised plan is deliberate: use blocking `Application::submit()`, shut
down the daemon on `command_timeout`, delete a newly created session after a
definitively rejected first input, and retain macOS build/test support. Do not
replace these decisions with the superseded asynchronous submission plan.

## Context budget and starting checks

This stage is scoped for one 500K window. Aim to keep initial reading below
150K and reserve at least 200K for edits, tests, debugging and review. These
are planning budgets, not exact tokenizer measurements. The existing
application/turn code and focused test references inspected during staging
totaled about 380 KiB; selective reads plus the small stage-1 implementation
leave substantial room for the work.

Read `AGENTS.md`, both design/plan documents and stage 1's execution record.
Inspect `git status` and the current daemon files before changing them. Verify
that the Linux baseline and nginx model-list smoke test passed. If required
evidence is missing, resolve that prerequisite before implementing turns, as
the main plan requires. Do not repeat a passed server setup exercise without
a relevant change or failure.

| Read first | Purpose |
| --- | --- |
| `src/daemon/*`, `tests/daemon/*`, daemon CMake wiring and `packaging/linux/*` | Continue the existing implementation and remove its temporary chat response |
| `src/app/application.h`; session, subscription and shutdown methods in `.cpp` | Public calls, epoch handling and error variants |
| `src/runtime/protocol.h`, `live_session.h` and relevant `.cpp` functions | Result types, subscription endpoint, output acknowledgement and terminal states |
| `src/runtime/session_output.*`, `runtime_settings.h` | Coalescing, snapshots, limits and shutdown grace |
| `src/runtime/text_input.cpp`, relevant `src/session/session_controller.cpp` functions | Rejection, Jev, multicast, stop and session retention semantics |
| `src/chat/transcript.h`, `src/session/controller_update.h`, `src/runtime/session_projection.cpp` | Entry types, appends and presentation rewrites |
| Focused tests in `tests/app/unit_application.cpp`, `tests/runtime/unit_session_output.cpp`, `unit_live_session.cpp`, `unit_jev.cpp` and `tests/app/unit_session_retirement.cpp` | Existing fixtures and lifecycle behavior |

Read deeper into storage/providers only to answer a concrete implementation or
test question. Keep production changes in the daemon unless a demonstrated
portability fix is required. No new application threads, persistent API state,
database schema, general transport framework or external dependency is needed.

## Work sequence

### 1. Implement the complete turn lifecycle

Replace stage 1's temporary `501` branch with a single request-local turn
handler in `openai_adapter.cpp`. Reuse its parsed request, tag helpers and
transport. Keep state local to the current request.

1. Capture the application's nonzero context epoch. If no assistant message
   was supplied, create a session with an empty label. Otherwise use the
   selected tag. Call `open_session()` for either case. Return `404` for a
   missing tagged session, without falling back to another tag or creating
   a replacement session.
2. Subscribe with request-specific connection/subscription IDs and the epoch.
   Retain `SubscribeResult::session`. Consume the first snapshot, record its
   highest entry ID (zero if empty), and acknowledge it before submitting.
   Do not access a controller or runtime internals directly.
3. Submit the final user text unchanged in `RawCommand` using blocking
   `Application::submit()`. The existing deadline governs the wait. Check
   peer closure and the signal stop flag immediately after it returns.
   Do not send SSE headers before acceptance.
4. Acceptance requires a `CommandResult` with
   `session.input_consumed == true`; `clear_input` alone is insufficient.
   Rejections return `400` with the CHA notice or failure message. Keep
   operational errors distinct from invalid input. A `command_timeout` means
   the command may still execute: request bounded process shutdown and do not
   accept another connection.
5. After acceptance, poll connection state, the stop flag and `take_output()`
   every 25 ms. Track entries with IDs greater than the baseline. Snapshots
   replace tracked entry state; `EntryTextTarget` appends extend the matching
   entry. Ignore reasoning appends. Acknowledge every consumed output item,
   including ignored items and output drained after disconnect.
6. A normal turn finishes only on a snapshot with
   `generation.active == false` and at least one new entry. Jev and all
   multicast recipients belong to the same turn. Naming updates and notices
   are not assistant replies. A new error entry means a provider failure at
   turn end. Terminal lifecycle snapshots and closed output need explicit
   handling so storage/session failures cannot leave the loop waiting forever.
7. On disconnect or failed write, call `stop()` once and drain without writes
   until the session is idle or terminal. Cancellation before a new entry is
   committed must not wait forever for a new entry. If idle/terminal state
   cannot be established, shut down the process instead of admitting more
   work against unresolved state.
8. Unsubscribe on all paths after a successful subscription, with the same
   IDs and epoch. After unsubscribe, delete a session created by this request
   if CHA definitively rejected its first input. The parser retains sessions
   before validating input, so startup pruning is insufficient. Do not use
   only-if-unused deletion if that retention would make it a no-op. Never
   delete an existing session, an accepted partial turn or a session whose
   acceptance is uncertain after a timeout.

Use small scope-bound cleanup helpers, not a general state-machine framework.
Keep normal idle sessions selected between requests. `open_session()` already
uses selection and retirement; do not use `LiveSessionManager::open()` or
maintain a separate cache of live sessions. Preserve the existing forum persona
and Jev target behavior. Do not save a new forum default from API input.

### 2. Add both response modes to the same turn handler

Keep collection and rendering shared between modes. It is reasonable to get
non-streaming working first internally, then add SSE before this stage ends.
Never create separate lifecycle/cancellation implementations for the modes.

Render character entries in transcript order as `**Name:** text`, separated
by blank lines. Use entry display names. If the completed turn contains only
its human entry, use `(recorded)`. Suppress reasoning and ordinary notices.
Prepend the tag plus a blank line only in API output, never in storage.

For non-streaming, monitor disconnects throughout collection. Return one
`chat.completion`, one choice at index zero, assistant role, the complete
tagged text and `finish_reason = "stop"`. Do not emit success headers until
the outcome is known. Return `502` for provider failure and `500` for
storage/internal failure. Do not invent aggregate token usage.

For streaming:

- Immediately after acceptance, send CGI SSE headers and a first
  `chat.completion.chunk` containing assistant role and the tag. Use one
  completion ID, timestamp and model for the whole response.
- Emit text through `choices[0].delta.content`. Track emitted text separately
  for each reply, excluding the tag. Send appends once. On a full snapshot,
  emit only the suffix after the already-sent prefix.
- If a snapshot rewrites that prefix, log once and stop sending that reply.
  Continue acknowledgements and later multicast replies. SSE cannot retract
  content; never resend a full snapshot as a new answer. The non-streaming
  result can use the final snapshot's complete text.
- Send `: keepalive\n\n` after 15 seconds without text and repeat during quiet
  generation. Use a monotonic clock. Keepalive timing starts only after SSE
  headers; the blocking acceptance wait can still return a JSON error.
- On success, send an empty delta with `finish_reason = "stop"`, then
  `data: [DONE]\n\n`. After headers, report provider/internal errors as one
  final SSE error event and close without a success chunk or `[DONE]`.

Use the existing OpenAI error helper for all error envelopes. Keep malformed
requests/rejected input (`400`), absence (`404`), provider failure (`502`) and
storage/internal failure (`500`) separate. Remove the intermediate `501`
branch completely. Do not log credentials or complete request bodies.

### 3. Finish shutdown and failure integration

Extend stage 1's signal flag and shutdown path to active chat requests. Keep
`SA_RESTART`; never call the application from a signal handler. Check the
flag during output polling and after blocking submission returns.

Use one bounded shutdown budget for cancellation, draining and
`request_shutdown()`/`join_shutdown()`. Do not grant each cleanup step a fresh
full grace period or wait indefinitely for a turn-ending snapshot. Preserve
the revised plan's `_exit(EXIT_FAILURE)` fallback when joining fails. The
blocking submission can delay observation of the stop flag; preserve the
existing command deadline and account for it in the service stop timeout.

No request may be accepted after a submission timeout or unrecoverable cleanup
failure. A normal client disconnect must allow subsequent requests once its
turn is idle or terminal. Preserve partial responses according to the core's
existing rules.

Reuse the executable launcher from stage 1 to automate shutdown during
generation. Together with invalid activation, this completes the two requested
executable-level tests. Keep the other deployment/startup scenarios manual,
as the revised plan specifies.

### 4. Add focused behavioral tests

Extend `tests/daemon/` using temporary databases, the deterministic test
provider and existing mock provider facilities. Do not require paid provider
calls for automated tests. Cover these groups:

| Group | Required cases |
| --- | --- |
| Identity and storage | New chat, tagged continuation, missing tagged session, pasted user-side tag creates a separate session, no client-history replay, no stored API tag |
| Input behavior | Plain text, `@Name`, `/mcast`, `@-`, empty/invalid mentions, unknown slash commands, failed multicast, Jev acceptance/rejection |
| Rejection cleanup | Rejected first input leaves no session; rejection in an existing session preserves its history; `clear_input=true` without consumption never starts a response |
| Output | Immediate completion, ordered multicast labels, snapshot coalescing, duplicate snapshots, prefix mismatch followed by another reply, ignored reasoning, `(recorded)` |
| Response protocol | Decode JSON/SSE, consistent completion metadata, streamed/non-streamed text agreement where snapshots remain append-only, finish/error framing, keepalives |
| Failure and cancellation | Provider failure in both modes, terminal/closed output, disconnect in both modes, next request after cancellation, submission timeout causes daemon shutdown |
| Session lifecycle | Alternate among more than eight stored sessions, continue after partial output/reopen, regenerate/edit appends rather than deletes, no forum-default writes |

Use a controllable time value for keepalive testing instead of a 15-second
sleep. Test timeout behavior at the adapter level using the existing runtime
settings/test seams; do not add a third executable-level scenario or a new
production configuration option just for tests. Exercise meaningful failure
paths without adding a general fake application framework.

### 5. Complete deployment and client validation

Finish the existing `packaging/linux/` examples in place. Preserve stage 1's
socket activation, authentication boundary and serial request handling. Check
the actual nginx group, SCGI parameters, socket permissions and service stop
timeout. Do not introduce a provisioning framework.

Run `nginx -t`, verify the units, and exercise the full step-7 matrix from
`head-plan.md` with two disposable users. The following must have recorded
results, including checks that can now reuse stage 1 evidence:

- First-connection activation, correct model listing and both response modes.
- A second chat and model-list request wait behind a slow turn; queued clients
  that disconnect with small or large bodies create no turn.
- Active disconnect cancels and permits the next request; partial output can
  be continued using the preserved tag.
- Missing/unknown keys return nginx `401`; missing socket/startup failure
  returns nginx `502`; oversized bodies get `413`; authorization headers
  never reach the daemon.
- A user cannot reach another user's vault through a session tag.
- Protected/unprotected startup, missing/wrong password errors, and rejection
  of a second process opening the same vault.
- Clean stop while idle or reading a partial request, stop during generation,
  crash/restart recovery, lease release, private workspace cleanup and stored
  session continuation after restart.
- Provider errors yield the correct non-streaming status or SSE error event.

Use one intended OpenAI-compatible client with automatic titles, tags and
follow-up requests disabled. Confirm that it preserves assistant tags,
displays speaker labels, tolerates SSE comments and continues after a stopped
reply. Regenerate/edit appends a turn; it does not remove prior stored turns.
With no remaining assistant tag, editing the first turn creates a new session.

Run daemon tests on macOS and Linux and the Linux core/application tests. Run
the relevant desktop suites if shared code changed. Confirm that the daemon
adds no application threads or queues and starts no media downloads. Treat
unavailable host/client access as an explicit unverified check, never as a
passed check; complete independent work before reporting that limitation.

## Exit criteria

- [ ] Stage 1's parsing, models and transport tests still pass.
- [ ] All lifecycle, output and failure test groups above pass.
- [ ] The executable shutdown-during-generation test passes; invalid activation
  still passes. No intermediate chat placeholder remains.
- [ ] Daemon tests pass on macOS and Linux; required existing suites pass.
- [ ] The full nginx/systemd matrix passes with two users, with evidence for
  queueing, cancellation, isolation and recovery.
- [ ] The intended chat client passes the tag, streaming and continuation checks.
- [ ] Only the two API endpoints are exposed. No storage/threading redesign,
  login flow, rollback, media endpoint or configuration API has been added.
- [ ] Review confirms no leaked subscriptions/descriptors, no abandoned busy
  session accepted by the next request, and bounded failure shutdown.

## Completion report

Add a short execution record below and include it in the final report. Record
the repository revision or exact working-tree state, changed files, commands
and results by platform, deployment/client results and any remaining failures
or unverified requirements. Include no secrets. This record is the only change
to this stage document needed during implementation; leave the design and main
plan intact.

Declare the implementation complete only when all required exit criteria pass.
The open questions about login, configuration import/export and branching
remain future work and do not block this release.

### Execution record

Not run yet.
