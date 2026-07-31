# chaweb implementation plan

Status: proposed execution plan.

Last updated: 2026-07-31.

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
- The lobby owns all lobby-side control channels and launch timers on one
  control-loop thread. It retains no worker PID or process handle.
- On POSIX the lobby enables automatic child reaping with `SA_NOCLDWAIT`; on
  Windows it launches session workers with `CREATE_NO_WINDOW` and closes the
  process and thread handles returned by `CreateProcessW()` immediately. A
  worker inherits no console standard handles and never opens a console window;
  lobby mode may use its normal console. The lobby never waits for, reaps, or
  force-terminates a worker.
- The companion-file session lease is acquired before session restore and
  remains held until after controller and journal shutdown.
- A worker reports exactly one startup result. A successful control channel
  remains open so worker lifetime is tied to lobby lifetime.
- The lobby forgets session identity and worker port after returning the ready
  response. It retains only a generic control endpoint and EOF/close state.
- HTTP mutations are serialized through the owner queue. An accepted command
  has one generous completion deadline; expiry has unknown outcome and starts
  worker shutdown rather than cancelling the queued command.
- Every SSE connection begins with a full snapshot. There is no replay log, no
  SSE `id:` field, and no `Last-Event-ID` behavior.
- `snapshot` and target-aware `append` are the only state-bearing SSE events.
  The writer has one immutable in-flight payload and at most one replaceable
  pending payload; the owner never waits for network output.
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
| 4 | Bounded startup protocol and portable fire-and-forget worker launcher | Block 3 |
| 5 | Single-owner lobby worker control loop | Block 4 |
| 6 | Lobby HTTP service and ready-port responses | Blocks 2, 3, 5 |
| 7 | Session worker owner runtime and bounded command queue | Blocks 1, 3 |
| 8 | Worker snapshot and command HTTP API | Block 7 |
| 9 | Snapshot/append SSE with latest-state mailbox | Blocks 7, 8 |
| 10 | Browser-stream and worker lifetime | Block 9 |
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
   - Complete session snapshot, including configured `lobby_port`, current
     notice, optional foreground request identity, and coarse worker lifecycle
     phase.
   - Web command result.
   - Stable error response.
   - The `snapshot`/`append` SSE event names and owning payload union.
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
- The initial snapshot fixture contains `lobby_port`, current notice, optional
  foreground request identity, and coarse worker lifecycle phase.
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

Implement the bounded startup record, a utility-layer platform process spawner,
and a web launcher that starts one fire-and-forget worker with one duplex
control channel and explicitly controlled descriptor/handle inheritance.

### Read first

- `web-design.md` Sections 6.2, 8.1–8.3, 16, 19.1, 20.2, and 23.10.
- The Block 3 web library.
- `UvEventLoop`, existing platform utilities, and integration-test
  conventions.

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
3. Add `FireAndForgetProcessSpawner` in `src/util/`, whose only
   responsibility is to create a process from an executable, argument vector,
   and explicit inherited descriptor/handle set:
   - On POSIX, use `posix_spawn()` for each launch.
   - On Windows, use `CreateProcessW()` with an explicit inherited-handle list
     and `CREATE_NO_WINDOW`; do not specify `CREATE_NEW_CONSOLE` or
     `DETACHED_PROCESS`. Use extended startup information, direct unused
     standard streams to `NUL`, and do not inherit the lobby's console handles.
     Close the returned process and primary-thread handles immediately.
   - Do not expose or retain a PID, native process handle, exit callback, or
     exit status after a successful launch.
   - Keep session identifiers, control protocol, ports, and timeouts out of the
     utility.
4. As lobby composition policy, install POSIX `SA_NOCLDWAIT` with the default
   `SIGCHLD` disposition before any worker is launched. Do not put session or
   worker lifecycle policy into `FireAndForgetProcessSpawner`.
5. Add `SessionProcessLauncher` as a control-loop-owned web primitive. A launch
   request contains the executable path, workspace root, forum ID, session ID,
   and the inherited control endpoint data the worker needs. It does not carry
   a browser hostname, advertised origin, or full lobby URL.
6. Create one duplex control channel per worker and expose the worker endpoint
   only through an internal inherited descriptor/handle. Use an internal
   argument only to identify that inherited endpoint; do not make it a general
   user-facing feature.
7. Pass only intentionally retained standard streams and that worker's control
   endpoint to the process-spawn utility. Mark unrelated handles
   non-inheritable or close-on-exec and close the unused channel end in each
   process immediately after spawn. A later worker must never inherit an
   earlier worker's endpoint.
8. After `ready`, retain only the lobby control endpoint. For `busy`, `error`,
   malformed record, EOF, or timeout, close the lobby endpoint. The worker is
   responsible for detecting EOF, cleaning up, and exiting; POSIX disposes of
   its status automatically.
9. Do not let request threads call this primitive. The launcher API may assert
   or document that all methods run on one control-loop thread; Block 5
   supplies that thread.
10. Add a dedicated test-worker executable rather than requiring a real session
   worker. It must support deterministic modes: ready with a selected port,
   ready then exit immediately, busy, error, fragmented record, malformed
   record, oversized record, EOF before record, hang until parent closes, and
   report whether it observed parent EOF.
11. Do not send chat data, heartbeats, or periodic status over the channel
    after readiness. Both endpoints keep an asynchronous read active solely to
    detect EOF.

### Likely files

- `src/util/process_spawner.*`
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
- Preserve only the lobby control endpoint after ready.
- Exercise a worker that writes `ready` and exits immediately; the
  ready-record/EOF race must complete once without a leaked or reused channel
  handle.
- Close terminal busy/error control endpoints; on POSIX verify the exited
  workers leave no zombie and no waitable status.
- Launch two siblings and prove neither inherits the other's endpoint.
- Close the lobby endpoint and prove the ready test child observes EOF.
- Ensure a delayed or late callback cannot use a destroyed launch record.
- Exercise paths and arguments containing spaces.
- On Windows, verify the process and primary-thread handles returned during
  creation are closed immediately.
- On Windows, verify a session worker has no attached console or console
  standard handles and that no new console window is created. Also verify that
  the spawner does not request `CREATE_NEW_CONSOLE` or `DETACHED_PROCESS` and
  that worker mode never calls `AllocConsole()`.

### Completion criteria

- A test lobby can portably launch and handshake with the deterministic worker.
- Ready returns a port while retaining only the live control endpoint.
- All failure paths close lobby-owned channel and temporary spawn resources;
  the lobby never waits for or reaps a worker.

### Not in this block

- Concurrent launch requests.
- A real `--session-worker` mode.
- Lobby HTTP routing.

## 8. Block 5 — Lobby worker control loop

### Objective

Put all lobby-side control-channel operations on one dedicated loop and expose
a thread-safe, bounded launch API to future HTTP handlers.

### Read first

- `web-design.md` Sections 8.4–8.5, 9.2, 16, 19.1, and 20.2.
- The Block 4 launcher and test child.
- `ConcurrentQueue`, `UvEventLoop`, and their tests.

### Required work

1. Add a `WorkerControlLoop` whose dedicated thread owns:
   - Its libuv loop and cross-thread wake mechanism.
   - Every lobby-side control pipe and startup timer.
   - Starting launch state and ready generic control records.
2. Expose a thread-safe typed request that an HTTP thread can wait on for a
   bounded startup result: ready port, busy, safe error, timeout, or control loop
   stopping.
3. Queue launch requests to the control loop. A caller timeout or disconnect
   must not cause the control thread to dereference caller-owned state.
   Results should use shared owning completion state, promise/future, or an
   equivalent lifetime-safe mechanism.
4. Serialize all spawn, read, timeout, EOF, and close transitions on the
   control loop. Callbacks must be idempotent under races among startup
   completion, EOF, timer expiration, and lobby shutdown.
5. After delivering a ready result, erase forum ID, session ID, port, request
   completion state, and any temporary spawn data. Retain only a generic record
   containing the control endpoint and EOF/close state.
6. Continue an asynchronous read after readiness. EOF closes the local
   endpoint and removes the generic control record; no exit status is expected.
7. Implement clean control-loop shutdown:
   - Reject new launches.
   - Resolve waiting launches with a bounded stopping result.
   - Close every starting and ready lobby control endpoint.
   - Close the loop's remaining channel/timer resources and join without
     waiting for worker termination.
8. Ensure destruction either performs this shutdown or requires an explicit
   completed shutdown with a debug assertion. No joinable thread or libuv
   handle may escape.

### Likely files

- `src/ui/web/worker_control_loop.h`
- `src/ui/web/worker_control_loop.cpp`
- `tests/ui/web/unit_worker_control_loop.cpp`
- `tests/ui/web/process_worker_control_test.cpp`

### Tests and validation

- Concurrent callers launch several deterministic children while all libuv
  control operations occur on the single control-loop thread.
- Ready, busy, error, malformed, EOF, and startup timeout results reach the
  correct caller once.
- A ready-then-immediate-exit worker may race result delivery with EOF, but its
  local endpoint is closed exactly once and leaves no control record.
- Lobby shutdown racing with an unready worker resolves the request and leaves
  no lobby-owned channel handle behind.
- Generic ready records contain no routing identity or port.
- Clean lobby shutdown closes every control endpoint; cooperative workers exit
  on EOF, independently of control-loop teardown.
- The control-loop thread joins without waiting for worker process completion.

### Completion criteria

- Future cpp-httplib handlers need no direct knowledge of libuv objects.
- The lobby can forget both routing and process identity while retaining only
  the pipe lifetime coupling required by workers.

### Not in this block

- HTTP endpoints.
- Signal handling in `web_main.cpp`.
- Launching a real session worker.

## 9. Block 6 — Lobby HTTP service

### Objective

Implement the lobby REST service, worker ready-port responses, same-origin
mutation checks, and an asset-serving boundary without selecting the actual
browser technology.

### Read first

- `web-design.md` Sections 6.1, 8.3–8.5, 10, 15, 16, and 18.
- Blocks 2, 3, and 5.
- cpp-httplib server tests/examples already available in the fetched version.

### Required work

1. Add `LobbyServer` with injected `Workspace`, `WorkerControlLoop`, web
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
   Require JSON content type and bounded bodies for mutating routes. A
   successful open/create launch response contains exactly one validated
   worker port, `{"port":<worker-port>}`.
5. On open, submit a launch request and wait for its bounded startup result.
   Map outcomes as follows:
   - ready: `200 OK` with `{"port":<worker-port>}`.
   - busy: `409 Conflict` with stable code `session_busy`.
   - invalid forum/session: a validation-oriented 4xx.
   - launch/startup/internal failure: presentation-safe 5xx.
6. On create, call the create-only operation exactly once, then launch that
   stored session. If the worker reports busy, return `409 Conflict` with code
   `session_created_but_busy` and include the created `SessionSummary`. Do not
   delete it and do not retry creation.
7. Validate the ready port as an integer in the range 1–65535 before
   serialization. Do not return `Location`, construct a URL from `Host`, add an
   advertised-host setting, or pass an external lobby URL to the worker.
8. Retain the independent same-origin mutation policy. Do not emit permissive
   CORS headers. When an `Origin` header is present, validate the request
   `Host`/`Origin` pair and require a match. Reject malformed values, but never
   use either header to construct a navigation URL.
9. Add an asset handler interface that can serve a future lobby entry point
    with correct MIME types, no path traversal, and an explicit not-found
    response. Tests may use tiny fixture assets; do not select or scaffold a
    browser framework.
10. Bound cpp-httplib worker count, pending requests where the library permits,
    payload size, and relevant socket timeouts through web settings.

### Likely files

- `src/ui/web/lobby_server.*`
- `src/ui/web/asset_handler.*`
- `tests/ui/web/unit_lobby_server.cpp`
- `tests/ui/web/fixtures/`

### Tests and validation

- List forums and sessions with exact JSON.
- Create once and return the ready port after fake worker readiness.
- Reproduce create-then-open contention and verify
  `session_created_but_busy` includes the one created session.
- Map worker busy and startup errors correctly.
- Reject invalid route identifiers without path access.
- Verify a ready result produces exactly `{"port":<worker-port>}` and rejects
  an invalid startup port; no response contains `Location`.
- Verify malformed and mismatched `Host`/`Origin` pairs, content type, and body
  limits for mutations without implementing navigation-URL construction.
- Verify fixture asset serving and traversal rejection.
- Verify `/health` never exposes worker or session routing state.

### Completion criteria

- `LobbyServer` can be tested in-process with a fake worker control loop and over a
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
notifications, copy all borrowed state, and implement the command-completion
deadline and unknown-outcome shutdown contract.

### Read first

- `web-design.md` Sections 9, 11.1–11.3, 12, 16, and 20.3.
- Blocks 1 and 3.
- `SessionController`, `handle_text_input()`, `UvEventLoop`,
  `ConcurrentQueue`, transcript ownership documentation, and console session
  behavior.

### Required work

1. Add `WebSessionRuntime` with a permanent owner thread. Controller creation,
   use, shutdown, and destruction all occur on that thread.
2. Supply the runtime with the workspace root, forum/session IDs, the
   configured lobby port, settings, and a controller factory seam. Production
   uses `Workspace::open_session()`; tests use deterministic fake
   backends/controllers without network access.
3. Reuse the owner thread's `UvEventLoop` as the `WakeNotifier` passed to the
   controller. Its wake processing must drain both queued web work and
   `SessionController::receive()` so agent events continue even with no
   browser. Use bounded batches or equivalent fair interleaving so a sustained
   agent-event stream cannot starve commands.
4. Define owner-runtime web commands for:
   - Raw text through `handle_text_input()`.
   - Stop.
   - Change default persona.
   - Obtain a full snapshot.
   - Internal shutdown/lobby-gone notifications needed by later blocks.
   Clear and off-record browser controls go through the raw-text command as
   `/clear`, `/hide-on`, `/hide`, and `/hide-off`; do not add separate runtime
   commands for them.
5. If the typed default-persona operation only has a stable persona ID while
   the controller only accepts a display handle, add an ID-based controller
   operation with session-layer tests. Keep text `/@Name` behavior unchanged
   and delegate both paths to one authoritative validation implementation.
6. Give every accepted HTTP-originated envelope an owning completion object
   shared by the handler and queue. The handler waits through one generous,
   injectable deadline covering queue delay and command execution. On expiry:
   - Do not cancel or remove the command; its outcome is unknown.
   - Return `503 Service Unavailable` with `worker_unresponsive` when the
     connection remains writable.
   - Invoke the idempotent shutdown coordinator and reject new work.
   - Permit late owner completion without accessing handler-owned state.
   Immediate enqueue rejection while stopping or at queue capacity remains a
   known not-accepted result.
7. Wake the loop after a successful enqueue. Reject new work once stopping
   begins. Close/drain queues in an order that cannot strand a waiting caller.
8. Convert each command's `SessionUpdate` into an owning `WebCommandResult`.
   Preserve `clear_input` and notice. Do not add an `applied`, accepted, or
   refusal field, and do not derive one from `clear_input`, `render_needed`, or
   notice text.
   Apply notice null/empty/value semantics to runtime-owned current notice
   before completing the command, and publish its change as a structural
   snapshot. The later browser treats response notices as request-scoped only;
   they never overwrite snapshot-owned notice state.
9. Build session snapshots only on the owner thread by copying forum/session
   metadata, personas, transcript entries, default persona, generation status
   with an optional stable foreground request ID, current notice, and worker
   lifecycle phase into owning protocol values. The web runtime owns the
   current notice implied by `SessionUpdate`'s null/empty/value semantics.
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
- Raw input preserves ordinary prompt, `@mention`, `/mcast`, and the documented
  slash-command semantics.
- Raw `/clear`, `/hide-on`, `/hide`, and `/hide-off` reach the controller
  operations used by the corresponding browser controls.
- Typed Stop and stable-ID default-persona actions use the same authoritative
  session semantics as their text equivalents.
- A delayed HTTP command response cannot overwrite current notice or other
  state delivered by a later snapshot.
- An accepted command that misses its deadline reports unknown outcome,
  requests shutdown exactly once, and remains safe if it completes after the
  handler returns.
- Immediate enqueue rejection is reported as not accepted and cannot execute.
- Several submitter threads are serialized by the owner.
- Sustained agent events do not starve an accepted command.
- A caller disappearing after enqueue does not cancel or automatically retry
  the command.
- Agent events are drained and persisted with no connected output consumer.
- An owning snapshot remains valid after later transcript mutation.
- Stopping rejects new commands and releases all waiters.

### Completion criteria

- No borrowed transcript/persona/generation value crosses the owner-thread
  boundary.
- The runtime works and is thoroughly testable without cpp-httplib.
- Command timeouts deterministically initiate shutdown and report unknown
  outcome.

### Not in this block

- HTTP status mapping.
- SSE socket writing or latest-state mailbox behavior.
- Browser disconnect timers.

## 11. Block 8 — Worker snapshot and command HTTP API

### Objective

Expose the owner runtime through bounded cpp-httplib routes, while keeping HTTP
threads transport-only and preserving unknown-outcome semantics after an
accepted mutation.

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
   - `POST /api/v1/actions/default-agent`
3. Define exact JSON request shapes. `input` carries one raw string; default
   agent carries a stable persona ID. Reject missing, wrong-type, unknown, and
   extra security-sensitive fields consistently.
4. A handler may parse, bound, enqueue, wait for the runtime result, and
   serialize an owning result. It must never call the controller or retain
   `TranscriptView`.
5. Apply one generous command-completion deadline after successful enqueue.
   Expiry returns `503 Service Unavailable` with `worker_unresponsive` when
   possible, reports unknown outcome, and initiates worker shutdown through
   the injected coordinator. Domain refusals remain successful command
   responses with structured outcome data.
6. If a network connection disappears after enqueue, permit the runtime
   command to finish. Add no automatic server retry and no idempotency fiction.
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
11. Do not register `/api/v1/close`.

### Likely files

- `src/ui/web/session_worker_server.*`
- `src/ui/web/request_validation.*`
- `tests/ui/web/unit_session_worker_server.cpp`

### Tests and validation

- Every route parses valid JSON and returns exact response fixtures.
- Raw input, including the four browser-control strings `/clear`, `/hide-on`,
  `/hide`, and `/hide-off`, reaches the expected runtime command. Typed Stop
  and stable-ID default-agent requests reach their expected runtime commands.
- `/api/v1/close` is not registered.
- Full snapshot contains owning transcript/persona/generation data, optional
  foreground request identity, current notice, coarse lifecycle phase, and
  configured `lobby_port`.
- Malformed JSON, wrong content type, excessive body/prompt, and invalid
  persona ID receive stable client errors.
- Accepted-command timeout returns `worker_unresponsive`, reports unknown
  outcome, and invokes shutdown once; late command completion is safe.
- Immediate enqueue rejection reports that the command was not accepted and
  cannot execute.
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
- SSE payloads, latest-state mailbox behavior, heartbeats, or reconnect.
- Worker lifecycle and final-SSE drain sequencing.

## 12. Block 9 — Snapshot/append SSE and latest-state mailbox

### Objective

Add live Server-Sent Events without blocking the owner thread, using exactly
two state-bearing payloads, a single replaceable pending mailbox, UTF-8 append
continuity checks, and heartbeats.

### Read first

- `web-design.md` Sections 9.3–9.4, 12, 13, 16, and 20.3/20.5.
- Blocks 7 and 8.
- cpp-httplib chunked content-provider behavior in the pinned version.

### Required work

1. Define exactly two owning state-bearing payloads:
   - `snapshot`: the complete Section 12 state.
   - `append`: a target (`entry_id` for an answer or active `request_id` for
     reasoning), text, and `length_before` measured in UTF-8 bytes.
   Extend the controller's presentation-facing generation status with its
   active request ID if necessary; do not infer target identity from an agent
   display name.
2. Publish a snapshot for every structural change: entry start/finish/cancel/
   failure/discard, transcript clear, default-agent or notice change,
   generation activation/target/phase change, and worker lifecycle change.
   The first answer or reasoning delta establishes its target through such a
   snapshot. Only later text for the same established target may be an append.
   If append safety is unclear, build a current owning snapshot rather than
   maintaining a general-purpose diff projector.
3. Add an owner-to-SSE latest-state mailbox:
   - The writer may hold one immutable in-flight payload; the mailbox holds at
     most one replaceable pending owning payload.
   - With no pending payload, store the new payload.
   - Any update while a snapshot is pending rebuilds that pending snapshot
     from current owner state.
   - Compatible appends to the same target with continuous UTF-8 offsets merge
     by concatenating text and retaining the first `length_before`.
   - A structural change, incompatible target, or discontinuous append replaces
     a pending append with a fresh snapshot.
   - With no connected stream, retain no presentation backlog.
   The owner never writes to a socket or waits for the browser.
4. Register `GET /api/v1/events` using cpp-httplib's streaming/chunked response
   support. Set `text/event-stream`, disable inappropriate buffering/caching,
   write a full snapshot first, then drain mailbox payloads until closure.
5. Emit only named `snapshot` and `append` SSE events with JSON `data:`
   records. Never emit an SSE `id:` field and ignore `Last-Event-ID` rather
   than treating it as a replay request. Serialize no presentation revision or
   `resync` instruction.
6. Send comment heartbeats at an injectable interval. A failed write closes
   the stream and reports the disconnect to runtime state without blocking
   controller draining.
7. Specify browser recovery for a target or `length_before` mismatch: discard
   and close the old EventSource, then use the ordinary bounded reconnect path
   whose first payload is a snapshot. Do not fetch a REST snapshot concurrently
   with continued delivery from the old stream.
8. Use a server-local stream ID so a late close callback can be distinguished
   from a newer connection. Do not send this ID to the browser.
9. Keep `TranscriptView::revision` internal. It may avoid unnecessary snapshot
   work, but it is not a web protocol field because it does not cover notice,
   default-agent, generation-only, or lifecycle changes.

### Likely files

- `src/ui/web/sse_payload.*`
- `src/ui/web/sse_mailbox.*`
- `src/ui/web/sse_channel.*`
- `src/ui/web/sse_writer.*`
- updates to `web_session_runtime.*` and `session_worker_server.*`
- possibly `src/session/generation_status.h` and `session_controller.*` for
  stable active request identity
- `tests/ui/web/unit_sse_mailbox.cpp`
- `tests/ui/web/unit_sse_channel.cpp`
- `tests/ui/web/sse_contract_test.cpp`

### Tests and validation

- Every connection begins with a full snapshot.
- `snapshot` and target-aware `append` encodings are exact; no other
  state-bearing event name is emitted.
- Answer and reasoning appends identify stable targets and use UTF-8 byte
  offsets, including multibyte test data.
- Entry start/finish, clear, generation phase, notice, default-agent, and
  lifecycle changes produce snapshots.
- Compatible pending appends merge. A pending snapshot is rebuilt on later
  change, and structural/incompatible/discontinuous changes replace a pending
  append with a snapshot.
- At most one payload is in flight and one is pending while owner-side agent
  draining continues under a blocked writer.
- Output contains no `id:` lines and reconnect performs no replay.
- A `Last-Event-ID` header has no protocol effect.
- Output contains no presentation revision or `resync` payload.
- Heartbeats are comments and carry no state.
- Disconnect wakes the runtime promptly and a duplicate late-close callback
  cannot detach a stream with a different internal ID.
- Untrusted multiline text is encoded correctly as SSE JSON data.

### Completion criteria

- Streaming model changes can reach a client while persistence and controller
  progress remain independent of network speed.
- The fixed mailbox shape bounds queued payload count, and snapshot replacement
  or reconnect always provides a recovery path from an unsafe append.

### Not in this block

- Rejecting a second simultaneous stream.
- Browser-disconnection deadline timing or idle worker exit.
- Actual browser SSE code.

## 13. Block 10 — Browser-stream and worker lifetime

### Objective

Implement the supported one-page guard, snapshot-based reconnect, the single
browser-disconnection deadline rule, and one idempotent worker shutdown
coordinator.

### Read first

- `web-design.md` Sections 11.4, 13.1, 14, 16, 19.2, and
  20.3–20.6.
- Blocks 7–9.

### Required work

1. Keep only coarse startup, running, and stopping lifecycle phases, or
   equivalent flags, for readiness and ordered teardown. Do not turn browser
   connection/timing conditions or completed teardown into lifecycle states.
   All decisions that inspect controller state occur on the owner thread.
2. Enforce one active SSE stream:
   - Accept the first stream.
   - Reject another active stream with `409 Conflict` and code
     `browser_stream_in_use`.
   - Clear the slot only for the matching internal stream ID.
   - Do not add attachment tokens, epochs, browser storage, takeover, or REST
     authorization by stream identity.
3. Track only the optional active stream ID, whose presence defines
   `stream_active`, optional `disconnected_since`, and one rearmable timer. Set
   `disconnected_since` at worker readiness and on closure of the matching
   active stream. Clear it and cancel the timer whenever a stream is accepted.
   A rejected stream or stale close callback changes none of these values.
4. Apply one rule whenever no stream is active:

   ```text
   deadline = is_generating() ? orphan_limit : idle_grace
   if now - disconnected_since >= deadline: begin_shutdown()
   ```

   Use the same `idle_grace` for initial browser arrival and reconnect. Define
   `orphan_limit` as an absolute duration since `disconnected_since`, require
   it to be at least `idle_grace`, and do not add the two durations together.
5. Reevaluate and rearm the rule at readiness, matching stream close,
   successful connection, every generation-state transition, and timer expiry.
   If generation ends after idle grace has elapsed, begin shutdown promptly;
   otherwise retain the remaining idle grace.
6. Continue receiving and persisting agent events while disconnected. Accept a
   reconnect before shutdown, cancel the timer, send a full snapshot showing
   active generation when applicable, and restore Stop. At `orphan_limit`,
   cancel through ordinary controller shutdown.
7. Add no close endpoint, close command, unload handler, beacon, or keepalive
   close request. Browser tab close, reload, navigation, browser failure,
   network loss, and device suspension all set the same disconnected timestamp.
   Only the single deadline rule decides whether browser loss becomes worker
   shutdown.
8. Add a single idempotent shutdown coordinator used by disconnect-deadline
   expiry, command-completion deadline expiry, fatal runtime failure, future
   lobby EOF, and process shutdown.
9. Define join and lifetime order:
   - Mark stopping and reject work.
   - Publish a final snapshot with lifecycle `stopping` and a safe reason, and
     give it a bounded write opportunity.
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
  `browser_stream_in_use` without changing the active stream or disconnect
  timestamp.
- Matching close records `disconnected_since`; duplicate/late close cannot
  clear a newer stream or reset its timestamp.
- Initial browser absence and a later idle disconnect use the same
  `idle_grace`; reconnect before expiry succeeds with a full snapshot.
- Active generation continues and persists until the absolute `orphan_limit`.
- Starting or finishing generation while disconnected recomputes the deadline;
  finishing after idle grace initiates shutdown promptly.
- `orphan_limit` invokes controller shutdown and is never added to idle grace.
- `/api/v1/close` is not registered.
- Concurrent disconnect expiry, fatal error, and lobby/process shutdown invoke
  shutdown exactly once.
- A writable stream receives a final lifecycle `stopping` snapshot when
  possible; a blocked writer cannot extend shutdown beyond the final drain
  deadline.
- New commands and streams are rejected after stopping starts.
- No path blocks the owner thread on SSE or joins the current thread.

### Completion criteria

- The complete worker browser/session lifetime rule is deterministic under
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
   IDs, and inherited control endpoint. Invalid internal invocations fail
   safely without opening a session. The documented private invocation and
   parser must both include the workspace root explicitly.
2. Derive a distinct worker log filename from
   `ApplicationConfig.log_file` before initializing logging. Include role and
   process ID in the name; include forum/session IDs as structured log context,
   not raw prompt/answer bodies.
3. Start the control endpoint on the worker owner loop early enough that lobby
   EOF during partial startup is observed. Convert EOF into an ordered
   `LobbyGone` runtime event.
4. Perform startup in the design order:
   - Load application configuration from the supplied workspace root and
     initialize worker logging.
   - Construct runtime/event-loop infrastructure.
   - Validate identifiers and open the session, acquiring its lease before
     restore.
   - Construct the controller.
   - Bind cpp-httplib on the configured interface using an OS-assigned port
     (`bind_to_any_port` or equivalent).
   - Report one `ready` record with the actual port.
   - Keep the control endpoint open and start/continue serving.
   - Expose `ApplicationConfig.port` as `lobby_port` in session snapshots so
     the browser can construct its return link from the worker page URL.
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
   path as other triggers. It preempts the browser-disconnection deadline and
   may skip the final SSE drain for prompt teardown.
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
  for the browser-disconnection deadline.
- Closing the SSE connection and allowing the applicable disconnect deadline
  to expire shuts down the worker and releases the lease.
- Worker log path differs from the configured base and includes role/process
  identity.
- Worker arguments and workspace paths containing spaces are handled.
- Snapshot `lobby_port` matches the application configuration loaded from the
  supplied workspace root; no external lobby URL is accepted or retained.

### Completion criteria

- The launcher can replace its deterministic child with a real session worker.
- A worker treats loss of the control endpoint supplied by its parent as an
  unconditional shutdown trigger.
- All live session objects remain confined to the worker owner thread/process.

### Not in this block

- Normal lobby-mode listener.
- Full create/open ready-port response from an end-user request.
- Actual browser assets.

## 15. Block 12 — Lobby process mode and end-to-end flow

### Objective

Compose the normal `chaweb` lobby process and prove the complete
lobby → worker → ready-port response → REST/SSE flow with multiple
different sessions.

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
   - Configure automatic POSIX child reaping and start `WorkerControlLoop`.
   - Construct and bind `LobbyServer` to configured host/port.
   - Serve until a process shutdown signal or fatal lobby failure.
2. Ensure the executable can reliably locate and spawn its own executable image
   on Linux, macOS, and Windows. Do not assume the current working directory
   contains `chaweb`.
   - Windows lobby mode may retain its normal console, but every internal
     session-worker launch must use the no-console policy from Block 4.
3. Pass the worker the same workspace root, selected identifiers, and inherited
   control endpoint. Pass no browser hostname or external lobby URL.
4. Return `200 OK` with `{"port":<worker-port>}` only after the real worker has
   acquired the lease, constructed its controller, bound its listener, and
   reported ready.
5. Immediately after the response is formed, ensure control-loop state for that
   worker contains no session ID, worker port, browser request data, or URL.
6. Support concurrent workers for different sessions. Racing attempts for the
   same session may spawn two processes, but exactly one acquires the lease and
   the other returns busy.
7. Implement lobby shutdown in the required order:
   - Reject launches.
   - Stop the lobby HTTP listener.
   - Close all starting/ready control endpoints.
   - Close remaining control-loop resources and join its thread without
     waiting for worker process termination.
   - Stop logging and exit. Each worker detects EOF and is independently
     responsible for cleanup and termination.
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

- Start lobby, list forums/sessions, open one, read the returned port, obtain
  the worker snapshot, connect SSE, submit only non-generating input such as
  `/info`, and close. This path must not contact a real completion provider.
- Create a session, use its returned port, and verify it exists exactly once.
- Exercise `session_created_but_busy`.
- Run two different sessions concurrently on different worker ports.
- Open the same session concurrently and observe one ready/one busy. Verify the
  losing worker exits, leaves no zombie or control record, and does not retain
  the session lease.
- Verify second active SSE stream receives `browser_stream_in_use`.
- Clean lobby shutdown closes all control endpoints and exits without waiting;
  independently verify every cooperative worker observes EOF, exits, and
  releases its lease.
- Abrupt lobby loss makes every worker observe EOF and exit.
- Make a ready test worker terminate abruptly through a worker-local test seam,
  verify the lobby observes control EOF and its OS lease is released, then
  reopen the same session successfully. Production lobby state must never gain
  a child process handle or PID.
- A replacement lobby can reopen after predecessor workers complete shutdown.
- No-browser startup timeout exits an abandoned worker.
- Ready responses contain only the worker port and never a `Location` header.
- Worker snapshots contain the configured lobby port for the browser's return
  link.
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
   - Temporary process/thread handles used only during platform spawn.
   - Both endpoints of every worker-control channel.
   - libuv loop, timer, async, and pipe handles.
   - HTTP listener, task-pool work, SSE writer, and socket state.
   - Owner command envelopes, result waiters, SSE in-flight/pending mailbox
     payloads, owner thread, and lobby control-loop thread.
   Prove that every normal, exception, timeout, race, and shutdown path releases
   each resource once and wakes every waiter.
3. Validate handle inheritance with more than one simultaneous worker. On
   POSIX, audit close-on-exec and stdio-slot behavior. Keep the Win32
   inheritable-handle review for Block 14's platform pass, but retain
   platform-neutral regression tests here.
4. Stress bounded resources using injectable reduced limits:
   - HTTP body/prompt limits.
   - HTTP task/connection pressure with one long-lived SSE stream.
   - Owner command timeouts, late completion, fair scheduling, and idempotent
     shutdown.
   - SSE blocked-writer mailbox replacement, append merging, snapshot rebuild,
     mismatch recovery, and disconnect.
   - Worker startup, browser disconnection, and final-SSE-drain deadlines.
   Register longer cases with the `web_stress` CTest label.
   This stress work is per worker and per launch lifecycle; do not add a global
   worker cap or launch-rate limiter, which the design explicitly leaves out of
   the initial trusted-LAN application.
5. Exercise shutdown and callback races repeatedly:
   - Lobby shutdown during spawn, lease acquisition, restore, controller
     construction, bind, and readiness write.
   - A worker that reports ready and exits before or during ready-result
     delivery.
   - Browser disconnect expiry concurrent with lobby EOF and process signal.
   - Worker crash followed by automatic child disposal, control EOF, and lease
     reacquisition.
   - SSE close callback after a replacement stream.
   - Agent completion changing the disconnect deadline while a timer or
     controller shutdown is concurrent.
   - Owner command timeout and late completion concurrent with global shutdown.
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
   command-result waiters, HTTP shutdown, thread joins, SSE mailbox payloads,
   and libuv handle closure. If no such preset exists, record that fact in the
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
- Race tests demonstrate idempotent shutdown, EOF callback handling, and
  automatic POSIX child disposal.
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
   - Exact ready-port JSON and rejection of invalid ports.
   - Malformed and mismatched `Host`/`Origin` rejection and origin comparison
     for mutations.
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
     and shutdown events are diagnosable.
   - Prompts, answers, reasoning, credentials, secrets, internal paths, and
     raw exception bodies are absent by default.
4. Build and test on Linux, macOS, and Windows where runners are available.
   Specifically audit:
   - POSIX lock and Win32 `LockFileEx` behavior.
   - POSIX close-on-exec and Win32 explicit inherited-handle lists.
   - Duplex libuv child-pipe behavior and parent EOF on every platform.
   - POSIX `SA_NOCLDWAIT` behavior with no zombies or waitable worker status.
   - Windows `CREATE_NO_WINDOW` worker creation, absence of inherited console
     standard handles, and immediate closure of process/primary-thread handles.
     Confirm that lobby mode may continue using its own console without any
     worker opening an additional console window.
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

- Lobby listing, create, open, ready-port, and error routes.
- On a successful open/create response, construct a fresh URL from the lobby
  page URL, replace only its port with the returned port through the URL API,
  clear lobby path/query/fragment state, and navigate with `location.assign()`;
  test hostname, IPv4, and bracketed IPv6 page URLs.
- Worker snapshot, raw-input, typed Stop, and stable-ID default-agent routes.
  Clear and off-record controls submit `/clear`, `/hide-on`, `/hide`, and
  `/hide-off` as synthetic raw input without replacing or clearing the editor
  draft.
- One active SSE stream with full-snapshot reconnect.
- Apply answer and reasoning appends only when their target and UTF-8
  `length_before` match. On mismatch, close the old stream and use bounded
  reconnect for a fresh snapshot rather than racing a REST fetch against it.
- Bounded retry for a reload that briefly receives
  `browser_stream_in_use`.
- Disabled controls and a clear conflict message when another stream remains.
- Stop, tab-close/disconnect expiry, and return-to-lobby behavior; there is no
  Close control or unload/beacon close request.
- A normal return link constructed through the URL API from the worker page's
  current URL and snapshot `lobby_port`.
- Safe rendering of all server-provided text as untrusted data.

Until that decision is made, fixture assets used by server tests are not a
product UI and must not grow into an accidental framework choice.
