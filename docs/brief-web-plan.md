# chaweb implementation plan — brief

Status: proposed execution plan.

Last updated: 2026-07-31.

This is a condensed guide to [`web-plan.md`](web-plan.md), which turns
[`web-design.md`](web-design.md) into 14 implementation blocks. The full plan
contains detailed requirements, likely files, tests, and completion criteria.
The design remains authoritative if the documents differ.

## How to use the plan

Each block should fit in one implementation session and leave the repository
building and tested. When executing a block:

1. Read the complete block in the full plan, its referenced design sections,
   and the current files it affects.
2. Check which dependencies are already implemented.
3. Complete production code, focused tests, build changes, and directly affected
   architecture documentation.
4. Preserve the boundaries in `src/README.md`: session policy belongs in
   `session/`, web transport in `ui/web/`, and `web_main.cpp` remains a small
   composition root.
5. Add only the seams or stubs required by the current block.
6. Run focused tests, then the normal non-TUI suite. Run real-server and stress
   tests separately through their CTest labels.
7. Report changed files, behavior, tests, and intentionally deferred work.

Baseline validation:

```bash
cmake --preset console
cmake --build --preset console
ctest --test-dir build/console --output-on-failure \
    -LE "web_process|web_stress"
```

Run `web_process` or `web_stress` separately when a block changes those tests.
Every block must keep warnings enabled, support `CHA_BUILD_TUI=OFF`, and
preserve portable Linux, macOS, and Windows abstractions.

Browser framework, component, build-tool, and styling choices remain outside
this plan. The plan does include the server asset boundary, REST/SSE contracts,
and framework-independent browser lifecycle behavior needed by that future work.

## Architecture guardrails

- `chaweb` is one process with one configured listener and one browser origin.
  It has no child session processes, internal worker mode, per-session ports, or
  control channels.
- The lobby is the server's root page and routes. It may use the shared immutable
  `Workspace` and session registry but never constructs or accesses a live
  `SessionController`.
- Each live session has one runtime and one permanent owner thread. Only that
  thread calls controller operations, reads borrowed session state, constructs
  owning transport state from it, and performs controller shutdown.
- HTTP threads have no session affinity. They resolve owning session handles,
  enqueue owning operations, and receive only owning values.
- The registry is the sole authority on in-process liveness. `starting`,
  `running`, and `stopping` entries all consume configured capacity.
- The registry mutex protects only registry state. Lease acquisition, controller
  work, socket I/O, shutdown, joins, and runtime destruction run outside it.
- Finished entries are removed under the registry mutex and joined/destroyed
  after unlocking.
- A companion-file lease is acquired before restore and held through controller
  and journal shutdown. It enforces exclusivity across `cha`, `chacon`, and
  `chaweb`; the registry enforces it inside one `chaweb` process.
- Independent controllers share no domain state, command queues, or session
  locks. `Workspace` remains immutable and safe for concurrent calls.
- Web creation and opening are separate. Creation returns a stored identity and
  starts no runtime; only open acquires a lease or constructs a controller.
- Only the owner thread completes a startup result. At its registry commit point
  it publishes either `running` or `shutting_down`, never both.
- Commands use a bounded per-session queue. `command_timeout` has unknown outcome
  and neither cancels the command nor stops the session; immediate rejection
  proves nothing executed.
- Every accepted SSE stream starts with a full snapshot. There is no event replay
  log, SSE `id`, `Last-Event-ID`, or presentation revision.
- `snapshot` and target-aware `append` are the only state-bearing events.
  Appends use per-target sequence numbers rather than byte/string offsets.
- The writer holds at most one immutable in-flight payload and one replaceable
  pending payload. The owner thread never waits for network output.
- One active SSE stream is supported per live session. This is a usage guard,
  not authentication or browser identity.
- Browser absence uses one timestamp and one deadline derived from generation
  state. There is no close endpoint, unload beacon, or attachment state machine.
- A thrown fatal session error tears down only that session. An owner thread that
  stops responding requires restarting the server.
- Process shutdown is bounded and prevents new sessions from becoming live after
  the registry stopping flag is set.
- The application targets a trusted LAN: no authentication, permissive CORS,
  built-in TLS, DNS-rebinding defense, per-client quotas, or rate limits.
- Tests use fakes, injectable limits/deadlines, and deterministic providers. They
  never call a real LLM provider.
- Errors use stable codes and presentation-safe messages. Sensitive data and
  internal details are neither returned nor logged by default.

## Block map

| Block | Outcome | Depends on |
| --- | --- | --- |
| 1 | Cross-process session lease used by every frontend | Existing session layer |
| 2 | Proven concurrent-controller and shared-workspace invariants | 1 |
| 3 | Create-only stored-session operation | 1–2 |
| 4 | Web library, protocol, settings, fixtures, and single-mode skeleton | Existing build; 3 for integration |
| 5 | Per-session owner runtime, notifier, and bounded command queue | 1, 2, 4 |
| 6 | Controller integration, snapshots, containment, and session shutdown | 5 |
| 7 | Session registry, open protocol, handles, sweeping, and capacity | 5–6 |
| 8 | Lobby routes and explicit create/open flow | 3, 4, 7 |
| 9 | Path-scoped session snapshot and command API | 6–8 |
| 10 | SSE framing, sequencing, mailbox, heartbeat, and write bounds | 6, 9 |
| 11 | Single-stream guard and disconnect-driven session lifetime | 7, 10 |
| 12 | Complete server composition and bounded process shutdown | 7–11 |
| 13 | Resource, network, logging, race, and conformance hardening | 1–12 |
| 14 | Platform, sanitizer, documentation, and final design audit | 13 |

Numerical order is recommended. Blocks 1–3 establish domain invariants, Block 4
establishes the web boundary, Blocks 5–7 build session ownership, Blocks 8–11
expose it over HTTP/SSE, and Blocks 12–14 integrate and harden the server.

## Block 1 — Cross-process session lease

**Purpose:** Prevent two processes or frontends from opening the same stored
session.

**Work:** Add a move-only `SessionLease` using a non-blocking operating-system
lock on a `.cha-lock` companion file and a typed busy result. Acquire it before
restore in every controller-opening path and move it into `SessionController`.
Its lifetime must extend beyond journal shutdown. Keep POSIX and Win32 details
behind one portable interface.

**Done when:** TUI and console report contention immediately; lock I/O failures
remain distinguishable from busy; multiple independent leases coexist; and
process tests prove contention, orderly release, and OS cleanup after a crash.

## Block 2 — Concurrent-controller and workspace invariants

**Purpose:** Prove that several controllers may run independently on different
threads inside one process.

**Work:** Document the N-controllers-on-N-threads invariant; audit libcurl
initialization, completion backends, time conversion, SQLite connections,
logging, and signal state for process-global mutation; confirm `Workspace` is
immutable and concurrently callable; and preserve atomic publish-or-retry
session creation.

**Done when:** Concurrent controller construction, deterministic generation,
journaling, creation, listing, and shutdown pass without shared state or races,
including ThreadSanitizer coverage where available.

## Block 3 — Create a stored session without opening it

**Purpose:** Let the lobby create a stored session without starting a live
lifecycle.

**Work:** Add a create-only `Workspace` operation returning `SessionSummary`.
It validates forum metadata and atomically publishes the database but does not
initialize providers, acquire a lease, create a notifier/agent pool, or construct
a controller. Keep terminal create-and-open behavior by following create-only
with the ordinary leased open path.

**Done when:** A successful create always returns the stored identity, concurrent
same-second creates lose nothing, the session can be opened separately, and a
later open failure never requires rollback or repeating creation.

## Block 4 — Web library, protocol, and server skeleton

**Purpose:** Establish reusable owning web types and the one-process application
boundary before adding live sessions or routes.

**Work:** Create `cha_web` and `cha_web_tests`; define owning summaries,
transcript/generation/snapshot values, commands/results, success bodies, errors,
and snapshot/append payloads; add exact JSON helpers and injectable limits;
create reusable test-workspace/fake-provider support; and make `chaweb_app` a
single-mode, one-listener composition skeleton.

Snapshots and open responses contain no host, port, absolute URL, or lobby
address. Error bodies have one exact `error.code`/`error.message` shape.

**Done when:** Exact JSON/code fixtures pass, protocol objects outlive their
source data, test workspaces are reusable, and the library/app build with TUI
disabled and no child-process mode.

## Block 5 — Owner runtime, notifier, and command queue

**Purpose:** Confine one live session's domain access to a permanent owner
thread.

**Work:** Add `WebSessionRuntime`, a condition-variable wake notifier, a bounded
multi-producer command queue, owning completion objects, raw input, typed Stop,
and stable-ID default-agent commands. Apply notices to runtime-owned state,
continue draining agent events without a browser, and fairly interleave commands
with notifications.

Map full-queue and stopping-before-enqueue to failures that execute nothing.
After successful enqueue, a completion timeout returns `command_timeout` with
unknown outcome but does not remove the command or stop the session.

**Done when:** All controller calls occur on the owner thread, borrowed state
never escapes, late completion is safe, queue outcomes are exact, and multiple
runtimes progress independently.

## Block 6 — Controller integration, snapshots, containment, and shutdown

**Purpose:** Complete real controller ownership, full owning snapshots,
session-local fatal-error handling, and idempotent teardown.

**Work:** Construct/open the controller on its owner thread; project forum,
session, persona, transcript, generation, reasoning, notice, and lifecycle state
into owning snapshots; classify structural changes versus append candidates;
catch thrown fatal session errors; and converge every trigger on one shutdown
sequence that rejects work, publishes a best-effort final snapshot, drains or
fails commands correctly, shuts down the controller, and releases the lease.

**Done when:** Snapshot values survive source mutation, notice semantics are
complete, a fatal failure unloads only its session, concurrent shutdown triggers
run teardown once, and no controller/journal/agent/lease survives shutdown.

## Block 7 — Session registry and open lifecycle

**Purpose:** Make one process-wide registry authoritative for opening,
reattachment, unloading, capacity, and runtime lifetime.

**Work:** Add `starting`, `running`, and `stopping` entries; a global stopping
flag; owning session handles; shared startup results; and a capacity limit that
counts every map entry. Concurrent same-session opens share one owner/result;
each waiter has its own deadline. Only the owner completes startup and atomically
publishes `ready` or `shutting_down`. Sweep finished entries in two phases so
joins and destruction occur outside the registry mutex.

**Done when:** Duplicate controllers and over-capacity threads are impossible;
all lifecycle/error branches are tested; request timeout does not cancel open;
handles protect in-flight requests; same-key reopen after teardown is safe; and
shutdown/open races never publish a live session after stopping begins.

## Block 8 — Lobby routes and explicit create/open flow

**Purpose:** Expose discovery, creation, opening, health, and same-origin session
navigation on the one listener.

**Work:** Add the lobby/asset boundary plus `GET /`, `GET /health`, forum/session
listing, create-only, and open routes. Validate identifiers before registry or
filesystem use. Creation returns `201` plus session identity and never opens;
open returns `{"path":"/s/{forum}/{session}/"}` only after readiness and
reattaches to an already-running session. Listing marks live sessions
advisorially.

**Done when:** Create/open remain separately recoverable, all response envelopes
and lifecycle codes are exact, navigation data contains no authority/port, and
the lobby never constructs a controller outside the registry.

## Block 9 — Path-scoped session HTTP API

**Purpose:** Provide snapshot and command access without giving HTTP threads
domain ownership.

**Work:** Add the chat-page boundary and session-scoped snapshot, raw-input,
typed Stop, and typed default-agent routes under
`/s/{forum}/{session}/api/v1/`. Resolve one owning running handle per request,
bound/parse input before enqueue, serve a not-open page for non-live page routes,
and map command admission, shutdown, and timeout results exactly. Add no
per-session health/status or close route.

**Done when:** Traversal and invalid routes are rejected, non-live page/API
behavior differs as specified, no HTTP result overwrites snapshot state,
non-executed commands are distinguishable from unknown-outcome timeouts, and
handlers never call a controller directly.

## Block 10 — SSE protocol and latest-state mailbox

**Purpose:** Stream presentation updates without allowing network speed to block
session progress.

**Work:** Add the session events route for `fetch` streaming; exact blank-line
SSE framing; full-snapshot connection start; answer/reasoning append targets;
per-target `seq`; one immutable in-flight and one replaceable pending payload;
append merging and snapshot replacement; comment heartbeats; and a bounded
lack-of-write-progress timeout. Emit no SSE IDs, replay behavior, or revisions.

**Done when:** Exact framing and event contracts pass; sequence continuity and
multibyte merge behavior are correct; structural/ambiguous updates collapse to
snapshots; buffering is bounded; and slow/disconnected output never blocks the
owner thread.

## Block 11 — Browser-stream guard and disconnect lifetime

**Purpose:** Enforce the supported one-page usage model and unload sessions after
browser absence.

**Work:** Serialize SSE accept/close through the owner loop; accept one stream
and reject another with `browser_stream_in_use`; use session-local connection IDs
so stale closes cannot detach a newer stream; and maintain one
`disconnected_since` timestamp plus one deadline:

```text
deadline = is_generating() ? orphan_limit : idle_grace
```

Both limits are absolute from the same timestamp. Reevaluate on every connection
or generation transition, continue generation/persistence while disconnected,
and enter ordinary session shutdown on expiry. Define bounded reload/reconnect
behavior without adding browser IDs, takeover, close endpoints, or unload
beacons.

**Done when:** Same-session stream conflicts and different-session independence
are tested, stale callbacks are harmless, initial-arrival/idle/generating
deadlines are exact, and reconnect obtains a fresh authoritative snapshot.

## Block 12 — Complete server composition and bounded process shutdown

**Purpose:** Assemble the full one-process server and ensure it can always be
stopped, even when an owner thread wedges.

**Work:** Compose configuration/logging, immutable `Workspace`, registry, routes,
HTTP pool, signal notification, and the one configured listener. Couple the
request-pool/pending bounds to the registry limit. On shutdown set the registry
stopping flag, reject opens, wake startup waiters, request session shutdown, let
owners resolve their own startup outcomes, drain executing commands, fail
unstarted commands, and join owners under one grace period. If the deadline
expires, log unfinished identities and terminate without destructors.

**Done when:** One real server serves several independent sessions on one origin;
clean shutdown joins everything and releases leases; open/shutdown races have
one outcome; stuck-owner shutdown remains bounded; and restart can reopen
previously leased sessions.

## Block 13 — Resource, network, logging, race, and conformance hardening

**Purpose:** Prove the complete server remains bounded, isolated, and safe under
contention and failure.

**Work:** Enforce body, prompt, header, connection, pending-request, queue, and
timeout limits. Require mutation content types and matching present
`Origin`/`Host`, emit no permissive CORS, and treat all rendered server text as
untrusted. Verify request-pool progress at maximum SSE occupancy; add
session-tagged, non-sensitive logging; audit every error envelope; and stress
open/unload/sweep, enqueue/shutdown, stream/stale-close, deadline/reconnect, and
cross-session fatal-error races.

**Done when:** Resource use follows documented limits, stalled readers release
HTTP threads, one session's failure never disrupts another, logs contain correct
identity but no prompt/answer bodies, and repeated lifecycle/load tests leak no
threads, descriptors, leases, waiters, handles, or entries.

## Block 14 — Platform, sanitizer, documentation, and final audit

**Purpose:** Close portable, ownership, test-evidence, and documentation gaps
without expanding product scope.

**Work:** Run ordinary, `web_process`, and `web_stress` suites; run available
ASan/UBSan/TSan builds; exercise native session locking on Linux, macOS, and
Windows; confirm one invocation and one listener; audit destruction order and
every design test bullet; reconcile code, CMake/CI, architecture docs, and user
docs; couple unsafe configuration defaults; and remove obsolete worker-process,
launch/control, per-session-port, and stale-error artifacts.

**Done when:** Available platform and sanitizer tests pass, unrun coverage is
recorded honestly, no known ownership race/leak/design contradiction remains,
and repository documentation consistently describes thread-per-session.

## Deferred browser implementation

Blocks 1–14 stop at a complete, tested server-side boundary. A separate design
and plan must choose and implement browser technology, components, build tooling,
styling, accessibility, responsive/mobile behavior, and safe rendering.

That work must consume the established contracts:

- Lobby listing plus two-step create-then-open navigation to the returned
  same-origin path.
- Path-scoped snapshot, raw-input, typed Stop, and stable-ID default-agent
  routes.
- Streaming `fetch` parsing of `snapshot`, `append`, and heartbeat records.
- Target/sequence mismatch recovery by closing and reconnecting for a fresh
  snapshot, without racing a REST snapshot against the old stream.
- Bounded retry for temporary `browser_stream_in_use` conflicts and disabled
  controls while a different page owns the stream.
- Prompt-draft preservation for Clear/off-record synthetic input and Stop.
- Request-scoped command feedback that never overwrites snapshot notice state.
- Return-to-lobby through `/`, with no close control or unload/beacon request.
- Safe rendering of every server-provided string as untrusted data.

Test fixture assets are not a product UI and must not become an accidental
framework choice.
