# Stage 1 of 2: Runnable daemon, SCGI and model discovery

## Assignment

Implement this stage as the first of two sequential Grok coding-agent runs.
Use a fresh context for each stage and continue from the same repository state.
The next assignment is [block2.md](block2.md).

The result of this stage is a POSIX `cha-daemon` that opens an existing vault,
serves `GET /v1/models` through SCGI, validates chat request envelopes and
session tags, and passes the early Linux/nginx smoke test. Chat generation is
implemented in stage 2.

Read [headless.md](headless.md) and the revised
[head-plan.md](head-plan.md). They define the behavior; these stage files group
the work and define handoff points. Preserve the revised decisions, including
macOS development support and blocking `Application::submit()` in stage 2.
Do not restore the earlier Linux-only or asynchronous-submission plan.

The two stages cover the full implementation plan:

| Stage | Implementation-plan coverage | Result |
| --- | --- | --- |
| 1 | Steps 1–3; startup, activation and basic shutdown from step 6; deployment examples and early smoke test from step 7 | A running server with model discovery and tested request parsing |
| 2 | Steps 4–5; active-request shutdown and remaining checks from step 6; final deployment and client validation from step 7 | Complete Chat Completions support and release validation |

This is the smallest useful split around the plan's required server smoke
test before turn implementation. Keep lifecycle, streaming and cancellation
together in stage 2 because they share the same request loop.

## Context budget and starting checks

This stage is scoped for one 500K context window. Aim to use less than 100K for
initial reading and retain at least 200K for implementation, test failures and
review. These are planning budgets, not exact tokenizer measurements. Read
relevant files and functions; do not ingest the entire repository or repeated
build logs.

Start by reading `AGENTS.md`, checking `git status`, and inspecting the files
below. Preserve existing user changes. No daemon implementation existed when
this assignment was written; check the current tree before creating files.

| Read first | Purpose |
| --- | --- |
| `CMakeLists.txt`, `CMakePresets.json`, `src/README.md` | Build conventions and target documentation |
| `src/app/application_config.h` and `.cpp` | Existing config loading, vault selection and desktop bootstrap behavior |
| `src/app/application.h`; relevant construction, bootstrap and shutdown functions in `.cpp` | Application ownership and model listing |
| `src/runtime/protocol.h`, `src/runtime/runtime_settings.h` | Forum summaries, errors and limits |
| `src/util/path_name.h`, `src/util/logging.h`, `src/workspace/builtins.h` | Identifier validation, logging and Entrance identity |
| `tests/support/test_workspace.*`, relevant `tests/app/unit_application_config.cpp` tests | Temporary vault and configuration fixtures |
| Existing POSIX socket fixtures under `tests/support/` | Test conventions, without adding a transport framework |

Use the existing core as a library. Do not add application threads, queues,
database tables, an HTTP server, a new dependency or a second configuration
system. Keep unused settings harmless, with warnings where appropriate.

## Work sequence

### 1. Check the Linux baseline and add build targets

Run implementation-plan step 1 on the supplied Linux test host: configure the
existing CMake project, build `cha_lib`, `cha_tests` and `cha_app_tests`, and run
both test binaries. Record the distribution, compiler and required packages.
Fix only demonstrated portability problems.

The Linux check need not prevent local macOS work in this stage, but it must
pass before the server smoke test. A supplied Linux test host, access to
nginx/systemd, and a disposable configured vault are prerequisites for that
test. If they are unavailable, finish independent local work and report the
specific missing prerequisite; do not declare the stage complete or replace
the Linux test with a macOS result.

Add `cha-daemon` and `cha_daemon_tests` under `if(NOT WIN32)`, with the test
target also under `BUILD_TESTING`. Link the existing `cha_lib` and use its
warning conventions. Compile daemon implementation sources directly into the
test target, excluding `main.cpp`; do not create a new library for this.
Register the tests with the existing CTest/GoogleTest mechanism. Add both
targets to the build-and-test table in `src/README.md`.

### 2. Implement SCGI framing and socket helpers

Create `src/daemon/scgi.h` and `.cpp` as described in plan step 2:

- Parse exactly one netstring header block and its declared body per socket.
  Require `CONTENT_LENGTH` first, `SCGI=1`, `REQUEST_METHOD` and `DOCUMENT_URI`.
  Reject malformed pairs, duplicate required fields, malformed lengths and
  integer overflow. Ignore unrelated variables.
- Bound headers separately, using the plan's suggested 64 KiB cap, and enforce
  the 16 MiB body limit before allocation. Use `400` or `413` where a response
  is possible; drop truncated requests without application calls.
- Handle partial reads/writes, `EINTR` and would-block results. After the full
  request is read, detect a closed peer before dispatch.
- Write CGI status/headers and body bytes, without HTTP chunk framing. Use
  `MSG_NOSIGNAL` as specified in the revised plan and verify compilation on
  both target platforms. A failed write is a disconnect.
- Use `accept()` and `fcntl()` for nonblocking and close-on-exec flags. Do not
  use `accept4()` or socket-type flags. Short `poll()` waits must let the main
  loop notice shutdown during a partial read or blocked write.
- Give each accepted descriptor a scope-bound owner. Keep helpers local and
  small; do not introduce an event framework or request worker.

### 3. Implement model listing, request parsing and tags

Create `src/daemon/openai_adapter.h` and `.cpp`, and
`src/daemon/session_tag.h` and `.cpp`.

Implement `GET /v1/models` using `bootstrap().presentation.forums`, filtering
out `builtin-entrance`. Return the list envelope and each model's `id`,
`object = "model"`, `owned_by = "cha"` and `created = 0`. Dispatch by
`DOCUMENT_URI`; unknown paths and unsupported methods return `404`.

Implement the chat validation and tag selection from plan step 3 as reusable
functions within these files:

- Require an object, string model, nonempty message array and final user
  message. An omitted `stream` means false; a supplied value must be boolean.
- Accept string content or ordered text parts. Ignore non-text parts. Reject
  unusable user text and input exceeding `settings().prompt_limit` before
  anything can create a session. Preserve accepted input bytes.
- Reject unknown models and Entrance with `404`. Ignore sampling fields,
  client tool definitions and other unused request options.
- Format `[//]: # (cha <forum_id>/<session_id>)` followed by a blank line.
  Find tags only on the first text line, after leading whitespace, in
  assistant messages scanned newest first. Validate IDs with
  `is_url_safe_identifier()`.
- Distinguish no assistant messages (new session) from assistant messages
  with no valid tag (`400`, exactly
  `This chat has no CHA session. Start a new chat.`). A newer untagged assistant
  message does not hide an older valid tag. A selected tag for another forum
  returns `400`; do not fall back to an older tag.
- Return a small parsed value containing model, user text, streaming choice
  and optional tagged identity. Do not replay history into the application.
  Checking a tagged session's existence belongs to `open_session()` in stage 2.
- Use one OpenAI error-envelope helper shared by transport-facing handlers.

Keep the executable honest about this intermediate state: after successful
chat preflight, return `501` with a clear OpenAI-style “chat completion is not
implemented” error. Do not create/open a session, submit input or fabricate a
reply. This one temporary branch is removed in stage 2; no feature flag or
permanent configuration is needed. Invalid preflight requests return their
normal validation errors now.

### 4. Make the daemon runnable and stoppable

Implement startup and basic shutdown in `src/daemon/main.cpp` now, so the
early server test uses the real executable:

1. Accept both forms of `--config`. Require an existing configuration and
   database. Use `load_configuration_directory()` and `find_vault()` to build
   `ApplicationCommand`; do not invoke the desktop parser's empty-directory
   bootstrap or create a default vault.
2. Validate activation before `Application::open()` starts threads:
   `LISTEN_PID` equals this PID, `LISTEN_FDS=1`, and fd 3 is a listening Unix
   stream socket. Set close-on-exec and clear the activation environment at
   this point, before concurrent environment readers can exist. There is no
   fallback listener and no `libsystemd` dependency.
3. Initialize diagnostics before opening the application. For a protected
   vault, read the private mode-`0600` `CONFIG_DIR/password` file, stripping
   only one optional terminal line ending. Preserve other whitespace and
   never log the password. An unprotected vault does not require the file.
   Send useful startup errors to stderr and the log when available.
4. Install signal-safe `SIGTERM`/`SIGINT` flag handling with `SA_RESTART`.
   Handlers only set the flag. Check it in the serial accept/read/write loop.
   Application methods run only in ordinary control flow.
5. Process and close one request before accepting another. Keep running when
   idle. On shutdown, call `request_shutdown()` and `join_shutdown()` within
   the existing bounded grace; call `_exit(EXIT_FAILURE)` if joining fails.
   Destroy normally and shut down logging after a successful join.

Keep a simple way for the adapter to observe the stop flag and request process
shutdown in stage 2. Do not build unused lifecycle abstractions in anticipation
of that work.

Add the small test launcher that supplies a listening Unix socket as fd 3,
sets activation variables in the child before exec, and launches the real
binary. Add the invalid-activation executable test now. Reuse this launcher
for the shutdown-during-generation test in stage 2; these are the only two
automated executable-level cases requested by the revised plan.

### 5. Add deployment examples and pass the server smoke test

Create `packaging/linux/cha@.socket`, `cha@.service` and
`nginx.conf.example` using the design. Make the binary installable at the
service's configured path. Keep the files as editable examples.

Preserve `Accept=no`, the per-user socket/account/config paths, socket mode
`0660`, nginx's group and `PrivateTmp=yes`. nginx must keep request buffering
on, response buffering off, incoming request headers out of SCGI, the 16 MiB
body limit and long SCGI read/send timeouts. Use the literal key-to-user map;
the daemon must never authenticate client keys.

On the supplied disposable Linux setup, check unit validity and run
`nginx -t`. Start with the socket active and service stopped. Request
`GET /v1/models` through nginx and confirm activation and correct model JSON.
Check socket group/mode, account permissions on `/var/lib/cha/<user>`, the
private temporary workspace and database lease. The characters and Assistant
in the test vault must use providers with API keys. Do not copy desktop OAuth
tokens or replace a live user vault for this test.

## Validation and exit criteria

Add focused tests under `tests/daemon/`, reusing temporary workspaces and the
existing GoogleTest conventions. Test behaviors rather than helper layout.

- [ ] `cha-daemon` and `cha_daemon_tests` build on macOS and Linux.
- [ ] Linux `cha_tests` and `cha_app_tests` pass.
- [ ] SCGI tests cover fragmented input, malformed headers and lengths,
  integer overflow, both size limits, premature EOF, complete abandoned
  requests, partial writes and closed output peers without process death.
- [ ] Request/tag tests cover content arrays, ignored roles/options, leading
  whitespace, malformed tags, missing tags, newest-match selection, forum
  mismatch and prompt-size boundaries.
- [ ] Models and error envelopes decode correctly; Entrance is absent;
  preflight never submits input or creates a stored session.
- [ ] The real executable rejects invalid activation; local manual checks
  confirm idle and partial-read shutdown.
- [ ] A model-list request passes through real nginx/systemd on Linux after
  the baseline tests pass. Record this evidence before stage 2 starts.
- [ ] Changed code has been reviewed for ownership, error paths, accidental
  extra threads and unintended desktop changes. Required tests pass.

Use `cmake --preset ninja`, a targeted build and `cha_daemon_tests` on macOS;
use the plan's separate `build/linux` directory on Linux. Run existing desktop
tests if shared code changed. Save concise failure summaries rather than
repeated full logs. Do not add live-provider requirements to unit tests.

## Handoff to stage 2

At completion, add a short execution record below and include it in the final
report. This record is the only change to this stage document needed during
implementation. Do not rewrite the design or main plan.

Record the repository revision or exact working-tree state, changed files,
public daemon helper interfaces, test commands and results on both platforms,
Linux smoke-test result and any unresolved failures. Record nonsecret setup
details needed to repeat the server test. Identify the temporary `501` branch
and the process-launch helper. Never include keys or passwords.

Stage 2 starts only after the required checks above pass. It should reuse the
working startup/SCGI/model implementation and complete chat handling in place.

### Execution record

Not run yet.
