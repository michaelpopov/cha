# chaweb implementation plan

Status: proposed execution plan.

Last updated: 2026-07-30.

This document turns [`web-design.md`](web-design.md) into bounded implementation
blocks. Each numbered block is intended to fit in one Codex session and to leave
the repository building and tested. The blocks are deliberately smaller than
the delivery steps in the design where process management, concurrency, or
streaming would otherwise create too much context for one session.

The design document remains authoritative. If this plan and the design differ,
follow the design and update this plan as part of the same change.

## 1. How to execute this plan

A request such as “execute block 4” means:

1. Read this introduction, the complete requested block, the named sections of
   `web-design.md`, and the current versions of all files the block will touch.
2. Check which preceding blocks are already present. Do not assume that the
   repository exactly matches the file suggestions below.
3. Implement the block completely, including production code, focused tests,
   build changes, and directly affected architecture documentation.
4. Preserve the dependency boundaries in `src/README.md`: session policy stays
   in `session/`, web transport stays in `ui/web/`, and `web_main.cpp` remains a
   small composition root.
5. Do not implement later blocks merely because a placeholder or interface is
   needed. Add the smallest test seam or stub that gives the current block a
   real, tested boundary.
6. Run the focused tests for the block, then build and run the ordinary
   non-TUI test suite. Web process and stress tests are run separately through
   their CTest labels so the ordinary suite remains fast:

   ```bash
   cmake --preset console
   cmake --build --preset console
   ctest --test-dir build/console --output-on-failure \
       -LE "web_process|web_stress"
   ```

   When the block adds or changes a web process/stress test, also run its
   focused target or label, for example:

   ```bash
   ctest --test-dir build/console --output-on-failure -L web_process
   ```

   Existing configured build directories may be reused when appropriate.
7. Report the files changed, behavioral result, tests run, and any remaining
   limitation that belongs to a later block.

Every block must keep warnings enabled and must work with `CHA_BUILD_TUI=OFF`.
Platform-specific code must be isolated behind a portable interface and compile
on Linux, macOS, and Windows. A block may add a platform-conditional test when
the repository's existing process test harness is not portable, but it must not
silently make the product feature platform-specific.

When development has only a Linux host, a successful Linux build plus the
applicable Linux unit/process tests is the minimum local completion bar for
Blocks 1–13. Code must still preserve the portable interfaces and native
backends required by the design. Block 14 adds or exercises macOS and Windows
verification where runners exist and explicitly records any backend not run.

Do not select a browser framework, language, component library, bundler, or CSS
system while executing this plan. The actual lobby and chat browser
implementations require a separate design and plan. The server-side asset
boundary, REST API, and SSE contract are included here because that future
browser implementation depends on them.

## 2. Global implementation rules

These rules apply to every block and need not be repeated in an execution
request:

- One session worker process owns one live `SessionController`, `Transcript`,
  `SessionJournal`, `AgentRegistry`, and agent thread pool.
- Only the worker owner thread calls the live controller or reads borrowed
  session values. HTTP and SSE threads receive owning values.
- The lobby never constructs a `SessionController`.
- The lobby owns all lobby-side libuv process and control handles on one
  supervisor thread.
- The companion-file session lease is acquired before session restore and
  remains held until after controller and journal shutdown.
- A worker reports exactly one startup result. A successful control channel
  remains open so worker lifetime is tied to lobby lifetime.
- The lobby forgets session identity and worker port after redirect. It retains
  only generic process/control handles for shutdown and reaping.
- HTTP mutations are serialized through the owner queue. A command cancelled
  before being claimed must never execute later.
- Every SSE connection begins with a full snapshot. There is no replay log, no
  SSE `id:` field, and no `Last-Event-ID` behavior.
- Presentation revisions are worker-local application data, not persisted
  journal revisions or SSE event IDs.
- One active SSE stream is supported per worker. This is a lightweight usage
  guard, not authentication and not a strict tab-identity protocol.
- No permissive CORS behavior or authentication is added. Mutations still
  validate content type and same-origin browser requests.
- Tests must use fake controllers, clocks, timers, launchers, or child helpers
  where needed. They must not call a real LLM provider or depend on long
  wall-clock sleeps.
- Process tests against a real worker must use non-generating commands such as
  `/info`, `/agents`, an empty prompt, or an invalid slash command. A process
  test that needs generation must install a deterministic fake provider through
  a test-only workspace/configuration seam.
- Web process and longer stress tests use separate CTest labels
  (`web_process` and `web_stress`) or equivalent non-default targets. Unit
  tests remain in the fast ordinary suite.
- The initial trusted-LAN application has no global maximum worker count,
  per-client launch quota, or launch-rate limit. Resource tests verify bounded
  per-worker behavior and reliable cleanup; they must not introduce worker
  admission control as an incidental hardening change.
- Error responses use stable machine codes and presentation-safe messages.
  Internal paths, exceptions, credentials, prompts, and answers are not exposed
  or logged by default.

## 3. Block overview and dependencies

| Block | Result | Depends on |
| --- | --- | --- |
| 1 | Cross-process session lease used by all frontends | Existing session layer |
| 2 | Create-only stored-session operation | Block 1 |
| 3 | `cha_web` library, web test target, and owning protocol types | Existing build |
| 4 | Bounded startup protocol and portable worker launcher | Block 3 |
| 5 | Single-owner lobby worker supervisor | Block 4 |
| 6 | Lobby HTTP service and validated redirects | Blocks 2, 3, 5 |
| 7 | Session worker owner runtime and cancellable command queue | Blocks 1, 3 |
| 8 | Worker snapshot and command HTTP API | Block 7 |
| 9 | Revisioned SSE publication and backpressure | Blocks 7, 8 |
| 10 | Browser-stream and worker-lifetime state machine | Block 9 |
| 11 | Complete internal session-worker process mode | Blocks 4, 8, 9, 10 |
| 12 | Complete lobby process mode and end-to-end launch flow | Blocks 5, 6, 11 |
| 13 | Design conformance, resource ownership, races, and stress | Blocks 1–12 |
| 14 | Network, logging, platform, and documentation audit | Block 13 |

The recommended order is numerical. Blocks 1–2 and Block 3 begin separate
dependency branches, but executing them out of order saves little and makes
integration reviews harder.

## 4. Block 1 — Cross-process session lease

### Objective

Add the companion-file operating-system lock and make every controller-opening
path use it. At the end of this block, `cha`, `chacon`, and future chaweb
workers fail immediately and clearly when another process owns the selected
session.

### Read first

- `web-design.md` Sections 7.1–7.3, 16, and 20.1.
- `src/session/workspace.*`, `session_catalog.*`,
  `session_controller.*`, and `session_database.*`.
- TUI and console startup paths and their tests.

### Required work

1. Add a move-only `SessionLease` in `src/session/`.
   - Derive its companion path deterministically by appending `.cha-lock` to
     the complete database filename.
   - Open or create the companion file without using its existence as state.
   - Attempt an exclusive lock without waiting.
   - Keep the native file descriptor or handle open for the full object
     lifetime.
   - Release the lock and close the native resource in the destructor.
   - Isolate POSIX and Win32 details in implementation files or small private
     backends. Linux and macOS may use a non-blocking whole-file `flock`;
     Windows may use `LockFileEx` with
     `LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY`.
   - Treat “already locked” separately from I/O, permission, or malformed-path
     failures.
2. Add a typed `SessionBusyError` or an equivalent typed result that the
   worker can later map to startup status `busy` without parsing an English
   exception message. Its public message should identify the session as in use
   without exposing implementation details.
3. Change `Workspace::open_session()` so it performs this order:
   validate forum and session identifiers; resolve and validate the database;
   acquire the lease; restore state; load definitions as needed; construct the
   controller while moving lease ownership into it.
4. Change `Workspace::create_session()` so a newly created database is leased
   before its controller is constructed. A later failure leaves the valid
   stored database in the catalog but releases the lease.
5. Make `SessionController` own the production lease. Its declaration and
   destruction order must guarantee that the lease outlives
   `SessionJournal`. Test-only controller factories may use an explicitly
   inactive lease rather than locking an unrelated file.
6. Preserve existing frontend APIs where practical. TUI and console may still
   use their current top-level exception handling, but their user-facing error
   for a busy session must be immediate and clear.
7. Document the new type and ownership order in `src/session/README.md` and any
   affected invariant in `src/README.md`.

### Likely files

- `src/session/session_lease.h`
- `src/session/session_lease.cpp`, possibly platform-specific implementation
  files
- `src/session/workspace.h`
- `src/session/workspace.cpp`
- `src/session/session_controller.h`
- `src/session/session_controller.cpp`
- `tests/session/unit_session_lease.cpp`
- `tests/session/unit_workspace.cpp`
- a small process test/helper for genuine inter-process contention
- `CMakeLists.txt`

The names are suggestions; preserve local naming conventions if the current
tree has evolved.

### Tests and validation

- Acquire an unused lease and verify the companion filename.
- Reject a second acquisition while the first process holds the lease.
- Acquire successfully after orderly release.
- Terminate a helper process holding a lease and verify OS cleanup permits a
  new acquisition.
- Verify an existing but unlocked companion file is harmless.
- Verify lease I/O failures are not misreported as “busy.”
- Verify session restore is not attempted after lease contention. Use an
  injected seam or a deliberately broken database to prove ordering.
- Verify both create-and-open and open-existing controller lifetimes hold the
  lease.
- Verify existing TUI/console-facing startup code reports the new busy
  condition.

### Completion criteria

- No production path can construct a live controller for a stored session
  without first acquiring its companion lease.
- The lease remains held through explicit controller shutdown and journal
  destruction.
- Contention behavior is proven with at least one multi-process test.
- All pre-existing tests continue to pass.

### Not in this block

- Lobby create-only behavior.
- Worker startup `busy` serialization.
- Browser or HTTP error mapping.

## 5. Block 2 — Create a stored session without opening it

### Objective

Give the lobby a session-layer operation that creates and returns catalog
metadata without initializing providers, acquiring a live-session lease, or
constructing a controller.

### Read first

- `web-design.md` Sections 7.4, 10, and 20.4.
- The Block 1 implementation.
- `Workspace`, `SessionCatalog`, forum validation, and workspace unit tests.

### Required work

1. Add a clearly named create-only operation such as:

   ```cpp
   [[nodiscard]] SessionSummary create_stored_session(
       const std::string& forum_name,
       std::string label) const;
   ```

2. The operation must:
   - Validate and load the forum metadata required by catalog creation.
   - Validate the forum's static persona definitions if that is part of the
     established definition of a usable forum, but not initialize completion
     clients or providers.
   - Create the database through `SessionCatalog` using its existing atomic
     no-overwrite behavior.
   - Return the created ID and effective label in `SessionSummary`.
   - Not construct `SessionController`, `AgentRegistry`, `ThreadPool`, or
     `WakeNotifier`, and not leave a session lease held.
3. Refactor `Workspace::create_session()` to share the catalog-creation logic
   without changing terminal frontend behavior. It should create the stored
   session, then follow the ordinary leased open path.
4. Preserve the race defined by the design: another process may acquire the
   new session after create-only returns and before a future worker opens it.
   Do not add a hidden lease handoff or rollback mechanism.
5. Update `src/session/README.md` to distinguish create-only from
   create-and-open.

### Likely files

- `src/session/workspace.h`
- `src/session/workspace.cpp`
- `tests/session/unit_workspace.cpp`
- `src/session/README.md`

### Tests and validation

- Create-only returns the stored ID and effective label.
- The session appears exactly once in `Workspace::sessions()`.
- Empty labels receive the same effective generated label as the existing
  create path.
- Invalid forums and invalid definitions fail without creating a database.
- A provider configuration that would fail during provider initialization does
  not prevent create-only if static validation succeeds.
- The returned session can subsequently be opened normally.
- Holding its lease between creation and open causes the later open to report
  busy while the stored session remains present.
- Existing TUI/console create-and-open tests still pass.

### Completion criteria

- The lobby can create persistent session metadata using only `Workspace` and
  without a notifier or live controller.
- Existing create-and-open semantics are unchanged apart from the global lease.

### Not in this block

- Launching a worker after creation.
- Returning `session_created_but_busy` over HTTP.

## 6. Block 3 — Web library and protocol foundation

### Objective

Create the reusable `cha_web` library and its focused test target, then define
the owning, presentation-neutral request/response types shared by the lobby,
worker runtime, HTTP routes, and SSE code.

### Read first

- `web-design.md` Sections 4, 10–13, 16, and 18.
- `src/ui/README.md`, JSON conventions in `src/agents/`, and current CMake test
  organization.

### Required work

1. Create `src/ui/web/` and a short `README.md` describing its dependency and
   ownership boundaries.
2. Add a `cha_web` static library:
   - Link it publicly or privately as appropriate to `cha_core` and
     `httplib::httplib`.
   - Apply C++20 and the repository warning settings.
   - Link `chaweb_app` to `cha_web`; keep `web_main.cpp` a placeholder in this
     block.
3. Add a focused `cha_web_tests` target linked to `cha_web` and GoogleTest.
   Register it with CTest. Web tests should not make every core unit test
   depend on cpp-httplib.
4. Define owning protocol values for:
   - Forum and session summaries.
   - Persona summaries.
   - Transcript entries.
   - Generation status.
   - Complete session snapshot, including its worker-local presentation
     revision from the first schema and serialization fixture.
   - Web command result.
   - Stable error response.
   - SSE event name and owning payload.
5. Do not place `TranscriptView`, `std::span`, string views into controller
   storage, references, or raw domain pointers in these types. It is acceptable
   to reuse scalar domain IDs and enums where their semantics are already
   stable, but JSON mappings must remain explicit.
6. Define explicit enum-to-JSON spellings and a common error envelope. Use one
   stable shape throughout later blocks, for example:

   ```json
   {"error":{"code":"session_busy","message":"Session is already in use"}}
   ```

7. Add small HTTP helpers for JSON content type and response serialization,
   but do not add actual routes yet.
8. Define named limits and deadline settings in one injectable settings value.
   Defaults may initially be implementation constants. Tests in later blocks
   must be able to substitute short durations and small capacities.
9. Add a shared temporary test-workspace builder under `tests/support/`. It
   must be able to create a minimal valid `app.toml`, forum metadata, persona
   definitions, sessions directory, and stored sessions in a unique temporary
   root. Let callers override configuration and persona/provider fixtures so
   later real-process tests can install a deterministic fake provider without
   copying workspace setup.
10. Do not define framework-specific browser models or generated client code.

### Likely files

- `src/ui/web/README.md`
- `src/ui/web/protocol.h`
- `src/ui/web/protocol.cpp`
- `src/ui/web/json.h`
- `src/ui/web/json.cpp`
- `src/ui/web/http_response.*`
- `src/ui/web/web_settings.h`
- `tests/support/test_workspace.*`
- `tests/ui/web/unit_protocol.cpp`
- `CMakeLists.txt`

### Tests and validation

- Every protocol enum has a stable documented JSON spelling.
- Snapshots and errors serialize to exact contract fixtures.
- The initial snapshot fixture contains its presentation revision; Block 9
  extends its semantics rather than retrofitting the field.
- Arbitrary transcript and notice strings are escaped as JSON data.
- Owning protocol objects remain valid after the source transcript mutates or
  is destroyed.
- Invalid or unknown request enum values fail as client input rather than
  falling through to a default operation.
- The shared workspace builder creates isolated valid roots and removes them
  through existing test cleanup conventions.
- The new library and test target build with TUI disabled.

### Completion criteria

- Later blocks can depend on `cha_web` without placing web files in
  `cha_core`.
- Protocol types are owning and have exact serialization tests.
- Later web and process tests have one shared workspace builder instead of
  private copies of forum/persona/app configuration fixtures.
- No browser technology has been selected.

### Not in this block

- Session-to-snapshot projection.
- HTTP listeners or route registration.
- Process launching.

## 7. Block 4 — Worker startup protocol and process launcher

### Objective

Implement the bounded startup record and a portable libuv launcher that starts
one child with one duplex control channel and explicitly controlled handle
inheritance.

### Read first

- `web-design.md` Sections 6.2, 8.1–8.3, 16, 19.1, 20.2, and 23.10.
- The Block 3 web library.
- `UvEventLoop`, libuv linkage, and existing integration-test conventions.

### Required work

1. Define `WorkerStartupRecord` with exactly three variants:
   - `ready` with a valid TCP port.
   - `busy` with no internal details.
   - `error` with a bounded presentation-safe message.
2. Use one explicit framing rule for the startup channel. A newline-terminated
   UTF-8 JSON object with a small fixed maximum such as 4 KiB is suitable.
   The decoder must handle fragmented reads, reject an oversized record,
   reject malformed JSON or invalid fields, reject EOF before the delimiter,
   and stop after exactly one record.
3. Add `SessionProcessLauncher` as a supervisor-thread-owned primitive.
   A launch request contains the executable path, workspace root, forum ID,
   session ID, and any internal launch data the worker will need, including the
   validated lobby URL if a future back-link is enabled.
4. Create one duplex `uv_pipe_t` per child and expose the child endpoint only
   through an internal inherited descriptor/handle. Use an internal argument
   only to identify that inherited endpoint; do not make it a general
   user-facing feature.
5. Build the child stdio array so it contains only intentionally retained
   standard streams and that child's control endpoint. Mark unrelated handles
   non-inheritable or close-on-exec and close the unused channel end in each
   process immediately after spawn.
6. Keep both the lobby process handle and its control endpoint alive after a
   `ready` record. For `busy`, `error`, malformed record, EOF, or timeout,
   transition to terminal cleanup and reap the child.
7. Do not let request threads call this primitive. The launcher API may assert
   or document that all methods run on one libuv owner thread; Block 5 supplies
   that thread.
8. Add a dedicated test-child executable rather than requiring a real session
   worker. It must support deterministic modes: ready with a selected port,
   ready then exit immediately, busy, error, fragmented record, malformed
   record, oversized record, EOF before record, hang until parent closes, and
   ignore graceful close for force-stop testing.
9. Do not send chat data, heartbeats, or periodic status over the channel after
   readiness.

### Likely files

- `src/ui/web/worker_startup_protocol.*`
- `src/ui/web/session_process_launcher.*`
- `tests/ui/web/unit_worker_startup_protocol.cpp`
- `tests/ui/web/worker_test_child.cpp`
- `tests/ui/web/process_launcher_test.cpp`
- `CMakeLists.txt`

### Tests and validation

- Encode/decode all three records, including fragmented input.
- Reject invalid ports, missing fields, unknown statuses, extra startup
  records, malformed UTF-8/JSON, and records beyond the limit.
- Report EOF-before-record and startup timeout distinctly.
- Preserve the control channel and child process handle after ready.
- Exercise a child that writes `ready` and exits immediately; the
  ready-record/process-exit callback race must complete once without a leaked
  or reused handle.
- Close and reap terminal busy/error children.
- Launch two siblings and prove neither inherits the other's endpoint.
- Close the lobby endpoint and prove the ready test child observes EOF.
- Ensure a delayed or late callback cannot use a destroyed launch record.
- Exercise paths and arguments containing spaces.

### Completion criteria

- A test parent can portably launch and supervise the deterministic child.
- Ready returns a port while retaining live generic OS/libuv resources.
- All failure paths close handles and reap processes.

### Not in this block

- Concurrent launch requests.
- A real `--session-worker` mode.
- Lobby HTTP routing.

## 8. Block 5 — Lobby worker supervisor

### Objective

Put all lobby-side libuv process/control operations on one dedicated supervisor
loop and expose a thread-safe, bounded launch API to future HTTP handlers.

### Read first

- `web-design.md` Sections 8.4–8.5, 9.2, 16, 19.1, and 20.2.
- The Block 4 launcher and test child.
- `ConcurrentQueue`, `UvEventLoop`, and their tests.

### Required work

1. Add a `WorkerSupervisor` whose dedicated thread owns:
   - Its libuv loop and cross-thread wake mechanism.
   - Every `uv_process_t`, lobby-side control pipe, startup timer, and exit
     callback.
   - Starting launch state and ready generic supervision records.
2. Expose a thread-safe typed request that an HTTP thread can wait on for a
   bounded startup result: ready port, busy, safe error, timeout, or supervisor
   stopping.
3. Queue launch requests to the supervisor. A caller timeout or disconnect
   must not cause the supervisor thread to dereference caller-owned state.
   Results should use shared owning completion state, promise/future, or an
   equivalent lifetime-safe mechanism.
4. Serialize all spawn, read, timeout, close, terminate, and reap transitions
   on the supervisor loop. Callbacks must be idempotent under races among
   startup completion, EOF, timer expiration, process exit, and lobby shutdown.
5. After delivering a ready result, erase forum ID, session ID, port, request
   authority, and redirect data from supervisor state. Retain only a generic
   record containing process/control handles and shutdown/reap state.
6. Implement clean supervisor shutdown:
   - Reject new launches.
   - Resolve waiting launches with a bounded stopping result.
   - Close every starting and ready lobby control endpoint.
   - Wait for child exit callbacks until an injectable clean-stop deadline.
   - Force-terminate remaining children through the portable launcher backend.
   - Reap every child and close all handles before joining the thread.
7. Ensure destruction either performs this shutdown or requires an explicit
   completed shutdown with a debug assertion. No joinable thread or libuv
   handle may escape.

### Likely files

- `src/ui/web/worker_supervisor.h`
- `src/ui/web/worker_supervisor.cpp`
- `tests/ui/web/unit_worker_supervisor.cpp`
- `tests/ui/web/process_supervisor_test.cpp`

### Tests and validation

- Concurrent callers launch several deterministic children while all libuv
  operations occur on the single supervisor thread.
- Ready, busy, error, malformed, EOF, and startup timeout results reach the
  correct caller once.
- A ready-then-immediate-exit child may race result delivery with its exit
  callback, but is reaped exactly once and leaves no supervision handle.
- Lobby shutdown racing with an unready child resolves the request and leaves
  no child or handle behind.
- Generic ready records contain no routing identity or port.
- Clean shutdown causes cooperative children to exit on EOF.
- An unresponsive child is force-terminated only after the injected deadline.
- Every child is reaped and the supervisor thread joins.

### Completion criteria

- Future cpp-httplib handlers need no direct knowledge of libuv objects.
- The lobby can forget routing while still guaranteeing worker shutdown and
  process reaping.

### Not in this block

- HTTP endpoints.
- Signal handling in `web_main.cpp`.
- Launching a real session worker.

## 9. Block 6 — Lobby HTTP service

### Objective

Implement the lobby REST service, worker launch responses, safe redirect
construction, and an asset-serving boundary without selecting the actual
browser technology.

### Read first

- `web-design.md` Sections 6.1, 8.3–8.5, 10, 15, 16, and 18.
- Blocks 2, 3, and 5.
- cpp-httplib server tests/examples already available in the fetched version.

### Required work

1. Add `LobbyServer` with injected `Workspace`, `WorkerSupervisor`, web
   settings, and an asset provider/root abstraction. Do not construct a
   controller.
2. Implement:
   - `GET /health`
   - `GET /api/v1/forums`
   - `GET /api/v1/forums/{forum}/sessions`
   - `POST /api/v1/forums/{forum}/sessions`
   - `POST /api/v1/forums/{forum}/sessions/{session}/open`
3. Validate forum/session route values through `Workspace`; never concatenate
   them into filesystem paths in the route layer.
4. Define and test exact request and response JSON for listing and creation.
   Require JSON content type and bounded bodies for mutating routes.
5. On open, submit a launch request and wait for its bounded startup result.
   Map outcomes as follows:
   - ready: `303 See Other` with `Location` on the worker origin.
   - busy: `409 Conflict` with stable code `session_busy`.
   - invalid forum/session: a validation-oriented 4xx.
   - launch/startup/internal failure: presentation-safe 5xx.
6. On create, call the create-only operation exactly once, then launch that
   stored session. If the worker reports busy, return `409 Conflict` with code
   `session_created_but_busy` and include the created `SessionSummary`. Do not
   delete it and do not retry creation.
7. Parse the incoming `Host` as an HTTP authority:
   - Accept hostname, IPv4, or bracketed IPv6.
   - Remove the incoming lobby port.
   - Preserve/restore IPv6 brackets and append the worker port.
   - Reject userinfo, paths, whitespace/control characters, malformed
     brackets, invalid ports, and ambiguous unbracketed IPv6.
   - Never emit wildcard bind addresses such as `0.0.0.0` in `Location`.
8. Derive a safe external lobby origin at request time for the optional worker
   back-link launch data. This routing value must disappear from supervisor
   state after the ready result.
9. Do not emit permissive CORS headers. On mutations with an `Origin` header,
   require it to match the validated origin of the lobby request.
10. Add an asset handler interface that can serve a future lobby entry point
    with correct MIME types, no path traversal, and an explicit not-found
    response. Tests may use tiny fixture assets; do not select or scaffold a
    browser framework.
11. Bound cpp-httplib worker count, pending requests where the library permits,
    payload size, and relevant socket timeouts through web settings.

### Likely files

- `src/ui/web/lobby_server.*`
- `src/ui/web/authority.*`
- `src/ui/web/asset_handler.*`
- `tests/ui/web/unit_authority.cpp`
- `tests/ui/web/unit_lobby_server.cpp`
- `tests/ui/web/fixtures/`

### Tests and validation

- List forums and sessions with exact JSON.
- Create once and redirect after fake worker readiness.
- Reproduce create-then-open contention and verify
  `session_created_but_busy` includes the one created session.
- Map worker busy and startup errors correctly.
- Reject invalid route identifiers without path access.
- Test hostname, hostname-with-port, IPv4, bracketed IPv6, wildcard bind, bad
  authority syntax, and header-injection characters.
- Verify `Location` contains the request-reachable host and reported worker
  port.
- Verify Origin/content-type/body-size policy for mutations.
- Verify fixture asset serving and traversal rejection.
- Verify `/health` never exposes worker or session routing state.

### Completion criteria

- `LobbyServer` can be tested in-process with a fake supervisor and over a
  loopback cpp-httplib client.
- No controller or live-session object exists in the lobby service.
- Actual browser implementation remains undecided.

### Not in this block

- Wiring the listener in `web_main.cpp`.
- Real worker process readiness.
- Lobby shutdown signals.

## 10. Block 7 — Worker owner runtime and command queue

### Objective

Create the single-owner session runtime independently of HTTP. It must open and
own one controller on its dedicated thread, serialize commands and agent
notifications, copy all borrowed state, and implement the pending/claimed
command deadline contract.

### Read first

- `web-design.md` Sections 9, 11.1–11.3, 12, 16, and 20.3.
- Blocks 1 and 3.
- `SessionController`, `handle_text_input()`, `UvEventLoop`,
  `ConcurrentQueue`, transcript ownership documentation, and console session
  behavior.

### Required work

1. Add `WebSessionRuntime` with a permanent owner thread. Controller creation,
   use, shutdown, and destruction all occur on that thread.
2. Supply the runtime with the workspace root, forum/session IDs, settings, and
   a controller factory seam. Production uses `Workspace::open_session()`;
   tests use deterministic fake backends/controllers without network access.
3. Reuse the owner thread's `UvEventLoop` as the `WakeNotifier` passed to the
   controller. Its wake processing must drain both queued web work and
   `SessionController::receive()` so agent events continue even with no
   browser.
4. Define typed web commands for:
   - Raw text through `handle_text_input()`.
   - Stop.
   - Clear.
   - Open, extend, and restore off-record state.
   - Change default persona.
   - Obtain a full snapshot.
   - Internal shutdown/lobby-gone notifications needed by later blocks.
5. If the typed default-persona operation only has a stable persona ID while
   the controller only accepts a display handle, add an ID-based controller
   operation with session-layer tests. Keep text `/@Name` behavior unchanged
   and delegate both paths to one authoritative validation implementation.
6. Give every HTTP-originated envelope atomic states `pending`, `claimed`,
   `completed`, and `cancelled`:
   - The submitter waits only a bounded time for `pending` to be claimed.
   - On expiry it atomically changes `pending` to `cancelled`.
   - The owner skips cancelled envelopes.
   - If the owner wins and changes it to `claimed`, the caller waits for the
     short synchronous domain result and receives the real result.
   - Do not add a post-claim response that falsely says the command did not
     apply.
7. Wake the loop after a successful enqueue. Reject new work once stopping
   begins. Close/drain queues in an order that cannot strand a waiting caller.
8. Convert each command's `SessionUpdate` into an owning `WebCommandResult`.
   Do not parse notice text to determine behavior. Preserve `clear_input`,
   `end_session`, application/refusal state, and the latest presentation
   revision.
9. Build session snapshots only on the owner thread by copying forum/session
   metadata, personas, transcript entries, default persona, and generation
   status into owning protocol values.
10. Add an event-publication interface for later SSE work. In this block a
    recording test sink is sufficient; it must accept only owning values.
11. Record the owner thread ID in test/debug builds or expose an injected
    assertion seam so tests prove all controller calls occur on it.

### Likely files

- `src/ui/web/web_session_runtime.*`
- `src/ui/web/web_command.*`
- `src/ui/web/session_snapshot.*`
- `tests/ui/web/unit_web_command.cpp`
- `tests/ui/web/unit_web_session_runtime.cpp`
- possibly `src/session/session_controller.*` for ID-based default selection

### Tests and validation

- Controller construction, every command, `receive()`, state reads, shutdown,
  and destruction occur on one owner thread.
- Raw input preserves ordinary prompt, `@mention`, `/mcast`, slash commands,
  and `/exit` semantics.
- Typed actions call the same controller operations as the text grammar.
- A command cancelled while pending never executes after the owner resumes.
- If claim wins the race, the caller receives the true result.
- Several submitter threads are serialized by the owner.
- A caller disappearing after claim does not cause automatic retry.
- Agent events are drained and persisted with no connected output consumer.
- An owning snapshot remains valid after later transcript mutation.
- Stopping rejects new commands and releases all waiters.

### Completion criteria

- No borrowed transcript/persona/generation value crosses the owner-thread
  boundary.
- The runtime works and is thoroughly testable without cpp-httplib.
- Queue timeout semantics are deterministic and non-ambiguous.

### Not in this block

- HTTP status mapping.
- SSE socket writing or backpressure.
- Browser disconnect timers.

## 11. Block 8 — Worker snapshot and command HTTP API

### Objective

Expose the owner runtime through bounded cpp-httplib routes, while keeping HTTP
threads transport-only and preserving unknown-outcome semantics after a
claimed mutation.

### Read first

- `web-design.md` Sections 9.2, 10–12, 15.2–15.4, 16, and 20.5.
- Blocks 3 and 7.
- Current cpp-httplib version's bind/listen and task-pool APIs.

### Required work

1. Add `SessionWorkerServer` with injected runtime, settings, and asset
   provider.
2. Implement these non-streaming routes:
   - `GET /health`
   - `GET /api/v1/session`
   - `POST /api/v1/input`
   - `POST /api/v1/actions/stop`
   - `POST /api/v1/actions/clear`
   - `POST /api/v1/actions/offrecord/open`
   - `POST /api/v1/actions/offrecord/extend`
   - `POST /api/v1/actions/offrecord/restore`
   - `POST /api/v1/actions/default-agent`
   - `POST /api/v1/close`, initially routed to an injected shutdown request
     whose full sequencing is completed in Block 10.
3. Define exact JSON request shapes. `input` carries one raw string; default
   agent carries a stable persona ID. Reject missing, wrong-type, unknown, and
   extra security-sensitive fields consistently.
4. A handler may parse, bound, enqueue, wait for the runtime result, and
   serialize an owning result. It must never call the controller or retain
   `TranscriptView`.
5. Map a pre-claim deadline cancellation to `503 Service Unavailable` with
   code `owner_queue_timeout`. Domain refusals remain successful command
   responses with structured outcome data. Fatal runtime failures produce a
   safe 5xx and initiate worker shutdown through the injected coordinator.
6. If a network connection disappears after claim, permit the runtime command
   to finish. Add no automatic server retry and no idempotency fiction.
7. Require expected JSON content type for mutations. Validate an optional
   browser `Origin` against the worker request's validated `Host`; emit no
   permissive CORS headers.
8. Apply payload, prompt, header, connection, task-pool, and socket timeout
   bounds. Ensure the configured HTTP task pool will later have capacity for
   one long-lived SSE request plus normal commands and health/snapshot calls.
9. Add chat asset handling through the framework-neutral asset abstraction.
   Fixture assets are enough until the separate browser plan.
10. Keep `/health` presentation-safe. It may initially report starting/ready
    and will gain detailed connection states in Block 10.

### Likely files

- `src/ui/web/session_worker_server.*`
- `src/ui/web/request_validation.*`
- `tests/ui/web/unit_session_worker_server.cpp`

### Tests and validation

- Every route parses valid JSON and returns exact response fixtures.
- Raw input and every typed action reach the expected runtime command.
- Full snapshot contains owning transcript/persona/generation data.
- Malformed JSON, wrong content type, excessive body/prompt, and invalid
  persona ID receive stable client errors.
- Pending command timeout returns exactly `owner_queue_timeout` and never
  executes.
- Domain refusal is not mapped to transport failure.
- Same-origin policy accepts a matching Origin, rejects a mismatched one, and
  permits non-browser clients with no Origin under the trusted-LAN design.
- Concurrent handler tests prove controller calls remain on the owner thread.
- Fixture assets cannot escape their root and untrusted transcript text stays
  JSON data.

### Completion criteria

- A loopback client can inspect and mutate a fake-backed worker session through
  the documented REST API.
- HTTP threads contain no domain ownership or session-thread assumptions.

### Not in this block

- `GET /api/v1/events`.
- Presentation diffs, heartbeats, or reconnect.
- Final close/SSE drain sequencing.

## 12. Block 9 — Revisioned SSE and bounded presentation output

### Objective

Add live Server-Sent Events without blocking the owner thread, including
owning event projection, worker-local presentation revisions, coalescing,
heartbeats, and bounded backpressure recovery.

### Read first

- `web-design.md` Sections 9.3–9.4, 12, 13, 16, and 20.3/20.5.
- Blocks 7 and 8.
- cpp-httplib chunked content-provider behavior in the pinned version.

### Required work

1. Add a bounded owner-to-SSE presentation channel. The owner thread may only
   publish owning events; it must never perform a socket write or wait for the
   browser.
2. Add an owner-thread projector that compares the last published owning state
   with current controller state and emits the smallest reliable update:
   `entry-added`, `entry-appended`, `entry-finished`, `transcript-reset`,
   `generation`, `notice`, or a full `snapshot`. If a safe incremental diff is
   unclear, publish a replacement snapshot rather than guessing.
3. Assign presentation revisions after any append/generation coalescing:
   - The initial/full snapshot carries the current revision.
   - Each state-bearing incremental event advances by one.
   - Heartbeats and stateless control signals do not advance it.
   - Revisions are worker-local and reset with a new worker process.
4. Register `GET /api/v1/events` using cpp-httplib's streaming/chunked response
   support. Set `text/event-stream`, disable inappropriate buffering/caching,
   write a full snapshot first, then drain queued events until closure.
5. Emit named SSE events and JSON `data:` records. Never emit an SSE `id:`
   field and ignore `Last-Event-ID` rather than treating it as a replay
   request.
6. Send comment heartbeats at an injectable interval. A failed write closes
   the stream and reports the disconnect to runtime state without blocking
   controller draining.
7. Implement bounded backpressure:
   - Coalesce compatible high-frequency append/generation updates before
     revision assignment.
   - If the queue cannot retain a correct consecutive sequence, discard
     fine-grained events and arrange a `resync` plus full snapshot, or close the
     stream so reconnect starts with a snapshot.
   - Never allow a slow consumer to make the owner queue or agent channels
     unbounded.
8. The browser-side rule to request `GET /api/v1/session` on a skipped or
   regressive revision belongs to the later UI, but server contract tests must
   make such recovery possible.
9. Use a server-local stream ID so a late close callback can be distinguished
   from a newer connection. Do not send this ID to the browser.

### Likely files

- `src/ui/web/presentation_projector.*`
- `src/ui/web/sse_channel.*`
- `src/ui/web/sse_writer.*`
- updates to `web_session_runtime.*` and `session_worker_server.*`
- `tests/ui/web/unit_presentation_projector.cpp`
- `tests/ui/web/unit_sse_channel.cpp`
- `tests/ui/web/sse_contract_test.cpp`

### Tests and validation

- Every connection begins with a full snapshot.
- Append, terminal, clear, generation, notice, and session-ended encodings are
  exact.
- State-bearing revisions are consecutive; duplicate snapshots retain their
  declared current revision.
- Coalescing occurs before revision assignment.
- Output contains no `id:` lines and reconnect performs no replay.
- A `Last-Event-ID` header has no protocol effect.
- Heartbeats do not change revision.
- Slow-consumer overflow causes deterministic resync/closure while owner-side
  agent draining continues.
- Disconnect wakes the runtime promptly and a duplicate late-close callback
  cannot detach a stream with a different internal ID.
- Untrusted multiline text is encoded correctly as SSE JSON data.

### Completion criteria

- Streaming model changes can reach a client while persistence and controller
  progress remain independent of network speed.
- Snapshot replacement always provides a recovery path from lost presentation
  events.

### Not in this block

- Rejecting a second simultaneous stream.
- Reconnect grace, orphan-generation timing, or idle worker exit.
- Actual browser SSE code.

## 13. Block 10 — Browser-stream and worker-lifetime state machine

### Objective

Implement the supported one-page guard, reload/reconnect behavior, idle and
orphan-generation policies, and one idempotent worker shutdown coordinator.

### Read first

- `web-design.md` Sections 11.4, 13.1, 14, 16, 19.2, and
  20.3–20.6.
- Blocks 7–9.

### Required work

1. Model explicit runtime states:
   `Starting`, `AwaitingClient`, `Connected`, `ReconnectGrace`,
   `OrphanGeneration`, `Stopping`, and `Exiting`.
   All transitions that inspect controller state occur on the owner thread.
2. Enforce one active SSE stream:
   - Accept the first stream.
   - Reject another active stream with `409 Conflict` and code
     `browser_stream_in_use`.
   - Clear the slot only for the matching internal stream ID.
   - Do not add attachment tokens, epochs, browser storage, takeover, or REST
     authorization by stream identity.
3. Make `/health` report the presentation-safe worker state and whether an
   active stream exists. This is advisory presentation data; the SSE endpoint
   remains the authoritative conflict decision.
4. On first readiness, start an injectable awaiting-client deadline. Exit
   cleanly if no SSE client arrives.
5. On active-stream close:
   - Enter reconnect grace and continue draining controller events.
   - Accept a replacement stream and send a full current snapshot.
   - If grace expires while idle, begin shutdown.
   - If generation remains active, enter bounded orphan generation.
6. During orphan generation:
   - Continue receiving and persisting agent events.
   - Accept a reconnect, cancel the orphan timer, send a snapshot showing the
     active generation, and restore Stop.
   - If generation finishes unattended, persist its terminal state and shut
     down immediately.
   - At the hard limit, cancel through ordinary controller shutdown.
7. Implement explicit `/api/v1/close` and text `/exit`:
   - Atomically mark stopping and reject later commands/SSE streams.
   - Complete the initiating HTTP response before transport teardown.
   - Publish `session-ended` best-effort.
   - Let the shutdown coordinator, never the owner thread, wait only for the
     injectable final-SSE drain deadline.
   - Treat EOF after a successful close response as normal.
8. Add a single idempotent shutdown coordinator used by explicit close, idle
   expiry, orphan completion/limit, fatal runtime failure, future lobby EOF,
   and process shutdown.
9. Define join and lifetime order:
   - Mark stopping and reject work.
   - Finish initiating response where applicable.
   - Give final SSE its bounded opportunity.
   - Stop the HTTP listener.
   - Wake and shut down the owner controller on its owner thread.
   - Join HTTP and owner threads in a non-self-joining order.
   - Destroy controller/journal before releasing the lease.
10. Make all timers injectable or manually advanceable in tests. Do not use
    long sleeps to test phone-like suspension.

### Likely files

- `src/ui/web/worker_lifecycle.*`
- `src/ui/web/worker_shutdown.*`
- updates to runtime, SSE, and worker server files
- `tests/ui/web/unit_worker_lifecycle.cpp`
- `tests/ui/web/worker_lifecycle_test.cpp`

### Tests and validation

- First stream accepted; simultaneous second stream gets
  `browser_stream_in_use` without changing lifecycle state.
- Matching close enters grace; duplicate/late close cannot clear a newer
  stream.
- Reconnect during grace and orphan generation succeeds with a full snapshot.
- Idle grace expiry shuts down.
- Active generation continues and persists while disconnected.
- Unattended generation completion exits before the orphan hard limit.
- Orphan hard limit invokes controller shutdown.
- Explicit close and `/exit` finish HTTP first and either flush
  `session-ended` within the deadline or continue without it.
- Concurrent close, fatal error, and disconnect invoke shutdown exactly once.
- New commands and streams are rejected after stopping starts.
- No path blocks the owner thread on SSE or joins the current thread.

### Completion criteria

- The complete worker browser/session lifetime policy is deterministic under
  races and tested with virtual or injected time.
- The implementation remains deliberately lightweight: one stream slot, no
  browser identity protocol.

### Not in this block

- Parent/lobby EOF wiring.
- Process signals and executable exit codes.
- Browser-side retry code.

## 14. Block 11 — Internal session-worker process mode

### Objective

Compose the real worker mode inside `chaweb`: open one leased session, bind an
available port, report readiness, watch lobby lifetime, serve REST/SSE, and
exit cleanly.

### Read first

- `web-design.md` Sections 6.2, 8.1–8.3, 9.5, 14.5, 15, 17, and 19.2.
- Blocks 1, 4, and 7–10.
- The shared test-workspace builder introduced in Block 3.
- `src/apps/console_main.cpp` for configuration/logging/signal composition
  conventions.

### Required work

1. Add a private worker-mode argument parser. It must accept only the internal
   data produced by `SessionProcessLauncher`: workspace root, forum/session
   IDs, inherited control endpoint, and optional validated lobby URL. Invalid
   internal invocations fail safely without opening a session.
2. Derive a distinct worker log filename from
   `ApplicationConfig.log_file` before initializing logging. Include role and
   process ID in the name; include forum/session IDs as structured log context,
   not raw prompt/answer bodies.
3. Start the control endpoint on the worker owner loop early enough that lobby
   EOF during partial startup is observed. Convert EOF into an ordered
   `LobbyGone` runtime event.
4. Perform startup in the design order:
   - Load configuration and initialize worker logging.
   - Construct runtime/event-loop infrastructure.
   - Validate identifiers and open the session, acquiring its lease before
     restore.
   - Construct the controller.
   - Bind cpp-httplib on the configured interface using an OS-assigned port
     (`bind_to_any_port` or equivalent).
   - Report one `ready` record with the actual port.
   - Keep the control endpoint open and start/continue serving.
   The initial implementation makes one atomic OS-assigned-port bind attempt.
   It does not retry a general bind failure; configured port-range iteration
   remains deferred.
5. Map `SessionBusyError` to startup `busy`. Map all other pre-ready failures
   to one bounded, presentation-safe `error` record where possible. Never send
   exception internals or paths.
6. If the control channel closes before readiness, or the readiness write
   fails, clean partial startup: stop any listener, shut down/destroy any
   controller, release the lease, and exit.
7. After readiness, control EOF immediately enters the same idempotent shutdown
   path as other triggers. It preempts reconnect and orphan-generation grace
   and may skip the final SSE drain for prompt teardown.
8. Add process signal handling that submits the same ordered shutdown request;
   do not call controller methods from a signal callback.
9. Close the worker control endpoint only after controller shutdown and lease
   release are complete. Return stable process exit codes for orderly shutdown,
   bad internal invocation, startup failure, and fatal runtime failure.
10. Keep `web_main.cpp` limited to mode selection, configuration/logging
    setup, component construction, top-level exception mapping, and exit code.

### Likely files

- `src/ui/web/worker_application.*`
- `src/ui/web/parent_lifetime_watcher.*`
- `src/ui/web/web_arguments.*`
- `src/apps/web_main.cpp`
- `tests/ui/web/session_worker_process_test.cpp`
- updates to `src/apps/README.md` and `src/ui/web/README.md`

### Tests and validation

- A harness launches a real worker, receives ready/port, calls health and
  snapshot, opens SSE, sends a non-generating command such as `/info`, and
  closes it. It must not contact a real completion provider.
- A held lease returns `busy` and no listener remains.
- Invalid session, configuration, or bind failure returns safe `error` and
  releases resources.
- EOF during each partial-startup phase exits without a leaked process or
  lease.
- Specifically inject EOF after lease acquisition but before controller
  construction, then prove another process can immediately acquire the lease;
  companion-file existence alone must not affect the assertion.
- EOF after ready shuts down the worker and releases the lease without waiting
  browser grace.
- An explicit close returns successfully before process exit.
- Worker log path differs from the configured base and includes role/process
  identity.
- Worker arguments and workspace paths containing spaces are handled.

### Completion criteria

- The launcher can replace its deterministic child with a real session worker.
- A worker cannot outlive the control endpoint supplied by its parent.
- All live session objects remain confined to the worker owner thread/process.

### Not in this block

- Normal lobby-mode listener.
- Full create/open redirect from an end-user request.
- Actual browser assets.

## 15. Block 12 — Lobby process mode and end-to-end flow

### Objective

Compose the normal `chaweb` lobby process and prove the complete
lobby → worker → redirect → REST/SSE flow with multiple different sessions.

### Read first

- `web-design.md` Sections 5, 6.1, 8, 10, 15–19, and 20.4.
- Blocks 5, 6, and 11.
- The shared test-workspace builder introduced in Block 3; extend it instead of
  creating a second end-to-end workspace fixture.
- Existing application composition roots and integration-test harnesses.

### Required work

1. Implement normal no-worker-flag lobby mode in `web_main.cpp` or a composed
   `LobbyApplication`:
   - Load environment and application configuration.
   - Derive a lobby-specific role/process log filename and initialize logging.
   - Construct `Workspace`.
   - Start `WorkerSupervisor`.
   - Construct and bind `LobbyServer` to configured host/port.
   - Serve until a process shutdown signal or fatal lobby failure.
2. Ensure the executable can reliably locate and spawn its own executable image
   on Linux, macOS, and Windows. Do not assume the current working directory
   contains `chaweb`.
3. Pass the worker the same workspace root, selected identifiers, inherited
   control endpoint, and validated external lobby URL when needed.
4. Return `303` only after the real worker has acquired the lease, constructed
   its controller, bound its listener, and reported ready.
5. Immediately after the response is formed, ensure supervisor state for that
   worker contains no session ID, port, Host, or redirect URL.
6. Support concurrent workers for different sessions. Racing attempts for the
   same session may spawn two processes, but exactly one acquires the lease and
   the other returns busy.
7. Implement lobby shutdown in the required order:
   - Reject launches.
   - Stop the lobby HTTP listener.
   - Close all starting/ready control endpoints.
   - Wait through the clean worker deadline.
   - Force-terminate only remaining workers.
   - Reap all children and close libuv handles.
   - Stop logging and exit.
8. Make unexpected lobby termination testable by using a parent harness that
   can terminate the lobby and observe its worker independently.
9. Update top-level README/app documentation with:
   - How to launch `chaweb`.
   - Required LAN/wildcard bind for phone access.
   - Need to permit lobby and worker ports through the desktop firewall.
   - The initial implementation uses unrestricted OS-assigned worker ports, so
     firewall policy may need to permit the platform's ephemeral port range.
     A configured worker-port range remains a deferred design parameter.
   - Ephemeral worker URL behavior and return-to-lobby limitation.
   - Trusted-network/no-auth warning.
   - The fact that actual bundled browser UI remains a separate delivery item
     if it has not yet been selected.

### Likely files

- `src/ui/web/lobby_application.*`
- `src/apps/web_main.cpp`
- `tests/ui/web/chaweb_process_test.cpp`
- `README.md`
- `src/apps/README.md`
- `src/ui/web/README.md`
- `CMakeLists.txt`

### Tests and validation

- Start lobby, list forums/sessions, open one, follow `Location`, obtain worker
  snapshot, connect SSE, submit only non-generating input such as `/info`, and
  close. This path must not contact a real completion provider.
- Create a session, follow redirect, and verify it exists exactly once.
- Exercise `session_created_but_busy`.
- Run two different sessions concurrently on different worker ports.
- Open the same session concurrently and observe one ready/one busy. Verify the
  losing worker exits, is reaped, leaves no supervision/control record, and
  does not retain the session lease.
- Verify second active SSE stream receives `browser_stream_in_use`.
- Clean lobby shutdown stops and reaps all workers and releases all leases.
- Abrupt lobby loss makes every worker observe EOF and exit.
- Abruptly terminate a ready worker while the lobby remains alive, verify its
  process is reaped and its OS lease is released, then reopen the same session
  successfully through the lobby. Use a test-only spawn observer to obtain the
  child process handle/PID without adding production routing state.
- A replacement lobby can reopen after predecessor workers complete shutdown.
- No-browser startup timeout exits an abandoned worker.
- Redirect host works from a non-loopback-style Host fixture, including IPv6.
- Lobby and worker log files do not collide.

### Completion criteria

- The complete server-side chaweb architecture works as designed through real
  processes and real loopback HTTP/SSE.
- Several different sessions can run concurrently while one session remains
  exclusive.
- Stopping the lobby stops its workers.

### Not in this block

- Choosing or implementing the actual browser UI.
- Worker rediscovery, reverse proxying, authentication, or stable/bookmarkable
  worker URLs.

## 16. Block 13 — Resource, race, and design-conformance audit

### Objective

Prove that the completed server-side flow satisfies the design under resource
pressure, process/thread races, and every shutdown phase, without leaking or
double-closing native resources.

### Read first

- The complete `web-design.md`, especially Sections 7–9, 13–14, 16, 19, and
  20.
- Blocks 1–12 and all current web/session/process tests.
- Native-resource and shutdown documentation for the libuv and cpp-httplib
  versions pinned by the build.

### Required work

1. Create a design-conformance checklist from `web-design.md`
   Sections 20.1–20.6. For the resource, concurrency, process, SSE, and
   lifecycle items in this block, point each item to an existing test or add
   the missing test. Leave network/logging/platform/documentation items marked
   for Block 14 rather than expanding this block.
2. Audit the lifetime of every native or concurrent resource:
   - Session lock descriptor/handle.
   - Spawned process handle.
   - Both endpoints of every worker-control channel.
   - libuv loop, timer, async, pipe, and process handles.
   - HTTP listener, task-pool work, SSE writer, and socket state.
   - Owner command envelopes, result waiters, output queues, owner thread, and
     supervisor thread.
   Prove that every normal, exception, timeout, race, and shutdown path releases
   each resource once and wakes every waiter.
3. Validate handle inheritance with more than one simultaneous worker. On
   POSIX, audit close-on-exec and stdio-slot behavior. Keep the Win32
   inheritable-handle review for Block 14's platform pass, but retain
   platform-neutral regression tests here.
4. Stress bounded resources using injectable reduced limits:
   - HTTP body/prompt limits.
   - HTTP task/connection pressure with one long-lived SSE stream.
   - Owner command queue claim/cancel races.
   - SSE output overflow, coalescing, resync, and disconnect.
   - Startup, awaiting-client, reconnect, orphan-generation, final-SSE-drain,
     and forced-stop deadlines.
   Register longer cases with the `web_stress` CTest label.
   This stress work is per worker and per launch lifecycle; do not add a global
   worker cap or launch-rate limiter, which the design explicitly leaves out of
   the initial trusted-LAN application.
5. Exercise shutdown and callback races repeatedly:
   - Lobby shutdown during spawn, lease acquisition, restore, controller
     construction, bind, and readiness write.
   - A child that reports ready and exits before or during ready-result
     delivery.
   - Worker explicit close concurrent with lobby EOF and process signal.
   - Worker crash followed by process reaping and lease reacquisition.
   - SSE close callback after a replacement stream.
   - Agent completion during reconnect/orphan timeout and controller shutdown.
   - Owner command cancellation concurrent with claim and global shutdown.
6. Add one named real-process test using the shared test workspace and a
   deterministic fake provider:
   - Launch a real worker and connect its SSE stream.
   - Submit a real prompt and wait until fake streamed generation is active.
   - Disconnect SSE before the fake provider finishes.
   - Release the fake provider to complete the response while no browser is
     connected.
   - Verify the terminal result is persisted.
   - Reconnect during the permitted lifetime or reopen after unattended worker
     shutdown, then verify the authoritative snapshot contains the completed
     response.
   Register the test as `web_process` or `web_stress`. It must not contain
   credentials, use the network, or fall back to a real completion provider.
7. Add counters, debug assertions, or test-only observers where needed to
   prove one-time transitions and cleanup. Do not expose a production worker
   registry or browser attachment protocol merely for testing.
8. Run an existing sanitizer or dynamic-analysis preset when the repository
   provides one. Focus on callbacks retaining destroyed launch records,
   command-result waiters, HTTP shutdown, thread joins, output queues, and
   libuv handle closure. If no such preset exists, record that fact in the
   completion report; adding general sanitizer/CI infrastructure is optional
   and its absence does not block this block.
9. Fix every correctness or cleanup defect found. Update directly affected
   architecture documentation when implementation details change, but leave
   the final cross-document reconciliation to Block 14.

### Likely files

Changes may span prior web/session implementation files, test helpers, process
and stress tests, sanitizer configuration, and narrowly affected architecture
documentation. Do not introduce new product features.

### Tests and validation

Run the fast suite and the focused process/stress labels separately:

```bash
cmake --preset console
cmake --build --preset console
ctest --test-dir build/console --output-on-failure \
    -LE "web_process|web_stress"
ctest --test-dir build/console --output-on-failure -L web_process
ctest --test-dir build/console --output-on-failure -L web_stress
```

Also run an existing applicable sanitizer or dynamic-analysis target. If none
exists, record “not available” rather than treating the block as incomplete.
Record any native backend that cannot be exercised on the current host for
Block 14.

### Completion criteria

- Every resource/concurrency/lifecycle item from the design checklist points to
  a passing test.
- No known leaked process, lease, thread, socket, waiter, queue item, or libuv
  handle remains.
- Race tests demonstrate idempotent shutdown, callback handling, and reaping.
- The ordinary unit suite remains fast; longer tests are isolated by label.
- The fake-provider disconnect/persist process test passes without external
  network or credentials.
- Existing sanitizer/dynamic-analysis targets pass, or their absence is
  explicitly recorded without blocking completion.

### Not in this block

- Final network-security, log-content, native-platform, or documentation audit.
- Browser technology, authentication, reverse proxying, worker rediscovery, or
  multi-page support.

## 17. Block 14 — Network, logging, platform, and documentation audit

### Objective

Complete the design-conformance checklist at the external and operational
boundaries, verify all supported native backends as far as the available
environment permits, and reconcile documentation with the implementation.

### Read first

- The complete `web-design.md`, especially Sections 8.3, 15–18, 20, and 22.
- The Block 13 conformance checklist and its report of unexecuted native
  backends.
- Current platform CI configuration, if present.
- All user and architecture documentation describing configuration, logging,
  networking, and executable behavior.

### Required work

1. Complete the remaining `web-design.md` Section 20 checklist entries and
   resolve any gap rather than merely documenting it.
2. Audit the network boundary:
   - Host/authority validation and redirect injection.
   - Hostname, IPv4, and bracketed IPv6 construction.
   - Origin comparison for mutations.
   - Absence of permissive CORS behavior.
   - Content-type, body, header, connection, and timeout limits.
   - Safe MIME types and asset traversal protection.
   - Untrusted labels, transcript text, reasoning, notices, and provider
     errors remain inert data.
   - Health and error responses disclose no sensitive state.
3. Audit diagnostic logging:
   - Lobby and every worker derive unique role/process-specific files.
   - Records include PID, role, and worker forum/session context where useful.
   - Launch/startup, control EOF, lease, bind, stream, lifecycle, fatal error,
     shutdown, and reaping events are diagnosable.
   - Prompts, answers, reasoning, credentials, secrets, internal paths, and
     raw exception bodies are absent by default.
4. Build and test on Linux, macOS, and Windows where runners are available.
   Specifically audit:
   - POSIX lock and Win32 `LockFileEx` behavior.
   - POSIX close-on-exec and Win32 explicit inherited-handle lists.
   - Duplex libuv child-pipe behavior and parent EOF on every platform.
   - Process termination, waiting, and reaping.
   - Executable self-location and paths containing spaces/non-ASCII text.
5. If the current environment offers only one platform, add or update CI-ready
   platform jobs and compile-time coverage. Clearly list which native tests
   were not executed locally; do not claim cross-platform runtime verification
   without evidence.
6. Reconcile `README.md`, `src/README.md`, `src/session/README.md`,
   `src/ui/web/README.md`, `src/apps/README.md`, `web-design.md`, and this plan.
   Record:
   - Concrete timeout, capacity, and size defaults and whether each is constant
     or configuration.
   - Trusted-network/no-auth exposure.
   - LAN bind and firewall requirements.
   - The use of unrestricted OS-assigned worker ports and possible need to
     permit the platform's ephemeral range.
   - The configured worker-port range as deferred work, if still unimplemented.
   - Ephemeral worker URLs, no rediscovery, and return-to-lobby limitations.
   - Browser implementation as a separate remaining delivery item.
7. Confirm CTest labels and documented commands keep unit, web process, and
   web stress tests selectable independently.
8. Update the conformance checklist with final test/file references and leave
   no unexplained difference between implemented behavior and
   `web-design.md`.

### Likely files

Changes may span network/logging helpers, native platform backends, CMake/CI
configuration, contract tests, and documentation. Do not use the audit to
introduce a new architecture or browser stack.

### Tests and validation

Run the fast suite and both web labels on each available platform:

```bash
cmake --preset console
cmake --build --preset console
ctest --test-dir build/console --output-on-failure \
    -LE "web_process|web_stress"
ctest --test-dir build/console --output-on-failure -L web_process
ctest --test-dir build/console --output-on-failure -L web_stress
```

Add focused native-backend and network-contract commands when they are not
covered by those labels. Record platform results in the completion report.

### Completion criteria

- Every bullet in `web-design.md` Section 20 points to a passing test or to the
  separately deferred browser implementation where the design explicitly
  places it.
- Network and logging boundaries satisfy the trusted-LAN design without
  leaking sensitive information.
- All supported platforms compile, and runtime results are recorded for every
  available runner.
- Code and documentation agree on limits, ports, logging, exposure, lifecycle,
  errors, and unsupported scenarios.
- The backend is ready for the separately selected browser implementation.

### Not in this block

- React, TypeScript, or any other browser technology decision.
- Authentication, TLS termination, reverse proxying, worker rediscovery,
  multi-page session support, or an integrated multi-session chat shell.

## 18. Deferred browser implementation

The numbered blocks intentionally stop at a complete and tested server-side
application boundary. A separate browser-design discussion must decide how to
implement the lobby and chat pages using a technology that LLMs can generate
and maintain reliably.

That later plan must consume, rather than redesign, these established
contracts:

- Lobby listing, create, open, redirect, and error routes.
- Worker snapshot and typed/raw command routes.
- One active SSE stream with full-snapshot reconnect.
- Presentation revision gap detection followed by snapshot replacement.
- Bounded retry for a reload that briefly receives
  `browser_stream_in_use`.
- Disabled controls and a clear conflict message when another stream remains.
- Stop and explicit close behavior.
- A normal link back to the supplied lobby URL.
- Safe rendering of all server-provided text as untrusted data.

Until that decision is made, fixture assets used by server tests are not a
product UI and must not grow into an accidental framework choice.
