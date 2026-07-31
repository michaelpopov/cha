# chaweb implementation plan — brief

Status: proposed execution plan  
Last updated: 2026-07-31

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
- The lobby never creates a session controller. One control-loop thread owns
  all lobby-side control channels and launch timers.
- Workers are fire-and-forget processes. The POSIX lobby enables automatic
  child reaping; the Windows lobby uses `CREATE_NO_WINDOW` for session workers
  and immediately closes native process/thread handles. Workers inherit no
  console streams and create no console windows; lobby mode may use its normal
  console. The lobby retains no PID or process handle and never waits for,
  reaps, or force-terminates a worker.
- A session lease is acquired before restore and held until controller and
  journal shutdown complete.
- A worker sends exactly one startup result. Its control channel then ties its
  lifetime to the lobby.
- After returning a ready port, the lobby forgets session and routing identity
  while keeping only a generic control endpoint so both sides can detect EOF.
- HTTP mutations go through a serialized owner queue. An accepted command has
  one generous completion deadline; expiry has unknown outcome and starts
  worker shutdown rather than cancelling the queued command.
- Every SSE connection starts with a full snapshot. There is no replay log,
  SSE `id`, or `Last-Event-ID` behavior.
- `snapshot` and target-aware `append` are the only state-bearing SSE events.
  The writer has one immutable in-flight payload and at most one replaceable
  pending payload; the owner never waits for it.
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
| 4 | Startup protocol and portable fire-and-forget worker launcher | 3 |
| 5 | Single-owner lobby worker control loop | 4 |
| 6 | Lobby REST service and ready-port responses | 2, 3, 5 |
| 7 | Worker owner runtime and bounded command queue | 1, 3 |
| 8 | Worker snapshot and command REST API | 7 |
| 9 | Snapshot/append SSE with latest-state mailbox | 7, 8 |
| 10 | Browser-stream and worker lifetime | 9 |
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
types for summaries, personas, transcript entries, generation, snapshots
including `lobby_port`, current notice, optional foreground request identity,
and coarse lifecycle phase, commands, errors, and snapshot/append SSE payloads.
Add shared settings, JSON/HTTP helpers, and a temporary test-workspace builder.
Keep `chaweb_app` thin and avoid selecting a browser stack.

**Done when:** Exact JSON fixtures and enum spellings are tested, protocol
objects outlive their source data, test workspaces are reusable, and everything
builds with TUI disabled.

## Block 4 — Worker startup protocol and process launcher

**Purpose:** Start a fire-and-forget worker portably and receive one bounded
startup result.

**Work:** Define newline-framed JSON startup records: `ready(port)`, `busy`,
and safe `error`. Add a utility-layer `posix_spawn()`/`CreateProcessW()` wrapper
that accepts an explicit inherited descriptor/handle set and retains no process
identity. Enable POSIX `SA_NOCLDWAIT`, close Windows process/thread handles
immediately, and use `CREATE_NO_WINDOW` with no inherited console streams for
Windows workers. Add a web launcher with one duplex control pipe and startup
timeout. Create a deterministic test worker covering success, fragmentation,
malformed/oversized records, EOF, parent loss, and Windows console absence.

**Done when:** The launcher handles fragmented and invalid input, retains
only the control endpoint after readiness, avoids sibling handle inheritance,
and leaves no zombie, waitable status, PID, or native process handle.

## Block 5 — Lobby worker control loop

**Purpose:** Keep all lobby-side control-channel operations on one owner thread
while serving concurrent launch requests safely.

**Work:** Add `WorkerControlLoop` with a dedicated loop, thread-safe bounded
request completion, race-safe EOF callbacks, and orderly channel teardown.
After a worker is ready, erase all session, worker-port, request-completion,
and temporary spawn data and retain only generic control-endpoint state.

**Done when:** Concurrent launches deliver exactly one result each; shutdown
resolves waiters, closes control pipes, does not wait for worker termination,
and joins its thread. Cooperative workers independently observe EOF and exit.

## Block 6 — Lobby HTTP service

**Purpose:** Expose lobby operations and return ready worker ports to clients.

**Work:** Add health, forum/session listing, create, and open routes. Validate
route identifiers, JSON, body limits, content type, `Host`, and same-origin
mutations. Map startup results to stable HTTP responses, including
`session_created_but_busy`. Return `{"port":<worker-port>}` on success; do not
return `Location`, construct navigation URLs from `Host`, or add an advertised
hostname. Keep `Host`/`Origin` validation only for same-origin mutations. Add a
framework-neutral asset-serving boundary.

**Done when:** In-process and loopback tests cover exact port JSON, invalid
ports, creation races, errors, origin policy, and asset traversal. The lobby
still owns no live session object.

## Block 7 — Worker owner runtime and command queue

**Purpose:** Confine one live session and all domain access to a permanent
owner thread.

**Work:** Add `WebSessionRuntime`, raw-input plus typed Stop and stable-ID
default-agent commands, owner-thread snapshot projection, and an owning
publication interface. Clear and off-record controls use `/clear`, `/hide-on`,
`/hide`, and `/hide-off` through raw input rather than distinct runtime
commands. Reuse the owner event loop to drain commands and agent notifications.
Use an owning completion object and one generous deadline from successful
enqueue through command completion. Expiry has unknown outcome, returns
`worker_unresponsive` when possible, and initiates idempotent worker shutdown;
late completion must be harmless. Fairly interleave commands and agent events.
Make snapshots authoritative for current notice; HTTP response notices remain
request-scoped and cannot overwrite newer snapshot state.

**Done when:** Every controller operation and destruction occurs on the owner
thread; borrowed data never escapes; concurrent callers serialize correctly;
agent events persist without a browser; and shutdown releases all waiters.

## Block 8 — Worker snapshot and command HTTP API

**Purpose:** Provide bounded REST access to the owner runtime without moving
domain ownership into HTTP threads.

**Work:** Add worker health and snapshot routes plus raw input and typed Stop
and stable-ID default-agent mutations. Clear and off-record browser controls
submit their shared slash-command strings through raw input; do not add
separate routes for them. Do not add a close route. Define strict JSON
requests, same-origin checks, task/body/prompt/socket limits, and stable error
mapping. Apply one generous command-completion deadline after enqueue. Expiry
reports `worker_unresponsive` with unknown outcome and initiates idempotent
shutdown; do not retry the command automatically. Snapshots include current
notice, optional foreground request identity, coarse lifecycle phase, and
`lobby_port` without a presentation revision.

**Done when:** A loopback client can inspect and mutate a fake-backed session,
invalid input receives exact safe errors, domain refusals remain domain
results, and concurrent handlers never call the controller directly.

## Block 9 — Snapshot/append SSE and latest-state mailbox

**Purpose:** Stream live presentation updates without letting network speed
block session progress.

**Work:** Define complete `snapshot` and target-aware `append` payloads for
answer and reasoning text, with `length_before` measured in UTF-8 bytes. Publish
snapshots for structural or ambiguous changes. Give the writer one immutable
in-flight payload and one replaceable pending payload: merge compatible
appends, rebuild a pending snapshot after later changes, and replace any unsafe
append with a snapshot. Add full-snapshot connection start, comment heartbeats,
and internal stream IDs. Serialize no presentation revision or `resync`.

**Done when:** Contract tests prove the two exact event types, UTF-8 continuity,
mailbox replacement under a blocked writer, no SSE `id` or replay behavior,
safe multiline data, prompt disconnect handling, and continued owner progress.

## Block 10 — Browser-stream and worker lifetime

**Purpose:** Define single-page connection, reconnect, disconnect timing,
generation, and shutdown behavior.

**Work:** Keep only coarse startup, running, and stopping phases. Track an
optional active stream ID, whose presence defines `stream_active`, optional
`disconnected_since`, and one rearmable timer. At readiness and matching stream
close, record the disconnect time; an accepted stream clears it. While
disconnected, use `idle_grace` when idle and the absolute `orphan_limit` when
generating, both measured from the same timestamp. Reevaluate on every
generation transition and require `orphan_limit >= idle_grace`. Treat tab
close, reload, navigation, browser failure, network loss, and device suspension
identically; use no close endpoint or unload/beacon signal. Unify deadline
expiry, fatal errors, lobby loss, and process signals under one idempotent
shutdown coordinator with bounded final snapshot drain and ordered listener,
owner, journal, and lease teardown.

**Done when:** Injected-time tests cover initial arrival, reconnects, stale
stream callbacks, idle and generating deadlines, generation transitions, hard
cancellation, concurrent shutdown triggers, and rejection of new work after
stopping.

## Block 11 — Internal session-worker process mode

**Purpose:** Compose a real worker process inside `chaweb`.

**Work:** Parse private launcher arguments including the explicit workspace
root, initialize a unique worker log, watch the lobby control channel, open one
leased session, bind an OS-assigned port, report exactly one startup record,
and serve REST/SSE. Load the same application configuration through that root
and expose its port as snapshot `lobby_port`; pass no external lobby URL. Map
lease contention to `busy`, sanitize other startup errors, route control EOF
and signals into ordered shutdown, and use stable exit codes.

**Done when:** A real-process harness can start a worker, use health/snapshot/
SSE and a non-generating command, then close it without contacting an LLM.
Partial startup and post-ready parent loss must release listeners, processes,
and leases.

## Block 12 — Lobby process mode and end-to-end flow

**Purpose:** Complete normal `chaweb` lobby execution and the full
lobby-to-worker flow.

**Work:** Compose configuration, role-specific logging, `Workspace`,
`WorkerControlLoop`, and `LobbyServer`; locate and spawn the executable
portably; return a worker port only after true readiness; support different
sessions concurrently; and close every worker-control endpoint during lobby
shutdown without waiting for worker processes. Document LAN binding,
firewall/ephemeral ports, trusted-network exposure, and temporary worker URLs.

**Done when:** Real-process tests cover list, create, open, ready-port response,
snapshot, SSE, commands, same-session races, multiple workers, clean and abrupt
lobby loss, worker crashes and reopen, and separate log files.

## Block 13 — Resource, race, and design-conformance audit

**Purpose:** Prove the server-side implementation is bounded and race-safe
under failure and load.

**Work:** Map the design checklist to tests; audit every lease, process,
control pipe, temporary spawn resource, libuv handle, HTTP/SSE resource, queue
item, waiter, and thread.
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

**Work:** Audit ready-port JSON, Host/Origin mutation validation, CORS absence,
request limits, asset safety, untrusted text handling, and response disclosure.
Verify useful but non-sensitive role/process logging. Build and test POSIX and
Win32 locking, inheritance, pipes, POSIX automatic child disposal, immediate
Windows process handle closure, hidden Windows worker creation, self-location,
and unusual paths on available Linux, macOS, and Windows runners. Reconcile all
architecture and user documentation with actual limits, networking, ports,
lifecycle, errors, and deferred features.

**Done when:** The design checklist is fully linked to evidence, available
platform tests pass, unexecuted native coverage is recorded honestly, CTest
labels remain independently runnable, and documentation matches the backend.

## Deferred browser implementation

The 14 blocks stop at a complete, tested server-side boundary. A separate
design must choose and implement the browser technology.

That work must consume the established contracts rather than redesign them:

- Lobby list, create, open, ready-port, and error routes. On success, construct
  a URL from the lobby page URL, replace its port through the URL API, clear
  lobby path/query/fragment state, and navigate with `location.assign()`; test
  hostname, IPv4, and bracketed IPv6 page URLs.
- Worker snapshot, raw-input, typed Stop, and stable-ID default-agent routes.
  Clear and off-record controls submit `/clear`, `/hide-on`, `/hide`, and
  `/hide-off` as synthetic raw input without replacing or clearing the editor
  draft.
- One active SSE stream and full-snapshot reconnect.
- Apply answer and reasoning appends only when their target and UTF-8
  `length_before` match; otherwise close the old stream and reconnect for a
  snapshot without a concurrent REST refetch.
- Bounded retry for temporary `browser_stream_in_use` conflicts.
- Disabled controls and a clear message while another stream remains active.
- Stop, tab-close/disconnect expiry, and return-to-lobby behavior, with no
  Close control or unload/beacon close request.
- Construct the return-to-lobby link through the URL API from the worker page
  URL and snapshot `lobby_port`.
- Safe rendering of all server-provided text as untrusted data.

Test fixture assets are not a product UI and should not become an accidental
framework choice.
