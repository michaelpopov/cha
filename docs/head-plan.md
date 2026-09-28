# CHA Headless Daemon — Implementation Plan

September 27, 2026

Implement the design in [headless.md](headless.md) as a POSIX executable,
`cha-daemon`, over the existing application API. Linux is the only deployment
target; the daemon and its tests also build and run on macOS for development.
Complete steps 2 to 7 in order. Step 1 is a server check: it can run in
parallel with steps 2 and 3, and it must pass before the server smoke test at
the end of step 3. Each step includes a check that must pass before the next
step depends on it. This document is a plan; the Linux build and deployment
checks have not yet been run.

## Scope and constraints

- One daemon owns one user's existing vault and handles one SCGI connection at
  a time. The kernel socket backlog provides request queuing.
- nginx owns HTTP, TLS, authentication and routing. The daemon receives no
  client API keys and adds no HTTP library.
- Link the existing `cha_lib`. Keep the runtime, provider workers, idle audio
  workers, database schema and database lease unchanged.
- Use only POSIX calls in the daemon code, so that it builds on Linux and
  macOS. Deploy it only on Linux.
- Add only `GET /v1/models` and `POST /v1/chat/completions`. Preserve the existing
  parser for ordinary text, mentions, `/mcast` and `@-`.
- Do not add session hashes, API history storage, turn deletion, rollback,
  configuration APIs, OAuth login, media endpoints or idle exit.
- Keep changes outside the new daemon code limited to build wiring and fixes
  demonstrated by the Linux build. Do not add a general server framework or
  split `cha_lib` into new libraries.
- Unused or obsolete configuration must not prevent an otherwise valid
  operation. Retain the existing warning behavior.

## Existing code to reuse

| Requirement | Existing implementation |
| --- | --- |
| Load configuration and select a vault | `src/app/application_config.*`: `load_configuration_directory()`, `find_vault()`, `ApplicationCommand` |
| Open the vault and own the runtime | `src/app/application.*`: `Application::open()`, `context_epoch()`, `bootstrap()` |
| Create and select sessions | `Application::create_session()` and `open_session()`; the latter calls the runtime's `select()` |
| Submit raw input and cancel | `Application::submit()` and `stop()`; `RawCommand` uses `src/runtime/text_input.cpp` |
| Subscribe and consume output | `Application::subscribe()` returns `SubscribeResult::session`; `LiveSession::take_output()` and `acknowledge_output()` consume its output |
| Transcript identity and entry kinds | `src/chat/transcript.h` and `src/runtime/protocol.h` |
| Input bound and shutdown grace | `Application::settings()` / `src/runtime/runtime_settings.h` |
| Diagnostics and shutdown | `src/util/logging.*`, `Application::request_shutdown()` and `join_shutdown()` |
| Local test vault and provider | `tests/support/test_workspace.*`, the existing `mode = "test"` provider, and provider test servers |

Three details need explicit treatment when applying the design to this code:

1. **`clear_input` alone does not mean that input was accepted.** Unknown slash
   commands and invalid multicast input can return `clear_input = true` without
   creating a turn. Require an accepted `CommandResult` with
   `session.input_consumed == true` as well. Otherwise return `400` with its
   notice. Keep desktop editor behavior unchanged.
2. **Submission can wait for Jev.** Use the blocking `Application::submit()`,
   as the design says. It applies the existing command deadline, and it
   normally waits only for Jev, which has a five-second limit. Check the
   connection and the stop flag after it returns. If it returns
   `command_timeout`, the command can still run later, so end the daemon
   through bounded application shutdown. Do not send SSE headers before
   acceptance.
3. **The desktop command parser can create a vault.**
   `parse_application_command()` initializes an empty configuration directory.
   The daemon must require an existing configuration and database. Parse its
   small CLI locally, then use `load_configuration_directory()` and
   `find_vault()` to construct `ApplicationCommand` without that bootstrap step.

## 1. Establish the Linux build

On the intended server distribution, install the compiler and tools required
by the existing CMake build. Check the C++20 compiler, CMake, Ninja, make,
OpenSSL development files and the SQLCipher amalgamation generation step.
Continue to use the dependency versions already selected by CMake.

```sh
cmake -S . -B build/linux -G Ninja -DBUILD_TESTING=ON
cmake --build build/linux --target cha_lib cha_tests cha_app_tests
./build/linux/cha_tests
./build/linux/cha_app_tests
```

Fix only observed Linux compile, link or test failures. Pay attention to POSIX
socket test fixtures, private filesystem permissions, database leases and curl
with OpenSSL. No frontend assets or native desktop host should be needed for
these targets.

**Completion check:** both test binaries pass on the target distribution.
Record the distribution, compiler and required packages with the implementation
validation results. This step does not block daemon development on macOS, but
it must pass before the server smoke test at the end of step 3.

## 2. Add the daemon target and SCGI transport

Add `cha-daemon` under an `if(NOT WIN32)` condition in `CMakeLists.txt`, as
the existing POSIX test fixtures do. Link `cha_lib`, use the existing compiler
warnings, and keep daemon sources out of the desktop library. Add a
`cha_daemon_tests` target under `BUILD_TESTING` and the same condition, using
GoogleTest and the daemon source files directly, excluding `main.cpp`. Put
tests in `tests/daemon/` and reuse `tests/support/test_workspace.cpp` for
application fixtures. Do not create a new library just to share these few
files with tests. Add both targets to the "Build and test map" in
`src/README.md`.

Implement `src/daemon/scgi.h` and `scgi.cpp`:

- Read one request per accepted socket. Handle partial reads and interrupted
  system calls. Parse the decimal netstring length, colon, NUL-separated
  name/value pairs, trailing comma, and exactly `CONTENT_LENGTH` body bytes.
- Require `CONTENT_LENGTH` first, `SCGI=1`, and the method and document URI.
  Reject malformed lengths, integer overflow, broken pairs and duplicate
  required fields. Ignore unrelated SCGI variables.
- Bound header storage separately from body storage; use a small fixed header
  cap, such as 64 KiB. Check the body length against 16 MiB before allocation.
  Use `400` for malformed framing and `413` for excessive size when a response
  can still be written. Drop incomplete requests without application work.
- After reading a complete request, check for peer closure before dispatch.
  Implement the design's nonblocking peek and distinguish a closed peer from
  `EAGAIN` or `EWOULDBLOCK`, which means it is still connected.
- Write CGI headers and response bytes with a partial-write loop and
  `MSG_NOSIGNAL`. A failed write is a disconnect. Never emit HTTP chunk framing.
- Keep socket reads and writes interruptible, using nonblocking I/O and short
  `poll()` waits where needed. A partial request or blocked output must not
  prevent the main loop from noticing shutdown. This remains one synchronous
  request handler, with no request queue or transport thread.
- Use only POSIX socket calls. Call `accept()` and set `O_NONBLOCK` and
  `FD_CLOEXEC` with `fcntl()`; do not use `accept4()` or socket type flags.
  `MSG_NOSIGNAL` is available on Linux and macOS.
- Give each accepted descriptor one scope-bound owner so all exit paths close
  it. Keep framing and socket helpers small and local to this transport.

**Completion check:** socket-pair tests cover fragmented headers and bodies,
bad framing, overflow, both size limits, premature EOF, a complete but abandoned
request, partial writes and a closed output peer. A broken connection must not
terminate the test process or affect the next request.

## 3. Implement request validation and session tags

Add `src/daemon/session_tag.h` and `session_tag.cpp` for formatting and finding
tags. Keep JSON endpoint handling in `openai_adapter.h` and
`openai_adapter.cpp`, using the existing `nlohmann::json` dependency.

Dispatch only the two method/path pairs in the design. Return `404` for other
paths or unsupported methods. Use `DOCUMENT_URI` for dispatch. For model
listing, use `bootstrap().presentation.forums`, exclude `builtin-entrance`, and
return the OpenAI list envelope. Each entry has `id`, `object = "model"`,
`owned_by = "cha"` and a stable `created` value; use `0` because the current
forum summary has no creation timestamp.

For chat requests:

1. Require a JSON object, a string `model`, a nonempty `messages` array and a
   final message whose role is `user`. Default omitted `stream` to `false` and
   require a boolean if supplied.
2. Accept string content and concatenate ordered `text` parts from array
   content. Ignore non-text parts; do not fetch images or other media. Reject a
   final user message with no usable text. This is the text-only policy for
   clients that include image parts in their requests.
3. Check the extracted input's byte length against
   `application.settings().prompt_limit` (currently 32 KiB), before creating a
   session. Pass accepted text unchanged to `RawCommand`.
4. Resolve `model` against the exposed forums. Unknown forums and Entrance
   return `404` before session creation.
5. Inspect assistant messages newest first. After leading whitespace, match
   only the first line against `[//]: # (cha <forum_id>/<session_id>)`.
   Validate the identifier boundaries and both IDs with the existing
   `is_url_safe_identifier()` helper in `src/util/path_name.h`.
   Concatenate assistant text parts in order before checking the line.
6. The newest matching tag wins, even if a newer assistant message has no tag.
   Never inspect user, system, developer or tool content for tags. If there
   are no assistant messages, create a new session. If there are assistant
   messages but no matching tag, return `400` with exactly
   `This chat has no CHA session. Start a new chat.`
7. Return `400` for a tag naming a different forum, or `404` if its session
   does not exist. Do not fall back to an older tag after selecting one.

Ignore sampling fields, client tool definitions and other unused options. Send
one choice per request. Do not replay client history or system instructions
into CHA; the saved transcript and forum configuration provide model context.

Use one error helper for `{"error":{"message":...,"type":...,"code":...}}`.
Keep validation failures (`400`), absence (`404`), provider errors (`502`) and
storage/internal failures (`500`) distinct. Preserve useful CHA notices and
`CommandFailure` messages without exposing secrets or raw requests.

**Completion check:** table-driven tests cover the session-matching table in
the design, role filtering, leading whitespace, text arrays, malformed tags,
validation boundaries and ignored options. Test that a pasted user-side tag
starts a separate session and that invalid requests do not submit input.

**Server smoke test.** Before step 4, find deployment problems while little
code depends on them. Write the first part of `main.cpp` from step 6:
configuration loading, the activation checks and the accept loop. Create the
unit files and the nginx example from step 7. Install them on a server where
step 1 passed, and get the model list once through nginx. Check the socket
group and mode, the account permissions on `/var/lib/cha/<user>`,
`PrivateTmp` and the lease file. Confirm that the characters and the
Assistant in the test vault use providers with API keys.

## 4. Connect requests to the application lifecycle

Implement one request-local turn handler in `openai_adapter.cpp`. Keep only the
current request's subscription, transcript entries and output text in memory.

1. Capture the application's current nonzero context epoch. Create a session
   with an empty label when required, then call `open_session()` in both the
   new and existing session cases.
2. Subscribe with request-specific connection and subscription IDs and that
   epoch. Retain `SubscribeResult::session`; no runtime internals or controller
   pointers are needed.
3. Take the initial snapshot, record its highest entry ID (zero if empty),
   and acknowledge it before submitting input. The baseline must precede the
   turn, including when a provider finishes immediately.
4. Submit `RawCommand` with the blocking `Application::submit()`. After it
   returns, check the connection and the stop flag. Interpret the result using
   the acceptance rule above. Return validation notices as `400`; map
   operational failures to server errors. If it returns `command_timeout`, the
   command can still run later, so end the daemon through bounded application
   shutdown (item 8).
5. After acceptance, start the selected response mode and consume output.
   Track only entries whose IDs exceed the baseline. Replace their state on
   snapshots, append `EntryTextTarget` text to the matching entry, and ignore
   `ReasoningTextTarget`. Acknowledge every consumed item, including ignored
   items and items consumed after a disconnect.
6. Normal completion requires a snapshot with `generation.active == false`
   and at least one new entry. Do not finish between multicast recipients or
   while Jev is pending. Treat new `EntryKind::error` entries as provider
   failures at turn end. Do not mistake naming or other notices for replies.
7. If the connection closes or a write fails, call `stop()` once and drain
   without further writes until the session is idle or terminal.
8. Handle terminal lifecycle snapshots and closed output explicitly. A storage
   failure or stopped session must not leave the loop waiting for an ordinary
   successful turn. If cleanup cannot establish an idle or terminal session,
   end the daemon through bounded application shutdown instead of accepting
   another request against unresolved work.
9. Unsubscribe on every path after successful subscription, using the same
   IDs and epoch. Use a small scope guard for cleanup. Do not close the selected
   session after a normal request.
10. If this request created the session and CHA rejected the input, call
    `delete_session()` after `unsubscribe()`. `handle_text_input()` keeps a
    session before it parses the input, so startup pruning does not remove
    it.

Use `open_session()` selection and its existing retirement policy. Do not use
`LiveSessionManager::open()` or retain a growing list of live sessions. A reused
live controller retains Jev's target; a controller reopened after retirement
starts from the forum default. Do not write `default_character` from the API.

**Completion check:** application tests with a temporary database cover a new
chat, continuation, `@Name`, `/mcast`, `@-`, invalid commands and mentions, Jev
acceptance/rejection, provider failure, and cancellation after a turn starts.
Alternate among more than eight stored sessions to check retirement. Assert
that the next request runs after cancellation, that a rejected first message
leaves no session, and that neither session tags nor client history are added
to the transcript.

## 5. Format streaming and complete responses

Use the same turn renderer for both response modes. Render only character
entries as `**Name:** text`, with blank lines between replies in transcript
order. Use each entry's display name. If a completed turn contains only its
human entry, return `(recorded)`. Keep reasoning and notices out of assistant
content; report error entries through the error path.

For streaming:

- After acceptance, send `Status: 200 OK` and `Content-Type: text/event-stream`,
  followed immediately by a chunk with `delta.role = "assistant"` and the tag
  line plus a blank line in `delta.content`.
- Use one completion ID, creation timestamp and model ID throughout the
  response. Each chunk has `object = "chat.completion.chunk"` and choice index
  zero. Use normal JSON escaping for all content.
- Track emitted text per reply, excluding the session tag. Send appends once.
  On a snapshot, send only the suffix beyond the already-emitted prefix. If a
  reply no longer has that prefix, log once and stop emitting that reply;
  continue consuming output and allow later multicast replies. Never resend
  or attempt to retract text. Snapshots can rewrite text through the existing
  presentation code, so this requires a regression test.
- Use a monotonic timer to send `: keepalive\n\n` after 15 seconds with no text,
  and repeat during quiet generation. This starts after SSE headers; waiting
  for submission acceptance still permits an ordinary JSON error.
- On success, send an empty delta with `finish_reason = "stop"`, then
  `data: [DONE]\n\n`. On provider or internal failure after headers, send the
  OpenAI error envelope as the final SSE event and close without a success
  chunk or `[DONE]`.

For non-streaming, continue checking disconnects while collecting output.
After successful completion return one `chat.completion` with the tag and the
complete final text in `choices[0].message.content`, role `assistant`, and
`finish_reason = "stop"`. A provider failure returns `502`, and a storage
failure returns `500`, before any success headers. Do not invent token usage
for a turn that may involve several providers.

**Completion check:** decode responses as a client would. Compare successful
streamed content with non-streaming content for ordinary and multicast turns.
Cover snapshot coalescing, duplicate snapshots, prefix mismatch, reasoning-only
periods, keepalives, error framing and partial replies after cancellation.
Use a controllable time value for the keepalive test, without adding production
timer machinery or a 15-second test sleep.

## 6. Implement startup, socket activation and shutdown

Complete `src/daemon/main.cpp`:

- Accept `--config DIR` and `--config=DIR`, with daemon-specific usage and
  errors. Load the existing selected vault as described above. Do not create
  a configuration directory, default vault or server-side login flow.
- Validate systemd activation first, before `Application::open()` starts
  threads: `LISTEN_PID` names this process, `LISTEN_FDS` is exactly one, and
  fd 3 is a listening Unix stream socket. Mark inherited descriptors
  close-on-exec and clear the activation environment at this point. Provider
  threads read the environment during curl and DNS calls, and `unsetenv()` is
  not safe while another thread reads it. Fail clearly if activation is
  invalid. Do not bind a fallback socket or introduce a `libsystemd`
  dependency for these checks.
- Initialize diagnostic logging before opening `Application`. Report startup
  failures to stderr for the systemd journal as well as the log where available.
- For a protected vault only, read `CONFIG_DIR/password`. Require a private
  file with mode `0600`; report missing, unreadable or invalid password files
  without logging their contents. Define the file as a password optionally
  followed by one line ending, preserving other whitespace. Pass it to
  `Application::open()`. An unprotected vault does not depend on this file.
- Run the serial accept/read/dispatch/close loop. The next request is not
  accepted until the current handler and cancellation cleanup finish.
- Install a signal-safe stop flag for `SIGTERM` (and `SIGINT` for local tests)
  with `SA_RESTART`. The kernel can deliver the signal to any thread;
  `SA_RESTART` restarts system calls that it interrupts in curl, SQLite or the
  log writer. The short `poll()` waits still let the main loop see the flag.
  Check it in accept/read/write waits, after `submit()` returns and in
  generation polling. Call application methods only from normal control flow,
  never from the signal handler.
- On shutdown, stop accepting, cancel current work and perform cleanup within
  one bounded shutdown budget. Call `request_shutdown()` and
  `join_shutdown()` with the remaining grace. If joining fails, log and call
  `_exit(EXIT_FAILURE)`, as the macOS host does. Finish normal destruction and
  logging shutdown after a successful join.

**Completion check:** launch the real executable from a small test helper that
supplies a temporary listening socket as fd 3 and sets activation variables in
the child before exec. Automate only two cases: invalid activation, and
shutdown during generation. Check the other cases once by hand in step 7.

## 7. Add deployment files and verify through nginx

Finish the template units and the nginx example under `packaging/linux/` that
the server smoke test in step 3 started:

- `cha@.socket`: `/run/cha/%i.sock`, `Accept=no`, mode `0660`, nginx's group.
- `cha@.service`: the per-user account, configured executable path,
  `--config /var/lib/cha/%i/config`, and `PrivateTmp=yes`.
- `nginx.conf.example`: literal key-to-user map, 16 MiB body limit, SCGI header
  forwarding disabled, response buffering disabled and long read/send timeouts.

Keep these as editable examples; do not add an account provisioning framework
or a package manager integration. Make the binary installable to the path used
by the unit. Check the actual distribution's nginx group, `scgi_params`, socket
directory permissions and service stop timeout when installing the examples.

On a disposable Linux deployment, run `nginx -t` and verify the units before
enabling two test users. Exercise the public API through nginx:

| Scenario | Required result |
| --- | --- |
| First request while service is stopped | Socket activation starts one daemon and returns the response |
| Model listing | All configured forums except Entrance; valid OpenAI list envelope |
| Streaming and non-streaming chat | Correct tag, labels, continuation and completion framing |
| Second request during a slow turn | Waits behind the first request, including model listing |
| Queued client disconnects, with small and large bodies | Abandoned request creates no turn |
| Active client disconnects | Current generation is cancelled; later requests succeed |
| No or unknown API key | nginx returns `401`; daemon receives no authorization header |
| Oversized request | nginx returns `413`; direct SCGI also enforces the body limit |
| One user's tag sent to the other user's daemon | It cannot reach the first user's vault |
| Provider failure or daemon startup failure | Correct API error or nginx `502`; no false success |
| Protected and unprotected vaults | Both start; a missing or wrong `config/password` gives a clear startup error |
| Second process on the same vault | The database lease stops it with a clear error |
| Stop while idle or while reading a partial request | Clean exit within the stop timeout; no turn |
| Service crash and restart | Socket activation recovers; stored sessions remain usable |
| Stop both units and restart | Clean shutdown, lease release, temporary workspace cleanup and session continuation |

Finally, test one intended OpenAI-compatible chat client with its automatic
title, tag and follow-up requests disabled. Check that it preserves the tag in
assistant history, displays labeled replies, handles SSE comments, and can
continue after a stopped reply. Verify that regenerate/edit adds a turn and
does not remove prior stored turns; editing the first turn with no assistant
tag creates a new session according to the matching rules.

**Completion check:** the daemon tests pass on macOS and Linux, and the Linux
core/application tests pass; the nginx/systemd checks pass with both users;
the desktop test suites still pass if shared code changed. Confirm that the
application still has its existing threads, with no daemon worker pool, and
that unused audio workers perform no downloads. No design open question is
required for this release.
