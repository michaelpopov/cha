# chaweb implementation plan — brief

Status: proposed execution plan  
Last updated: 2026-07-30

This is a condensed guide to [`web-plan.md`](web-plan.md), which turns
[`web-design.md`](web-design.md) into 14 implementation blocks. The full plan
contains the detailed requirements, file suggestions, and test cases. The
design remains authoritative if the documents differ.

## How to use the plan

Each block should fit in one implementation session and leave the repository
building and tested. When executing a block:

1. Read the block in the full plan, its referenced design sections, and the
   current files it affects.
2. Check which dependencies are already implemented.
3. Complete the production code, focused tests, build changes, and affected
   architecture documentation.
4. Preserve the boundaries in `src/README.md`: session policy belongs in
   `session/`, web transport in `ui/web/`, and `web_main.cpp` stays a small
   composition root.
5. Add only the seams or stubs required by the current block.
6. Run focused tests, then the normal non-TUI suite. Run process and stress
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
preserve portable Linux, macOS, and Windows interfaces.

## Architecture guardrails

- Each worker process owns exactly one live session controller and its related
  transcript, journal, registry, and agent pool.
- Only the worker owner thread may use the live controller or borrowed session
  values. HTTP and SSE code receives owning copies.
- The lobby never creates a session controller. One supervisor thread owns all
  lobby-side libuv process and control handles.
- A session lease is acquired before restore and held until controller and
  journal shutdown complete.
- A worker sends exactly one startup result. Its control channel then ties its
  lifetime to the lobby.
- After redirect, the lobby forgets session and routing identity while keeping
  only generic resources needed to stop and reap the worker.
- HTTP mutations go through a serialized owner queue. A request cancelled
  before claim must never execute; a claimed request completes with its real
  domain result.
- Every SSE connection starts with a full snapshot. There is no replay log,
  SSE `id`, or `Last-Event-ID` behavior.
- Presentation revisions are worker-local and are not journal revisions.
- Only one SSE stream is supported per worker. This is a usage guard, not
  authentication or browser identity.
- The initial application targets a trusted LAN: no authentication,
  permissive CORS, global worker limit, launch quota, or launch-rate limit.
- All limits and deadlines are bounded and injectable for tests.
- Tests use fakes and short virtual deadlines. Real-process tests must not call
  a real LLM provider.
- Errors use stable codes and safe messages. Sensitive data and internal
  details are neither returned nor logged by default.
- Browser framework and UI implementation decisions are outside this plan.

## Block map

| Block | Outcome | Depends on |
| --- | --- | --- |
| 1 | Cross-process session lease for every frontend | Existing session layer |
| 2 | Create a stored session without opening it | 1 |
| 3 | Web library, tests, protocol types, and shared fixtures | Existing build |
| 4 | Startup protocol and portable worker launcher | 3 |
| 5 | Single-owner lobby worker supervisor | 4 |
| 6 | Lobby REST service and validated redirects | 2, 3, 5 |
| 7 | Worker owner runtime and cancellable command queue | 1, 3 |
| 8 | Worker snapshot and command REST API | 7 |
| 9 | Revisioned SSE with bounded backpressure | 7, 8 |
| 10 | Browser-stream and worker-lifetime state machine | 9 |
| 11 | Complete internal session-worker process mode | 4, 8–10 |
| 12 | Complete lobby process and end-to-end flow | 5, 6, 11 |
| 13 | Resource, race, stress, and conformance audit | 1–12 |
| 14 | Network, logging, platform, and documentation audit | 13 |

Numerical order is recommended. Blocks 1–2 and Block 3 begin separate
dependency branches, but keeping the normal order simplifies integration.

## Block 1 — Cross-process session lease

**Purpose:** Prevent two processes from opening the same stored session.

**Work:** Add a move-only `SessionLease` backed by a non-blocking OS file lock
on a `.cha-lock` companion file, plus a typed busy result. Acquire the lease
before restore in every create/open path and move it into `SessionController`.
Its lifetime must extend beyond journal shutdown. Keep POSIX and Win32 details
behind portable interfaces.

**Done when:** Terminal frontends report contention immediately; I/O failures
remain distinguishable from “busy”; process tests prove contention, orderly
release, and OS cleanup after a child exits.

## Block 2 — Create a stored session without opening it

**Purpose:** Let the lobby create catalog metadata without initializing a live
session.

**Work:** Add a create-only `Workspace` operation returning `SessionSummary`.
It validates static forum data and creates the database atomically, but does
not initialize providers, acquire a lease, or construct runtime objects.
Refactor the existing create-and-open path to reuse this logic.

**Done when:** The stored session appears once, labels and validation match
existing behavior, provider initialization is not triggered, and the session
can later be opened or independently become busy.

## Block 3 — Web library and protocol foundation

**Purpose:** Establish reusable server-side web boundaries before adding
listeners or routes.

**Work:** Create `cha_web` and `cha_web_tests`; define owning JSON protocol
types for summaries, personas, transcript entries, generation, snapshots,
commands, errors, and SSE events. Add shared settings, JSON/HTTP helpers, and a
temporary test-workspace builder. Keep `chaweb_app` thin and avoid selecting a
browser stack.

**Done when:** Exact JSON fixtures and enum spellings are tested, protocol
objects outlive their source data, test workspaces are reusable, and everything
builds with TUI disabled.

## Block 4 — Worker startup protocol and process launcher

**Purpose:** Start a child worker portably and receive one bounded startup
result.

**Work:** Define newline-framed JSON startup records: `ready(port)`, `busy`,
and safe `error`. Implement a libuv launcher with one duplex control pipe,
explicit handle inheritance, startup timeout, and deterministic cleanup.
Create a test child covering success, fragmentation, malformed/oversized
records, EOF, hangs, and forced termination.

**Done when:** The launcher handles fragmented and invalid input, retains
generic process/control resources after readiness, avoids sibling handle
inheritance, and closes and reaps every failure path.

## Block 5 — Lobby worker supervisor

**Purpose:** Keep all lobby-side libuv process operations on one owner thread
while serving concurrent launch requests safely.

**Work:** Add `WorkerSupervisor` with a dedicated loop, thread-safe bounded
request completion, race-safe callbacks, and orderly shutdown. After a worker
is ready, erase all session and redirect identity and retain only generic
process/control state.

**Done when:** Concurrent launches deliver exactly one result each; shutdown
resolves waiters, closes control pipes, gives workers a clean-stop deadline,
force-terminates only when needed, reaps all children, and joins its thread.

## Block 6 — Lobby HTTP service

**Purpose:** Expose lobby operations and redirect clients to ready workers.

**Work:** Add health, forum/session listing, create, and open routes. Validate
route identifiers, JSON, body limits, content type, `Host`, and same-origin
mutations. Map startup results to stable HTTP responses, including
`session_created_but_busy`. Construct safe hostname, IPv4, and bracketed IPv6
redirects. Add a framework-neutral asset-serving boundary.

**Done when:** In-process and loopback tests cover exact contracts, safe
redirects, creation races, errors, origin policy, and asset traversal. The
lobby still owns no live session object.

## Block 7 — Worker owner runtime and command queue

**Purpose:** Confine one live session and all domain access to a permanent
owner thread.

**Work:** Add `WebSessionRuntime`, typed commands, owner-thread snapshot
projection, and an owning publication interface. Reuse the owner event loop to
drain commands and agent notifications. Implement atomic
`pending → claimed/completed` or `pending → cancelled` request transitions so
cancelled work never runs and claimed work returns its real result.

**Done when:** Every controller operation and destruction occurs on the owner
thread; borrowed data never escapes; concurrent callers serialize correctly;
agent events persist without a browser; and shutdown releases all waiters.

## Block 8 — Worker snapshot and command HTTP API

**Purpose:** Provide bounded REST access to the owner runtime without moving
domain ownership into HTTP threads.

**Work:** Add worker health and snapshot routes plus raw input, stop, clear,
off-record, default-agent, and close mutations. Define strict JSON requests,
same-origin checks, task/body/prompt/socket limits, and stable error mapping.
Map pre-claim cancellation to `owner_queue_timeout`; do not retry commands
whose outcome may already be committed.

**Done when:** A loopback client can inspect and mutate a fake-backed session,
invalid input receives exact safe errors, domain refusals remain domain
results, and concurrent handlers never call the controller directly.

## Block 9 — Revisioned SSE and bounded presentation output

**Purpose:** Stream live presentation updates without letting network speed
block session progress.

**Work:** Add an owner-to-SSE channel, state projector, worker-local revisions,
named events, full-snapshot connection start, and comment heartbeats.
Coalesce compatible updates before assigning revisions. On overflow, resync
with a snapshot or close the stream; never create an unbounded queue. Use
internal stream IDs to ignore stale close callbacks.

**Done when:** Contract tests prove exact events, consecutive revisions,
snapshot recovery, no SSE `id` or replay behavior, safe multiline data, prompt
disconnect handling, and continued owner progress under slow consumers.

## Block 10 — Browser-stream and worker-lifetime state machine

**Purpose:** Define single-page connection, reconnect, idle, generation, and
shutdown behavior.

**Work:** Model `Starting`, `AwaitingClient`, `Connected`, `ReconnectGrace`,
`OrphanGeneration`, `Stopping`, and `Exiting`. Permit one active SSE stream,
support snapshot-based reconnect, continue and persist generation while
temporarily disconnected, and enforce awaiting-client and orphan limits.
Unify close, `/exit`, fatal errors, and expiry under one idempotent shutdown
coordinator with explicit response, SSE drain, listener, owner, journal, and
lease ordering.

**Done when:** Injected-time tests cover reconnects, stale stream callbacks,
idle expiry, unattended generation, hard cancellation, concurrent shutdown
triggers, and rejection of new work after stopping.

## Block 11 — Internal session-worker process mode

**Purpose:** Compose a real worker process inside `chaweb`.

**Work:** Parse private launcher arguments, initialize a unique worker log,
watch the lobby control channel, open one leased session, bind an OS-assigned
port, report exactly one startup record, and serve REST/SSE. Map lease
contention to `busy`, sanitize other startup errors, route control EOF and
signals into ordered shutdown, and use stable exit codes.

**Done when:** A real-process harness can start a worker, use health/snapshot/
SSE and a non-generating command, then close it without contacting an LLM.
Partial startup and post-ready parent loss must release listeners, processes,
and leases.

## Block 12 — Lobby process mode and end-to-end flow

**Purpose:** Complete normal `chaweb` lobby execution and the full
lobby-to-worker flow.

**Work:** Compose configuration, role-specific logging, `Workspace`,
`WorkerSupervisor`, and `LobbyServer`; locate and spawn the executable
portably; redirect only after true worker readiness; support different
sessions concurrently; and shut down workers in close, wait, force, reap
order. Document LAN binding, firewall/ephemeral ports, trusted-network
exposure, and temporary worker URLs.

**Done when:** Real-process tests cover list, create, open, redirect, snapshot,
SSE, commands, same-session races, multiple workers, clean and abrupt lobby
loss, worker crashes and reopen, IPv6-style hosts, and separate log files.

## Block 13 — Resource, race, and design-conformance audit

**Purpose:** Prove the server-side implementation is bounded and race-safe
under failure and load.

**Work:** Map the design checklist to tests; audit every lease, process,
control pipe, libuv handle, HTTP/SSE resource, queue item, waiter, and thread.
Stress reduced limits and all startup/shutdown races. Add a deterministic fake
provider process test proving generation completes and persists after SSE
disconnect. Use existing sanitizer or dynamic-analysis targets where
available, and fix every discovered correctness issue.

**Done when:** Every resource/concurrency/lifecycle requirement has a passing
test, no known resource or waiter leaks remain, race transitions are
idempotent, long tests have separate labels, and the fake-provider persistence
test requires neither credentials nor network access.

## Block 14 — Network, logging, platform, and documentation audit

**Purpose:** Finish conformance at external, operational, and native-platform
boundaries.

**Work:** Audit authority/origin validation, CORS absence, request limits,
asset safety, untrusted text handling, and response disclosure. Verify useful
but non-sensitive role/process logging. Build and test POSIX and Win32 locking,
inheritance, pipes, termination, self-location, and unusual paths on available
Linux, macOS, and Windows runners. Reconcile all architecture and user
documentation with actual limits, networking, ports, lifecycle, errors, and
deferred features.

**Done when:** The design checklist is fully linked to evidence, available
platform tests pass, unexecuted native coverage is recorded honestly, CTest
labels remain independently runnable, and documentation matches the backend.

## Deferred browser implementation

The 14 blocks stop at a complete, tested server-side boundary. A separate
design must choose and implement the browser technology.

That work must consume the established contracts rather than redesign them:

- Lobby list, create, open, redirect, and error routes.
- Worker snapshot and typed/raw command routes.
- One active SSE stream and full-snapshot reconnect.
- Revision-gap detection followed by snapshot replacement.
- Bounded retry for temporary `browser_stream_in_use` conflicts.
- Disabled controls and a clear message while another stream remains active.
- Stop, explicit close, and return-to-lobby behavior.
- Safe rendering of all server-provided text as untrusted data.

Test fixture assets are not a product UI and should not become an accidental
framework choice.
