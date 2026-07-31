# chaweb implementation plan

Status: proposed execution plan.

Last updated: 2026-07-31.

This document turns [`web-design.md`](web-design.md) into bounded implementation
blocks. Each numbered block is intended to fit in one Codex session and leave the
repository building and tested. The blocks are smaller than the design's
delivery steps where concurrency, streaming, or lifecycle work would otherwise
create too much context for one session.

The design document remains authoritative. If this plan and the design differ,
follow the design and update this plan in the same change.

## 1. How to execute this plan

A request such as “execute block 5” means:

1. Read this introduction, the complete requested block, the named sections of
   `web-design.md`, and the current versions of every file the block will touch.
2. Check which preceding blocks are already present. Do not assume the repository
   exactly matches the file suggestions below.
3. Implement the block completely, including production code, focused tests,
   build changes, and directly affected architecture documentation.
4. Preserve the dependency boundaries in `src/README.md`: session policy stays
   in `session/`, web transport stays in `ui/web/`, and `web_main.cpp` remains a
   small composition root.
5. Do not implement later blocks merely because a placeholder or interface is
   needed. Add the smallest tested seam that gives the current block a real
   boundary.
6. Run the focused tests for the block, then build and run the ordinary non-TUI
   suite:

   ```bash
   cmake --preset console
   cmake --build --preset console
   ctest --test-dir build/console --output-on-failure \
       -LE "web_process|web_stress"
   ```

   When the block adds or changes a web process or stress test, also run its
   focused target or label, for example:

   ```bash
   ctest --test-dir build/console --output-on-failure -L web_process
   ```

   Existing configured build directories may be reused when appropriate.
7. Report the files changed, behavioral result, tests run, and any limitation
   intentionally left to a later block.

Every block must keep warnings enabled and work with `CHA_BUILD_TUI=OFF`.
Platform-specific code must stay behind a portable interface and compile on
Linux, macOS, and Windows. A platform-conditional test is acceptable when the
repository's process-test harness is not portable, but the feature itself must
not silently become platform-specific.

When development has only a Linux host, a successful Linux build plus the
applicable Linux unit/process tests is the minimum local completion bar for
Blocks 1–13. Code must still preserve the portable abstractions required by the
design. Block 14 exercises macOS and Windows verification where runners exist
and records any platform that was not run.

Do not choose a browser framework, language, component library, bundler, or CSS
system while executing this plan. The actual lobby and chat browser
implementations need a separate design and plan. The server-side asset boundary,
REST API, SSE protocol, and framework-independent browser lifecycle contract are
included because that later implementation depends on them.

## 2. Global implementation rules

These rules apply to every block:

- `chaweb` is one process with one configured listener and one origin. It has no
  internal worker mode, child-process launcher, control channel, per-session
  listener, or ready-port protocol.
- The lobby is the root page and routes of that server. Lobby routes may use the
  shared immutable `Workspace` and the session registry but never construct or
  access a live `SessionController`.
- Each live session has one runtime and one permanent owner thread. Only that
  thread calls controller commands, calls `receive()`, reads borrowed session
  state, constructs owning transport state from it, or shuts the controller
  down.
- HTTP request threads have no session affinity. They resolve an owning session
  handle, enqueue owning commands or connection notifications, and receive only
  owning results.
- The registry is the sole authority on in-process liveness. Its map contains
  `starting`, `running`, and `stopping` entries; every entry counts against the
  configured limit.
- The registry mutex protects only map state, lifecycle transitions, the global
  stopping flag, and the `finished` flag. Lease acquisition, controller work,
  socket I/O, shutdown, thread joins, and runtime destruction never run while it
  is held.
- Finished entries are swept in two phases: remove them under the registry mutex,
  then join threads and drop runtime references after unlocking.
- A companion-file session lease is acquired before restore and remains held
  through controller and journal shutdown. The registry prevents duplicate
  opens inside `chaweb`; the lease prevents them across processes and frontends.
- `Workspace` remains immutable and safe for concurrent calls. Session-layer,
  agent, and transcript code may not introduce process-global mutable state that
  makes independent controllers interfere.
- Web creation and opening are separate operations. Creation returns a stored
  session identity and starts no runtime. Only the open route acquires a lease or
  constructs a controller.
- A startup result has one writer: the owner thread. At its commit point it reads
  the registry stopping flag and publishes either `running`/`ready` or
  `shutting_down`, never both.
- HTTP mutations are serialized through a bounded per-session command queue. A
  `command_timeout` has unknown outcome and does not cancel the command or shut
  down the session. Queue-full, session-stopping, and process-stopping failures
  prove the command did not execute.
- Every accepted SSE connection begins with a full snapshot. There is no replay
  log, SSE `id:` field, `Last-Event-ID`, or presentation revision.
- `snapshot` and target-aware `append` are the only state-bearing SSE events.
  Appends use a per-target sequence counter, not a byte or string offset.
- The SSE writer has at most one immutable in-flight payload and one replaceable
  pending payload. The owner thread never waits for network output.
- One active SSE stream is supported per live session. This is a lightweight
  usage guard, not authentication or a strict tab-identity protocol.
- Browser absence is governed only by `disconnected_since`, `is_generating()`,
  `idle_grace`, and `orphan_limit`. There is no explicit close route or separate
  browser-lifetime state machine.
- A thrown fatal session error tears down that session only. Undefined behavior,
  abort/terminate paths, and unrecoverable allocation failure remain
  process-fatal.
- Process shutdown is bounded. It prevents new opens, wakes startup waiters,
  requests owner-thread shutdown, joins within one grace period, and exits
  immediately without destructors if a thread remains stuck.
- No permissive CORS behavior or authentication is added. Mutations still
  validate content type and same-origin browser requests.
- Error responses use the common envelope, stable machine codes, and
  presentation-safe messages. Paths, exceptions, credentials, prompts, and
  answers are neither exposed nor logged by default.
- Tests use fake controllers, clocks, notifiers, providers, and socket writers
  where appropriate. They do not call a real LLM provider or depend on long
  wall-clock sleeps.
- Real-server tests use non-generating commands such as `/info`, `/agents`, an
  empty prompt, or an invalid slash command. A test that needs generation must
  install a deterministic fake provider through a test-only workspace/config
  seam.
- Unit tests stay in the fast ordinary suite. Real-server and longer concurrency
  tests use `web_process` and `web_stress` labels or equivalent non-default
  targets.

## 3. Block overview and dependencies

| Block | Result | Depends on |
| --- | --- | --- |
| 1 | Cross-process session lease used by every frontend | Existing session layer |
| 2 | Proven concurrent-controller and shared-workspace invariants | Block 1 |
| 3 | Create-only stored-session operation | Blocks 1–2 |
| 4 | `cha_web` library, protocol types, settings, and single-mode server skeleton | Existing build; Block 3 for integration |
| 5 | Per-session owner runtime, notifier, and bounded command queue | Blocks 1, 2, 4 |
| 6 | Controller integration, snapshots, containment, and idempotent session shutdown | Block 5 |
| 7 | Session registry, open protocol, handles, sweeping, and capacity bound | Blocks 5–6 |
| 8 | Lobby routes, create/open flow, listing, and same-origin paths | Blocks 3, 4, 7 |
| 9 | Path-scoped session command and snapshot API | Blocks 6–8 |
| 10 | SSE framing, sequencing, mailbox, heartbeat, and write bounds | Blocks 6, 9 |
| 11 | Single-stream guard and disconnect-driven session lifetime | Blocks 7, 10 |
| 12 | Complete server composition and bounded process shutdown | Blocks 7–11 |
| 13 | Resource, network, logging, race, and conformance hardening | Blocks 1–12 |
| 14 | Platform, sanitizer, documentation, and final design audit | Block 13 |

The recommended order is numerical. Blocks 1–3 establish domain invariants;
Block 4 establishes the web boundary; Blocks 5–7 build session ownership;
Blocks 8–11 expose it over HTTP/SSE; Blocks 12–14 integrate and harden the
complete server.

## 4. Block 1 — Cross-process session lease

### Objective

Add the companion-file operating-system lock and make every controller-opening
path use it. At completion, `cha`, `chacon`, and future `chaweb` runtimes fail
immediately and clearly when another process owns the selected session.

### Read first

- `web-design.md` Sections 7.1–7.3, 16, and 20.1.
- `src/session/workspace.*`, `session_catalog.*`, `session_controller.*`, and
  `session_database.*`.
- TUI and console startup paths and their tests.

### Required work

1. Add a move-only `SessionLease` in `src/session/`.
   - Derive the companion path by appending `.cha-lock` to the complete database
     filename.
   - Open or create the file without treating existence as state.
   - Acquire an exclusive whole-file lock without waiting.
   - Keep the native descriptor or handle open for the object lifetime.
   - Release the lock and native resource through RAII.
   - Keep POSIX and Win32 details behind one portable interface.
   - Distinguish contention from permission, I/O, and malformed-path failures.
2. Add `SessionBusyError` or an equivalent typed result so later web code can map
   contention without parsing English text.
3. Make `Workspace::open_session()` validate and resolve the session, acquire the
   lease, restore state, load definitions, and construct the controller in that
   order.
4. Preserve the terminal create-and-open operation, but acquire the new
   session's lease before constructing its controller.
5. Move the lease into `SessionController`. Declaration/destruction order must
   make it outlive `SessionJournal` and explicit controller shutdown.
6. Permit test-only controllers to use an explicitly inactive lease rather than
   locking unrelated files.
7. Update the relevant session and architecture ownership documentation.

### Likely files

- `src/session/session_lease.h`
- `src/session/session_lease.cpp` and optional platform backends
- `src/session/workspace.*`
- `src/session/session_controller.*`
- `tests/session/unit_session_lease.cpp`
- `tests/session/unit_workspace.cpp`
- a small multi-process lease helper/test
- `CMakeLists.txt`

### Tests and validation

- Acquire an unused lease and verify its companion filename.
- Reject a second process while the first holds the lease.
- Acquire after orderly release and after forced owner-process termination.
- Treat an existing but unlocked companion file as available.
- Do not misreport lease I/O failures as contention.
- Prove restore is not attempted after contention.
- Hold several independent session leases in one process and release one without
  affecting the others.
- Verify TUI and console opening paths report contention immediately and clearly.

### Completion criteria

- No production path constructs a live controller without first acquiring its
  companion lease.
- The lease survives through controller and journal shutdown.
- Genuine inter-process contention is covered by a process test.
- Existing tests continue to pass.

### Not in this block

- In-process registry exclusivity.
- Create-only behavior.
- HTTP error mapping.

## 5. Block 2 — Concurrent-controller and workspace invariants

### Objective

Prove that several independent controllers may run concurrently inside one
process and document the invariants on process-global state and shared
`Workspace` access before `chaweb` depends on them.

### Read first

- `web-design.md` Sections 9.7–9.8 and 20.4.
- `src/README.md` and ownership documentation under `src/session/`, `src/agents/`,
  and `src/transcript/`.
- Completion-client initialization, session catalog naming, logging, SQLite
  connection setup, and signal handling.

### Required work

1. State in `src/README.md` that N controllers on N owner threads are supported
   when no domain object is shared between them.
2. Audit process-global mutable state:
   - Keep libcurl initialization thread-safe and one-time.
   - Keep each completion backend/easy handle session-local.
   - Preserve serialization around non-thread-safe time conversion.
   - Keep each SQLite connection and statement on one owner thread.
   - Confirm the shared logger is thread-safe.
   - Keep signal state outside session/controller objects.
3. Confirm that `Workspace` is immutable after construction, every public
   operation used by the web server is `const`, and calls do not share lazy
   mutable caches.
4. Preserve session creation's atomic publish-or-retry behavior. Do not replace
   it with a check-then-create sequence.
5. Add focused concurrency seams and tests using distinct session databases and
   deterministic fake providers.
6. If the audit finds mutable global or shared state, remove it or make it
   construction-time immutable rather than adding a global web mutex.

### Likely files

- `src/README.md`
- affected files under `src/session/` and `src/agents/`
- `tests/session/unit_concurrent_controllers.cpp`
- `tests/session/unit_workspace.cpp`
- `tests/support/` concurrency/provider fixtures as needed

### Tests and validation

- Construct two controllers concurrently and initialize their completion clients
  without a race.
- Run independent deterministic generations and verify transcripts, journals,
  request IDs, and shutdown do not cross.
- Call workspace listing/open operations concurrently on one `Workspace`.
- Create several sessions in one forum with the clock fixed to one timestamp
  second and verify every database is unique and none is lost.
- List concurrently with creation and observe either a complete database or no
  database, never a partial one.
- Run the focused tests under ThreadSanitizer when available.

### Completion criteria

- The supported concurrent-controller invariant is explicit and tested.
- `Workspace` can be shared read-only by lobby and owner threads.
- No process-global mutable domain state serializes otherwise independent
  sessions.

### Not in this block

- Web owner threads or registry code.
- General caching of workspace/forum metadata.

## 6. Block 3 — Create a stored session without opening it

### Objective

Give the lobby a session-layer operation that atomically creates and returns a
stored session without acquiring a live-session lease, initializing providers,
or constructing a controller.

### Read first

- `web-design.md` Sections 7.5, 9.8, 10, 16.2, and the creation cases in
  Sections 20.4–20.5.
- The Block 1 lease path and Block 2 workspace invariants.
- `Workspace`, `SessionCatalog`, forum validation, and workspace tests.

### Required work

1. Add a clearly named create-only operation returning `SessionSummary`.
2. The operation must:
   - Validate and load the forum metadata required for creation.
   - Create through `SessionCatalog` using atomic no-overwrite publication and
     collision retry.
   - Return the created ID and effective label.
   - Avoid provider/client initialization, `WakeNotifier`, `AgentRegistry`,
     `ThreadPool`, `SessionController`, and session lease acquisition.
3. Refactor the existing terminal create-and-open operation to call create-only
   and then the ordinary leased open path without changing frontend behavior.
4. Do not add lease handoff, rollback, or a composite web create-and-open result.
5. Document the distinction between create-only and create-and-open.

### Likely files

- `src/session/workspace.*`
- `src/session/session_catalog.*` if a small seam is needed
- `tests/session/unit_workspace.cpp`
- `src/session/README.md`

### Tests and validation

- Return the stored ID and effective label and list it exactly once.
- Preserve generated-label behavior for empty labels.
- Fail invalid forums/definitions without publishing a database.
- Do not initialize a provider during create-only.
- Open the returned session normally in a separate step.
- If another process leases it before open, report ordinary busy while retaining
  the stored session.
- Concurrent same-second creates produce distinct complete databases.
- Existing terminal create-and-open tests still pass.

### Completion criteria

- Creation has an all-or-nothing stored-session result and never starts a live
  lifecycle.
- The caller always receives the identity of a successfully published session.
- Existing terminal behavior remains intact.

### Not in this block

- HTTP routes.
- Automatic opening after web creation.

## 7. Block 4 — Web library, protocol, and server skeleton

### Objective

Create the reusable `cha_web` library, owning protocol types, injectable
settings, and a single-mode `chaweb` composition skeleton with one HTTP listener.

### Read first

- `web-design.md` Sections 4–6, 10–13, 16, 18, and 22.
- `src/ui/README.md`, JSON conventions, CMake targets, and current app entry
  points.

### Required work

1. Create `src/ui/web/` with a short boundary/ownership README.
2. Add a `cha_web` static library linked to `cha_core` and cpp-httplib, with C++20
   and repository warning settings.
3. Link `chaweb_app` to `cha_web`; keep `web_main.cpp` a small single-invocation
   skeleton. Add no private mode flags or child-process arguments.
4. Add a focused `cha_web_tests` target without making core unit tests depend on
   cpp-httplib.
5. Define owning protocol types for:
   - Forum, session, and persona summaries.
   - Transcript entries and generation state.
   - Complete snapshots, current notice, and lifecycle/shutdown reason.
   - Raw/typed web commands and command results.
   - Open/create success bodies and the common error envelope.
   - `snapshot` and target-aware `append` payloads.
6. Snapshots must contain no host, port, absolute URL, or lobby address. The
   return path is `/`.
7. Define exact enum/code JSON spellings. An error object contains exactly
   string `code` and `message`; successful bodies contain no `error` field.
   Include the transport codes of Section 16.1 alongside the lifecycle ones:
   `not_found`, `bad_request`, `body_too_large` with `413 Content Too Large`,
   `forbidden_origin`, and `internal_error`.
8. Add reusable JSON/content-type/response helpers, but no production routes.
9. Define injectable settings for the session limit, HTTP pool/pending bounds,
   command-queue capacity, open/command deadlines, SSE heartbeat/write/drain
   deadlines, disconnect limits, reload retry, request/prompt limits, and the
   process shutdown grace period. Every one of them must be settable by a test;
   Blocks 11–13 depend on shortening deadlines rather than sleeping through
   production values.
10. Add a shared temporary test-workspace builder with deterministic fake-provider
    configuration support.

### Likely files

- `src/ui/web/README.md`
- `src/ui/web/protocol.*`
- `src/ui/web/json.*`
- `src/ui/web/http_response.*`
- `src/ui/web/web_settings.h`
- `src/apps/web_main.cpp`
- `tests/ui/web/unit_protocol.cpp`
- `tests/support/test_workspace.*`
- `CMakeLists.txt`

### Tests and validation

- Serialize exact creation, open, command-result, snapshot, append, and error
  fixtures.
- Omit an absent command-result notice and preserve an explicitly empty notice.
- Escape arbitrary transcript/notice/provider strings as data.
- Prove protocol objects outlive mutation/destruction of their source state.
- Reject invalid enum/input values rather than selecting defaults.
- Verify snapshots and open responses contain no host, port, or absolute URL.
- Build the library and application with TUI disabled.

### Completion criteria

- Later blocks can use one owning web protocol without adding web types to
  `cha_core`.
- `chaweb` has one mode and one listener-shaped composition root.
- Protocol and settings seams are exact and unit tested.

### Not in this block

- Live runtimes, registry, or production routes.
- Browser framework or generated client.

## 8. Block 5 — Owner runtime, notifier, and command queue

### Objective

Implement the thread-confinement core of `WebSessionRuntime`: one permanent owner
thread, a condition-variable wake notifier, a bounded command queue, owning
completions, and fair command/agent-event scheduling.

### Read first

- `web-design.md` Sections 9.1–9.4, 9.6, 11.1–11.3, and 20.3.
- Block 4 protocol/settings types.
- `SessionController`, `SessionUpdate`, `WakeNotifier`, and terminal owner-loop
  patterns.

### Required work

1. Add `WebSessionRuntime` and a construction/factory seam suitable for real and
   fake controllers.
2. Give it one owner thread and assert/test owner-thread affinity for every
   controller-facing operation.
3. Implement a condition-variable notifier supporting cross-thread wake and
   timed wait without a per-session libuv loop or descriptor.
4. Implement a bounded multi-producer/one-consumer command queue carrying owning
   typed commands and shared owning completion objects.
5. Support raw input, typed Stop, and stable-ID default-agent commands. Keep
   Clear/off-record controls as exact raw grammar strings.
6. Apply `SessionUpdate.notice` to runtime-owned notice state before completing a
   command; never infer an `applied` result from existing update flags/text.
7. Map enqueue outcomes precisely:
   - Full queue: `command_queue_full`, nothing executed.
   - Runtime became stopping before enqueue: `session_not_live`, nothing
     executed.
   - Accepted command: wait within the request deadline.
   - Wait expiry: `command_timeout`, unknown outcome, no cancellation.
8. Keep late completion safe after the HTTP waiter releases its reference.
9. Drain controller notifications even with no browser and interleave bounded
   command/event batches so neither starves the other.
10. Define, but do not yet expose, owner-serialized SSE connect/disconnect
    notification types with internal connection IDs.

### Likely files

- `src/ui/web/web_session_runtime.*`
- `src/ui/web/web_command.*`
- `src/ui/web/wake_notifier.*`
- `src/ui/web/command_queue.*`
- `tests/ui/web/unit_web_session_runtime.cpp`
- `tests/ui/web/fake_session_controller.*`

### Tests and validation

- Every controller command and `receive()` call occurs on the owner thread.
- Raw grammar and typed actions reach the correct controller semantics.
- Completion objects remain valid after request timeout/disconnect.
- Full-queue and stopping rejection execute nothing and carry the required code.
- `command_timeout` leaves the command queued/running and the session alive.
- Sustained agent notifications do not starve commands; sustained commands do not
  prevent event draining/persistence.
- Two independent runtimes make progress concurrently without sharing queues or
  notifiers.

### Completion criteria

- No borrowed domain state crosses the owner-thread boundary.
- Command execution and completion semantics match the design.
- Owner-loop progress does not depend on an HTTP server or browser.

### Not in this block

- Full snapshot construction.
- Registry or routes.
- Network SSE writer.

## 9. Block 6 — Controller integration, snapshots, containment, and session shutdown

### Objective

Complete the runtime's real controller ownership, owning snapshot construction,
fatal-error containment, and one idempotent owner-thread shutdown sequence.

### Read first

- `web-design.md` Sections 9.3–9.5, 12, 14.4, 16, 19.2, and runtime/containment
  tests in Sections 20.3–20.4.
- Block 5 runtime and protocol types.
- `TranscriptView`, generation/request state, `SessionJournal`, and controller
  shutdown behavior.

### Required work

1. Integrate runtime construction with a real controller produced on the owner
   thread by the workspace open path.
2. Convert every borrowed session value into an owning, presentation-neutral
   snapshot on the owner thread.
3. Include forum/session metadata, personas, default agent, transcript entries,
   generation/request/phase/reasoning state, current notice, and lifecycle state.
4. Classify changes as structural versus safe text append. Build a full snapshot
   whenever a safe append cannot be proved.
5. Put the owner loop inside a top-level exception boundary. On a thrown fatal
   session error, log session identity and enter session-local shutdown with
   reason `session_failed`, without affecting other runtimes.
6. Implement one idempotent shutdown sequence:
   - Mark runtime/registry lifecycle stopping through an injected registry hook.
   - Reject new work.
   - Publish a final stopping snapshot when possible through an abstract sink,
     then wait up to the configured final-drain deadline for that sink to report
     the payload written. A successful write, a sink failure, or expiry ends the
     wait; a fatal session error or process shutdown may skip it. Final-state
     delivery is best effort and shutdown never waits longer than the deadline.
   - Complete an executing command normally; fail queued unstarted commands with
     `session_not_live` or `server_stopping` according to the trigger.
   - Call `SessionController::shutdown()`.
   - Destroy the controller and release its lease.
   - Signal finished through a registry hook.
7. Guard teardown substeps so one exception does not prevent controller
   destruction, lease release, or owner-thread return where recovery is possible.
8. Make the notifier outlive all agent workers and the lease outlive the journal.

### Likely files

- `src/ui/web/web_session_runtime.*`
- `src/ui/web/session_snapshot.*`
- `src/ui/web/protocol.*`
- `tests/ui/web/unit_web_session_runtime.cpp`
- `tests/ui/web/unit_session_snapshot.cpp`

### Tests and validation

- Snapshot values remain valid after transcript/controller mutation and teardown.
- Notice absent/clear/replace semantics survive reconnect snapshots.
- Structural changes produce snapshots; established text changes can produce
  append candidates.
- Agent events drain and persist with no presentation sink attached.
- A stalled sink lets the final-drain deadline expire and shutdown continues; a
  sink that reports the write ends the wait immediately; a fatal-error teardown
  skips the wait altogether.
- Persistence failure tears down only the failing runtime and releases only its
  lease.
- Fatal errors during open handoff, event processing, and shutdown converge on
  the same idempotent path.
- Concurrent shutdown triggers execute teardown once.
- A deliberately blocked owner is not killed or falsely marked failed.

### Completion criteria

- The controller is constructed, used, and destroyed on one owner thread.
- A thrown fatal error is session-local.
- Shutdown leaves no controller, journal, agent worker, or lease alive.

### Not in this block

- Registry map implementation.
- Actual SSE socket writes or disconnect deadlines.

## 10. Block 7 — Session registry and open lifecycle

### Objective

Implement the process-wide registry, serialized open protocol, session handles,
capacity accounting, two-phase finished-entry sweep, and shutdown coordination.

### Read first

- `web-design.md` Section 8, Sections 9.2 and 19, and registry/server lifecycle
  tests in Sections 20.2 and 20.6.
- Blocks 5–6 runtime lifecycle hooks.
- `Workspace::open_session()` and typed busy/error results.

### Required work

1. Add a registry map keyed by validated forum/session identity. Each entry owns
   lifecycle state, runtime reference when published, owner `std::thread`, shared
   startup result, and mutex-protected `finished` flag.
2. Add one registry-wide stopping flag, written/read only under the registry
   mutex.
3. Count every map entry toward the configured limit. Check capacity and insert
   `starting` under one mutex acquisition.
4. Implement open behavior:
   - `running`: return its existing stable path.
   - `starting`: attach to the same startup result.
   - `stopping`: return `session_stopping`.
   - Absent at limit: return `session_limit_reached`.
   - Absent with capacity: insert and start exactly one owner thread.
5. Give each waiter its own full open deadline. Timeout fails only that request;
   it neither removes the entry nor cancels the owner.
6. Make the owner thread the startup result's only writer. At the one commit
   point under the registry mutex:
   - If not stopping, publish runtime, set `running`, resolve `ready`.
   - If globally stopping, publish nothing, resolve `shutting_down`, and tear the
     new controller down.
7. Map startup results to `ready`, `session_busy`, `internal_error`,
   `server_stopping`, and `session_open_timeout` without parsing messages.
8. Return owning session handles only for `running` entries. The owner thread
   must never own a handle to its own runtime.
9. Let the owner transition itself to `stopping` and set `finished` under the
   registry mutex as its final action.
10. Sweep at the head of every registry operation — open, lookup, and shutdown —
    in two phases: move finished entries out under the mutex, then join threads
    and release references after unlocking. Phase 1 must share its mutex
    acquisition with the capacity check of item 3, so an entry swept by an open
    frees its slot for that same open.
11. Expose process-shutdown operations that set the global flag, wake startup
    waiters, request all running runtimes to stop, and enumerate unfinished
    owners without completing startup results from the shutdown thread.

### Likely files

- `src/ui/web/session_registry.*`
- `src/ui/web/session_handle.*`
- `src/ui/web/startup_result.*`
- `tests/ui/web/unit_session_registry.cpp`

### Tests and validation

- Cover every lifecycle branch and exact error code.
- Concurrent same-key opens start one owner and share one result.
- Concurrent different-key opens proceed independently up to the bound.
- `starting` and `stopping` entries consume capacity.
- An open that sweeps a finished entry admits in that same operation instead of
  reporting `session_limit_reached` against a slot nothing holds.
- Simultaneous distinct opens at the limit never exceed the owner-thread bound.
- Each waiter receives its own open deadline; one timeout does not affect others.
- A timed-out open may later be found `running`.
- A handle keeps a runtime alive after map removal and observes only stopping.
- Sweep joins once, outside the mutex, and permits a same-key reopen after lease
  release even while the old thread is being joined.
- Shutdown racing every open stage yields exactly one owner-written startup
  result and never publishes `running` after the stopping flag.

### Completion criteria

- The registry is the sole in-process liveness authority.
- Duplicate controllers and over-capacity owner threads are impossible.
- No blocking work or runtime destruction occurs under the registry mutex.

### Not in this block

- HTTP routes.
- Network listener/process shutdown mechanics.

## 11. Block 8 — Lobby routes and explicit create/open flow

### Objective

Expose the lobby page/asset boundary, health, forum/session listing, create-only,
and open/reattach routes on the one configured listener.

### Read first

- `web-design.md` Sections 6, 10, 15.2–15.4, 16, and lobby contract tests in
  Section 20.5.
- Blocks 3–4 protocol/create support and Block 7 registry.
- cpp-httplib routing, body-limit, and thread-pool extension points.

### Required work

1. Add `LobbyRoutes` using the shared immutable `Workspace` and registry.
2. Register:
   - `GET /`
   - `GET /health`
   - `GET /api/v1/forums`
   - `GET /api/v1/forums/{forum}/sessions`
   - `POST /api/v1/forums/{forum}/sessions`
   - `POST /api/v1/forums/{forum}/sessions/{session}/open`
3. Validate route identifiers through application/domain validation before any
   registry or filesystem use.
4. Make create return `201 Created` carrying only the created session's `id` and
   `label`, per Section 16.2. Serialize those two fields explicitly rather than
   emitting `SessionSummary` wholesale, whose `error` field does not belong in
   this body. It must not call the registry or return open-lifecycle errors.
5. Make open call only the registry and return a relative session path after
   `ready`. Reattach to `running` returns the same path.
6. Mark sessions live in listings from a registry snapshot, while treating that
   marker as advisory and the open route as authoritative.
7. Add the HTML/static-asset boundary and a minimal lobby placeholder without
   choosing the eventual browser stack.
8. Return `/health` process readiness and bounded live-entry count without
   exposing forum/session names or transcript data.
9. Use the common success/error serializers and exact statuses/codes.

### Likely files

- `src/ui/web/lobby_routes.*`
- `src/ui/web/asset_handler.*`
- `src/ui/web/http_server.*`
- `tests/ui/web/unit_lobby_routes.cpp`
- `tests/ui/web/http_test_client.*`

### Tests and validation

- Route/method/content-type/identifier validation.
- Create returns identity, opens nothing, and leaves no registry entry.
- Create never returns `session_busy`, `session_limit_reached`, or
  `session_open_timeout`.
- Create followed by each possible open failure retains the created session and
  supports retrying open without repeating create.
- Open returns a relative path with no host, port, or absolute URL.
- Opening a running session does not construct/acquire again.
- Listings mark live sessions but tolerate state changing before open.
- Every JSON error has exactly the common envelope.
- `/health` exposes no session identity or content.

### Completion criteria

- The lobby starts no controller except through the registry's open operation.
- Creation and opening are observably separate and recoverable.
- All navigation output is same-origin path data.

### Not in this block

- Session command/SSE routes.
- Final lobby browser implementation.

## 12. Block 9 — Path-scoped session HTTP API

### Objective

Expose the chat-page boundary, snapshot route, raw input, typed Stop, and typed
default-agent action under `/s/{forum}/{session}/`, with all work resolved through
owning registry handles.

### Read first

- `web-design.md` Sections 8.3, 11, 12, 16, and relevant Section 20.5 tests.
- Blocks 5–8 runtime, protocol, and route helpers.

### Required work

1. Add `SessionRoutes` for:
   - `GET /s/{forum}/{session}/`
   - `GET /s/{forum}/{session}/api/v1/session`
   - `POST /s/{forum}/{session}/api/v1/input`
   - `POST /s/{forum}/{session}/api/v1/actions/stop`
   - `POST /s/{forum}/{session}/api/v1/actions/default-agent`
2. Reserve the events route shape for Block 10 without opening a stream yet.
3. Validate identifiers, resolve a running session handle once per request, and
   retain it until the handler completes.
4. Serve a not-open HTML page with a `/` link for a non-live page request;
   return `session_not_live` for non-live JSON/API requests.
5. Bound and parse request bodies before enqueue. Enforce expected JSON content
   types on every mutation.
6. Map runtime outcomes exactly:
   - Successful command result with `clear_input` and optional notice.
   - `command_queue_full` for safe-to-retry non-admission.
   - `session_not_live` for stopping-before-execution.
   - `server_stopping` for shutdown-drained commands.
   - `command_timeout` for unknown outcome, without runtime shutdown.
7. Never update snapshot state from an HTTP result. Keep any returned notice
   request-scoped.
8. Add a minimal chat-page/static-asset boundary without selecting browser
   technology.
9. Do not add a per-session health/status or close route.

### Likely files

- `src/ui/web/session_routes.*`
- `src/ui/web/http_server.*`
- `src/ui/web/asset_handler.*`
- `tests/ui/web/unit_session_routes.cpp`

### Tests and validation

- Exact path/method/validation behavior, including encoded traversal rejection.
- Non-live page versus non-live API behavior.
- Raw grammar commands and typed controls preserve their distinct editor
  semantics.
- Successful results carry no `applied` inference and omit an absent notice.
- Queue-full/stopping/server-shutdown failures execute nothing.
- `command_timeout` does not remove the command or stop the session.
- Client disconnect after enqueue may still complete safely.
- `/api/v1/close` and per-session health/status routes do not exist.

### Completion criteria

- Every session request reaches domain state only through a running handle and
  the owner queue.
- HTTP response ordering cannot overwrite authoritative snapshot state.
- The path-scoped non-SSE API matches the design.

### Not in this block

- SSE body streaming.
- Active-browser guard and disconnect lifetime.

## 13. Block 10 — SSE protocol and latest-state mailbox

### Objective

Implement the session events route, exact SSE framing, full-snapshot reconnect,
target/sequence-aware appends, a one-pending latest-state mailbox, heartbeats,
and bounded network writes.

### Read first

- `web-design.md` Section 13, snapshot cost discussion in Section 12, and SSE
  contract tests in Section 20.5.
- Blocks 6 and 9 snapshot/runtime/route integration.
- cpp-httplib streaming response and socket timeout behavior.

### Required work

1. Implement `GET /s/{forum}/{session}/api/v1/events` using a streaming response
   suitable for consumption by `fetch`, not assumptions specific to
   `EventSource`.
2. Emit only blank-line-terminated records containing:
   - `event: snapshot` or `event: append` plus one single-line JSON `data:`.
   - `:` comment heartbeats with no state.
   - No `id:`, `retry:`, or other fields.
3. Send a fresh full snapshot as the first payload of every accepted stream.
4. Implement append targets for answer `entry_id` and reasoning `request_id`.
5. Maintain a per-target `seq` counter:
   - Snapshot establishes expected sequence 0.
   - A newly pending append consumes one sequence value.
   - Merging compatible pending appends consumes no additional value.
6. If an update is structural, changes target, or cannot maintain continuity,
   replace the pending append with a current snapshot.
7. Implement one immutable in-flight payload plus at most one replaceable pending
   payload per runtime. Never mutate in-flight data or block the owner on the
   writer.
8. Retain no presentation backlog with no connected stream.
9. Add periodic comment heartbeats and a bounded lack-of-write-progress timeout.
   A slow reader making progress must remain connected even during a large
   snapshot.
10. Treat write failure/timeout as ordinary stream closure. Do not stop generation
    or controller draining.
11. Implement the sink the Block 6 shutdown sequence drains against: report when
    a published payload has been written, so a final lifecycle snapshot can be
    awaited up to the final-drain deadline and no longer.

### Likely files

- `src/ui/web/sse_stream.*`
- `src/ui/web/sse_mailbox.*`
- `src/ui/web/session_routes.*`
- `src/ui/web/web_session_runtime.*`
- `tests/ui/web/unit_sse_mailbox.cpp`
- `tests/ui/web/unit_sse_stream.cpp`

### Tests and validation

- Exact framing, event names, one-line JSON, comments, and absence of `id:`.
- Every connection starts with a full current snapshot and ignores
  `Last-Event-ID`.
- Answer/reasoning appends start at sequence 0 and increment as delivered.
- Compatible appends merge byte-for-byte, including multibyte text, while
  consuming one sequence number.
- Structural/incompatible/discontinuous changes collapse to a snapshot.
- The writer never owns more than one in-flight and one pending payload.
- Slow/disconnected output never blocks command or agent-event processing.
- A non-reading peer releases its request thread within the write timeout; a
  slowly progressing peer does not.
- A final lifecycle snapshot written to a reading peer completes the shutdown
  drain wait; one written to a peer that has stopped reading expires it, and
  shutdown proceeds either way.

### Completion criteria

- The stream is recoverable solely by reconnecting for a snapshot.
- Presentation buffering is bounded in count and session-local.
- Network I/O never runs on the owner thread.

### Not in this block

- Rejecting a second active stream.
- Disconnect unload deadlines or browser retry implementation.

## 14. Block 11 — Browser-stream guard and disconnect lifetime

### Objective

Add the one-active-stream policy, connection-ID-safe close handling, bounded
reload conflict behavior at the contract level, and the single disconnect
deadline that unloads idle/orphaned sessions.

### Read first

- `web-design.md` Sections 11.4, 14, 19.2, and Sections 20.3, 20.6, and 20.7.
- Blocks 7 and 10 registry/runtime/SSE behavior.

### Required work

1. Serialize SSE accept/close notifications through the owner loop.
2. Accept the first stream and reject another active stream with `409` and
   `browser_stream_in_use` before starting an SSE body.
3. Assign every accepted stream an internal session-local connection ID. Only a
   matching close clears the active slot; rejected, duplicate, and stale closes
   change nothing.
4. On runtime publication, set `disconnected_since` and arm `idle_grace` for
   initial browser arrival.
5. On successful stream acceptance, clear the timestamp and cancel the deadline.
   On matching close, record the current time and rearm.
6. While disconnected, compute the only deadline as:

   ```text
   is_generating() ? orphan_limit : idle_grace
   ```

   Both are measured from the same `disconnected_since`; `orphan_limit` is not an
   additional period and must be at least `idle_grace`.
7. Rearm whenever stream presence, `disconnected_since`, or generation state
   changes. If generation ends after idle grace has elapsed, begin shutdown
   promptly.
8. Continue receive/persistence during disconnection until the absolute deadline.
9. On expiry, request the ordinary owner-thread shutdown sequence; do not add an
   explicit close route, browser unload beacon, or takeover protocol.
10. Specify framework-independent page behavior for later implementation:
    controls enable only after accepted stream plus initial snapshot; a conflict
    retries briefly and then displays the already-open message; target/sequence
    mismatch closes and reconnects rather than racing a REST snapshot.

### Likely files

- `src/ui/web/web_session_runtime.*`
- `src/ui/web/browser_connection_state.*`
- `src/ui/web/session_routes.*`
- `tests/ui/web/unit_browser_connection_state.cpp`
- `tests/ui/web/unit_session_lifetime.cpp`

### Tests and validation

- First stream accepted; second same-session stream rejected; different-session
  streams accepted concurrently.
- Rejected streams do not change active ID or disconnect deadline.
- Stale/duplicate close cannot detach a newer stream.
- Opened-but-never-visited sessions unload after `idle_grace`.
- Idle disconnected sessions and generating disconnected sessions unload at the
  correct absolute limits from `disconnected_since`.
- Generation completion reevaluates the deadline immediately.
- Reconnect before shutdown cancels the deadline and starts with a fresh
  snapshot.
- No close endpoint, page-lifecycle signal, browser ID, or takeover mechanism is
  introduced.

### Completion criteria

- Each live session supports exactly one active interactive stream without
  affecting other sessions.
- Browser absence releases runtime resources and leases under the one documented
  rule.
- Reload/reconnect recovery requires no event replay or browser identity.

### Not in this block

- Actual JavaScript retry UI.
- Process-wide shutdown orchestration.

## 15. Block 12 — Complete server composition and bounded process shutdown

### Objective

Wire the workspace, registry, routes, HTTP pool, signal notification, session
runtimes, and one configured listener into the complete `chaweb` server, then
implement bounded orderly process shutdown.

### Read first

- `web-design.md` Sections 5–6, 8.6, 14.5, 18–19, and Section 20.6.
- Blocks 7–11.
- Existing application configuration, logging, signal, and cpp-httplib server
  setup.

### Required work

1. Keep `web_main.cpp` limited to configuration/logging, `Workspace`, registry,
   routes/server, signal notification, and top-level error handling.
2. Run one invocation with one listener on `ApplicationConfig.host` and
   `ApplicationConfig.port`. Remove/avoid internal mode flags and all
   session-specific listener behavior.
3. Size the cpp-httplib request pool and pending-request bound together with the
   maximum registry-entry count, leaving documented headroom beyond maximum SSE
   occupancy.
4. Install process-level signal handling once. Use a safe notification path into
   normal shutdown code; do not perform complex work in a signal handler.
5. Implement shutdown order:
   - Set the registry stopping flag and reject new opens.
   - Stop accepting new requests and wake startup waiters.
   - Request every running runtime's idempotent shutdown.
   - Let opening owners reach their single commit point and resolve
     `shutting_down` themselves.
   - Drain executing commands; fail queued unstarted commands with
     `server_stopping`.
   - Join all owner threads under one process-wide grace deadline.
   - On expiry, log unfinished session identities and terminate without running
     destructors.
   - Otherwise close/join the HTTP server and destroy registry, workspace, and
     logging resources.
6. Ensure clean and forced process exit both release all companion locks through
   ordinary RAII or operating-system process cleanup.
7. Add real-server fixtures using temporary workspaces and no real provider.

### Likely files

- `src/apps/web_main.cpp`
- `src/ui/web/http_server.*`
- `src/ui/web/session_registry.*`
- `src/ui/web/server_shutdown.*`
- `tests/ui/web/process_web_server.cpp`
- `tests/support/web_server_process.*`
- `CMakeLists.txt`

### Tests and validation

- Start one real server, use lobby/create/open/session routes, and verify every
  URL stays on the configured origin.
- Run two live sessions concurrently through one listener.
- Clean shutdown stops sessions, joins owners/request threads, and releases
  leases.
- Shutdown racing an open never publishes `running` after the stopping flag and
  returns `server_stopping` to waiters promptly, even for a wedged open.
- Only the owner thread completes each startup result exactly once.
- Shutdown during generation invokes ordinary controller cancellation/join.
- A deliberately blocked owner causes process exit within the grace period and
  is logged by identity.
- Restart opens sessions previously leased by the terminated server.

### Completion criteria

- `chaweb` is a usable one-process, one-listener server with several independent
  live sessions.
- Normal shutdown is orderly and stuck-thread shutdown is bounded.
- No child process, per-session port, or control-channel implementation exists.

### Not in this block

- Internet-facing authentication/TLS.
- Production browser application.

## 16. Block 13 — Resource, network, logging, race, and conformance hardening

### Objective

Exercise the complete server under contention and enforce the resource, trust,
observability, error, and isolation guarantees that are easy to miss in isolated
feature blocks.

### Read first

- `web-design.md` Sections 8.6, 9.7–9.8, 15–17, and all of Section 20.
- Blocks 1–12 and their tests.
- cpp-httplib limit/timeout facilities and project logging conventions.

### Required work

1. Enforce request-body, prompt, header, connection, pending-request, queue, and
   timeout limits before expensive parsing or enqueue.
2. Require expected content types on mutations and reject a present mismatched
   `Origin`/`Host` with `403 forbidden_origin`. Emit no permissive CORS headers
   and add no Host allowlist.
3. Treat transcript, reasoning, notices, labels, and provider messages as
   untrusted serialized text.
4. Verify pool sizing under the maximum number of connected SSE streams; normal
   requests must continue making progress.
5. Add server-scoped logging for startup, the bound address, and the configured
   limits, and session-tagged logging for open outcomes, registry transitions,
   lease lifecycle, reattach, SSE connect/disconnect/conflict/collapse counts,
   generation terminal state, deadlines, fatal containment, limit rejection,
   shutdown, and stuck owners.
6. Exclude prompt/answer bodies and secrets from default logs. Distinguish
   server-scoped records from session-scoped ones.
7. Audit all JSON routes against the one error envelope and exact stable codes.
8. Add adversarial races:
   - Same-key and different-key opens at/around the limit.
   - Open versus unload/reopen/sweep.
   - Command enqueue versus session/process stopping.
   - SSE replace versus stale close.
   - Disconnect deadline versus reconnect/generation transition.
   - Fatal error in one session while another generates.
9. Add load tests for multiple sessions, independent queues/mailboxes/leases, and
   concurrent create/list/open/command/unload.
10. Keep long tests behind `web_stress`; keep deterministic focused versions in
    the ordinary suite.

### Likely files

- route/server/runtime/registry files from Blocks 5–12
- `tests/ui/web/process_web_server.cpp`
- `tests/ui/web/stress_web_sessions.cpp`
- logging/test-support helpers
- `CMakeLists.txt`

### Tests and validation

- Every JSON failure has status >=400 and exactly `error.code`/`error.message`.
- Invalid and nonexistent identifiers share `404 not_found` and cannot traverse
  through raw or percent-encoded paths.
- Cross-origin mutation checks behave exactly as designed for loopback, LAN, and
  mDNS authorities.
- Maximum connected streams do not starve health, lobby, snapshot, or command
  requests.
- A stalled SSE reader releases its HTTP thread within the write timeout.
- Fatal persistence failure unloads one session and leaves others usable and
  reopenable.
- Artificial owner blocking causes timeouts only for that session until released
  or the process restarts.
- Concurrent logging carries correct identities and no prompt/answer content.
- Repeated open/unload cycles leave no leaked owner, agent, HTTP, descriptor,
  handle, lease, or registry entry.

### Completion criteria

- Resource use is bounded by documented settings and one payload-size caveat,
  not by request arrival rate.
- Cross-session isolation holds under race and load tests.
- Network/error/logging behavior matches the trusted-LAN design exactly.

### Not in this block

- Authentication, TLS, DNS-rebinding defense, quotas, or per-client rate limits.
- Whole-transcript paging/windowing.

## 17. Block 14 — Platform, sanitizer, documentation, and final audit

### Objective

Verify portable lease/server behavior, run available sanitizers and platform
builds, reconcile documentation with implementation, and close every remaining
design-conformance gap without expanding scope.

### Read first

- `web-design.md` Sections 18–23 and the complete testing strategy.
- This complete plan and all changes from Blocks 1–13.
- `src/README.md`, component READMEs, build documentation, and CI definitions.

### Required work

1. Run the complete ordinary, `web_process`, and `web_stress` suites on Linux.
2. Run AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer builds where
   supported, emphasizing concurrent open, command, generation, SSE, unload, and
   shutdown.
3. Build and test the native companion-file lease backend on macOS and Windows
   runners where available.
4. Verify `chaweb` has one invocation, one listener, no worker-process mode, and
   no platform-specific console/process-launch code left from the rejected
   architecture.
5. Audit ownership and destruction order for runtime, notifier, controller,
   journal, lease, mailbox, session handle, registry entry, and HTTP pool.
6. Audit every design test bullet against an automated test or an explicitly
   documented manual/platform verification.
7. Update `src/README.md`, relevant component READMEs, user/build documentation,
   and CMake/CI labels to describe the implemented composition and tests.
8. Check that configuration exposes only values operators genuinely need; keep
   the rest as documented implementation constants. Ensure session limit and
   request-pool sizing cannot drift into unsafe independent defaults.
9. Remove obsolete process-per-session terminology, launch/control artifacts,
   per-session port settings, and stale error codes from source, tests, and the
   web documentation set.
10. Record any platform or sanitizer not exercised; do not claim unrun coverage.

### Likely files

- code/tests from prior blocks as findings require
- `src/README.md` and component READMEs
- `docs/web-design.md`, `docs/brief-web-design.md`, and this plan
- `CMakeLists.txt`, presets, and CI workflows

### Tests and validation

- Full Linux configure/build/test plus labeled suites.
- Available macOS and Windows builds/tests.
- Available ASan/UBSan/TSan runs.
- Search for obsolete process/port/control terminology and unsupported routes.
- Verify TUI and console behavior remains unchanged except for explicit session
  lease contention.
- Verify documentation route tables, codes, lifecycle states, and ownership
  statements match production code and tests.

### Completion criteria

- All automated tests pass on available platforms and sanitizers.
- No known ownership race, leak, cross-session state, or design contradiction
  remains.
- The repository documentation consistently describes the thread-per-session
  server and records any verification not run.
- Browser implementation remains the only intentionally deferred product work.

### Not in this block

- New product behavior or redesign prompted only by hypothetical future scale.
- Browser framework selection or UI implementation.

## 18. Deferred browser implementation

The server completed by Blocks 1–14 exposes the asset boundary, REST API, SSE
contract, and framework-independent browser behavior required by the detailed
design. A separate design and plan must choose and implement:

- Browser language/framework/build tooling.
- Lobby and chat page component structure.
- The two-step create-then-open interaction and retry presentation.
- Streaming `fetch`/SSE parsing, bounded conflict retry, target/sequence mismatch
  reconnect, and control enablement after the initial snapshot.
- Prompt-editor draft behavior, typed Stop/default-agent controls, and
  request-scoped command feedback.
- Transcript/reasoning rendering, untrusted-text handling, styling,
  accessibility, responsive layout, and mobile behavior.

That future work must consume the contracts in `web-design.md`; it must not
reopen controller ownership, registry lifecycle, session exclusivity, or the
single-origin architecture merely to accommodate a UI choice.
