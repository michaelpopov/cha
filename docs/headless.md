# CHA Headless Daemon — Design

Sep 27, 2026 · @Michael Popov

## Overview

CHA gets a headless Linux variant, `cha-daemon`, that any OpenAI-compatible client can use as a model provider. Each forum appears as a model. Each user has one vault, one existing SQLite file and one daemon process.

Goals:

- Reach all existing CHA functionality (forums, characters, `@Name`, `/mcast`, stored sessions) from standard chat clients.
- Keep HTTP out of the application. nginx handles HTTP, TLS and authentication.
- Add no concurrency to the application. One process serves one user, and it handles one request at a time.
- Reuse the existing core (`app/`, `runtime/`, `session/`, `storage/`, `providers/`) without changes to its threading model.

Non-goals:

- Sharing one vault or one session between users.
- The Responses API, embeddings, images, audio and client-side tool calls.
- Vault administration over the API (switching, merge, backup, R2 transfer, configuration edits).
- Voice and media features.
- Deleting request/response pairs from a session, including rollback on regenerate or edit. This functionality is not supported by the headless daemon.

## Architecture

nginx is the only HTTP component. It maps the API key to a user and forwards the request over SCGI to that user's Unix socket. systemd owns each socket and starts the user's daemon on the first connection.

&#91;embedded content: cha-daemon deployment · nginx, systemd, one daemon per user\]

Only the SCGI loop and the OpenAI adapter are new code. The CHA core keeps its current threads: one runtime thread, provider workers and three audio download workers, which stay idle.

## Request flow

A chat request passes through nginx and systemd to exactly one daemon, which handles it to completion before it accepts the next one.

1. The client sends `POST /v1/chat/completions` with `Authorization: Bearer <key>`, `model` = forum ID, the full `messages` history and usually `stream: true`.
2. nginx maps the key to a user name. An unknown key gets `401`. nginx reads the whole request body, then connects to `unix:/run/cha/<user>.sock`.
3. If the user's daemon is not running, systemd starts `cha@<user>.service` and passes it the listening socket as file descriptor 3. The connection waits in the socket backlog meanwhile.
4. The daemon accepts the connection. If it is busy with another request, the connection waits in the backlog until that request ends.
5. The SCGI loop reads the header block and the body. If nginx closed the connection in the meantime, the loop drops the request. Otherwise it gives the request to the OpenAI adapter.
6. The adapter resolves the forum from `model`. It reads the session tag from the newest `assistant` message and opens that session. If the history has no `assistant` message, it creates a new session (see Session matching).
7. The adapter submits the last user message as session input (see Turn lifecycle). It goes through the same input parser as GUI input, so `@Name`, `/mcast` and `@-` work. If CHA rejects the input, the adapter returns `400`.
8. When CHA accepts the input, the adapter sends the SSE headers and a first chunk with the session tag. Then it streams the session output as SSE chunks until the turn ends, and sends `data: [DONE]`.
9. The adapter closes the socket, and the loop accepts the next connection.

## Authentication and routing

nginx identifies the user from the API key and routes to that user's socket. The daemon never sees or checks keys.

OpenAI clients send the key as `Authorization: Bearer <key>`. A `map` turns the full header value into a user name, and the user name becomes part of the socket path:

```nginx
map $http_authorization $cha_user {
    "Bearer <alice-key>"  alice;
    "Bearer <bob-key>"    bob;
    default               "";
}

server {
    listen 443 ssl;
    # ssl_certificate, ssl_certificate_key ...

    location /v1/ {
        if ($cha_user = "") { return 401; }
        client_max_body_size      16m;     # long histories and base64 image parts
        include                   scgi_params;
        scgi_pass                 unix:/run/cha/$cha_user.sock;
        scgi_pass_request_headers off;     # keep the API key out of the daemon
        scgi_buffering            off;     # stream SSE chunks as they arrive
        scgi_read_timeout         3600s;   # wait while queued behind another request
        scgi_send_timeout         3600s;   # large bodies can block while queued
    }
}
```

- The socket path comes only from the `map`, never from request data, so a client cannot reach another user's socket.
- nginx compares literal `map` strings without case. API keys are lowercase hex (`openssl rand -hex 32`), so case has no effect.
- `scgi_pass_request_headers off` stops nginx from sending the HTTP headers, including `Authorization`, to the daemon. The daemon needs only `REQUEST_METHOD` and `DOCUMENT_URI` from `scgi_params`, and `CONTENT_LENGTH`, which nginx always sends.
- `client_max_body_size` is 1 MiB by default. Long histories, and images that clients send as base64 parts, can be larger. A body above 16 MiB gets `413` from nginx.
- `scgi_request_buffering` stays at its default (`on`). The daemon receives the complete body at once.
- A user with no socket unit gets `502` from nginx.
- The `model` field is not used for routing. The daemon reads it to select the forum.

## Process lifecycle

systemd socket activation starts each user's daemon on the first connection. One pair of template units serves all users; `%i` is the user name.

`/etc/systemd/system/cha@.socket`:

```ini
[Socket]
ListenStream=/run/cha/%i.sock
SocketGroup=www-data
SocketMode=0660
Accept=no

[Install]
WantedBy=sockets.target
```

`/etc/systemd/system/cha@.service`:

```ini
[Service]
ExecStart=/usr/local/bin/cha-daemon --config /var/lib/cha/%i/config
User=cha-%i
PrivateTmp=yes
```

- **Socket ownership.** systemd creates the socket and keeps it open even while no daemon runs. `Accept=no` gives the one listening socket to one service instance.
- **Accounts.** Each daemon runs as its own system account, `cha-<user>`. This account owns only `/var/lib/cha/<user>/`, so one daemon cannot read the vault, password or logs of another user.
- **Private `/tmp`.** CHA writes the materialized workspace, with the provider API keys as plain TOML, to the temporary directory. `PrivateTmp=yes` gives each instance its own `/tmp`, and systemd removes it when the instance stops. A crash therefore leaves no key files behind.
- **Start.** On the first connection, systemd starts the service with `LISTEN_FDS=1`. The daemon uses file descriptor 3 as its listening socket and does not bind one itself.
- **Crash.** If the daemon exits, the socket stays open. The next connection starts a new daemon.
- **Stop.** On `SIGTERM` the daemon stops accepting, finishes or cancels the current request, and runs the existing application shutdown.
- **No idle exit.** The daemon keeps running after its first start. Three or four idle daemons use little memory, and each start opens the vault, writes the workspace to `/tmp` and validates every forum.
- **One daemon per file.** The existing SQLite database lease stops a second process from opening the same vault.

## SCGI bridge and serialization

The daemon handles one SCGI connection at a time. Serialization comes from the accept loop itself, so the application adds no locks, queues or threads.

**Request format.** Each request arrives on its own connection:

```text
<len>:CONTENT_LENGTH\0<n>\0SCGI\01\0REQUEST_METHOD\0POST\0DOCUMENT_URI\0/v1/chat/completions\0...,<n bytes of JSON body>
```

The header block is a netstring of NUL-separated name/value pairs, with `CONTENT_LENGTH` first. The body of `CONTENT_LENGTH` bytes follows the comma.

**Response format.** CGI-style headers, a blank line, then the body. Closing the socket ends the response.

```text
Status: 200 OK\r\n
Content-Type: text/event-stream\r\n
\r\n
data: {...}\n\n
data: {...}\n\n
data: [DONE]\n\n
```

**Accept loop.**

```text
listen_fd = 3                                // from systemd
loop:
    fd = accept(listen_fd)
    request = read_scgi(fd)                  // headers + full body
    if request is incomplete: close(fd); continue
    if peer_closed(fd): close(fd); continue  // client left while queued
    handle(request, fd)                      // adapter writes to fd
    close(fd)
```

- While one request runs, new connections wait in the kernel backlog. nginx waits with them, which is why both nginx timeouts are long.
- nginx cannot do this queuing itself. `max_conns=1` without the commercial `queue` directive returns `502` instead of waiting.
- nginx writes the complete request into the socket before it waits. A typical request fits in the socket buffer, so a request whose client left while queued is often complete. When the client leaves, nginx closes the connection (`scgi_ignore_client_abort` is `off` by default). `peer_closed(fd)` finds this: `recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT)` returns 0.
- `read_scgi` refuses a `CONTENT_LENGTH` above 16 MiB with `413` before it allocates memory. This is the same limit as in nginx.
- All writes use `send(..., MSG_NOSIGNAL)`, so a closed connection gives `EPIPE` instead of killing the process.
- The SCGI code needs no library.

## OpenAI API surface

The daemon implements two Chat Completions endpoints. Every other path returns `404`.

| Endpoint | Behavior |
| --- | --- |
| `GET /v1/models` | One entry per forum in the vault, except the built-in Entrance forum (`builtin-entrance`): `id` = forum ID, `object` = `model`, `owned_by` = `cha`. |
| `POST /v1/chat/completions` | Runs one turn in the matched session. Streams when `stream` is `true`, else returns one `chat.completion` object. |

**Request fields.**

| Field | Handling |
| --- | --- |
| `model` | Forum ID. An unknown forum, or `builtin-entrance`, returns `404` with an OpenAI-style error body. |
| `messages` | The session tag in the newest `assistant` entry selects the session; the last `user` entry is the new input. |
| `stream` | Selects SSE or a single JSON response. |
| `temperature`, `max_tokens`, `top_p` and similar | Ignored. Character configuration decides provider settings. |
| `tools`, `tool_choice` | Ignored. |

**Streaming.** Each chunk is a `chat.completion.chunk` with `choices[0].delta.content`. The first chunk carries `delta.role = "assistant"` and the session tag line. The adapter sends it as soon as CHA accepts the input. The last chunk carries `finish_reason = "stop"`.

If the adapter sends no text for 15 seconds, it writes an SSE comment line (`: keepalive`). SSE clients ignore comment lines. This stops client idle timeouts during long reasoning, Jev classification or web search.

`SessionOutput` usually delivers text appends, but it can fall back to a full snapshot. The adapter keeps the reply text it has already sent for the turn, without the tag line. On a snapshot, it sends only the part after that prefix. If the snapshot does not start with that prefix, it logs the case and sends nothing more for that part, because SSE cannot retract text.

**Non-streaming.** The adapter waits for the turn to finish. It returns the session tag line and the complete assistant text as `choices[0].message.content`.

## Session matching

Each assistant reply carries a session tag. The client keeps the tag in its history, and the adapter reads it back on the next request. The daemon stores no extra data and computes no hashes.

**Tag format.** The first line of every assistant reply is the session tag, followed by a blank line:

```text
[//]: # (cha <forum_id>/<session_id>)
```

- The line is a Markdown link reference definition. Markdown renderers do not show it.
- Forum and session IDs are URL-safe identifiers. They contain no spaces, parentheses or `/`, so the tag needs no escaping.
- The tag comes before the speaker label. The adapter sends it in the first chunk, as soon as CHA accepts the input. A stopped or failed reply therefore still carries it.
- The tag is part of the API output only. CHA does not store it in the transcript.
- The tag costs no model tokens. CHA builds the model context from its own transcript, not from the client history.

**Lookup.** The adapter examines the `assistant` messages in `messages`, newest first. For each message, it compares the first line of the text with the tag format and ignores leading whitespace. For array content, it uses the `text` parts. The first match selects the session. The adapter ignores session tags in other roles.

| Case | Action |
| --- | --- |
| No `assistant` message | Create a new session in the forum. |
| `assistant` messages, but none has a session tag | Return `400` with the error message `This chat has no CHA session. Start a new chat.` |
| The tag names a forum other than `model` | Return `400`. The user changed the model in the middle of the chat. |
| The tagged session does not exist | Return `404`. |
| The tagged session exists | Continue that session. |

**Regenerate and edit.** The client resends its history up to the changed message. The newest remaining `assistant` message still carries the tag, so the daemon continues the same session and adds a new turn. The replaced reply stays in the CHA transcript and in the model context.

**Security.** The tag is not a secret. nginx sends each API key only to its own user's daemon, so a tag cannot reach the sessions of another user.

**Side requests.** Some clients send extra requests for titles, chat tags or follow-up suggestions, with the chat pasted into one user message. The adapter reads session tags only from `assistant` messages, so these requests cannot reach an existing session. Each one still starts a new, unused session. Such clients must disable these features, or route them to a different provider.

## Turn lifecycle

The adapter runs each turn through the existing `Application` API. `SessionOutput` has no blocking wait, so the adapter polls it. The desktop host polls it in the same way, with a 50 ms wait.

1. For a new session, call `create_session()`. Then call `open_session()` and `subscribe()`. Record the highest entry ID in the first snapshot.
2. Check the input against `prompt_limit` (32 KiB). Today only the bridge checks this limit, and the daemon does not use the bridge.
3. Call `submit()` with the text. CHA accepted the input only if the result is a `CommandResult` with `session.input_consumed == true`. `clear_input` alone is not enough: an unknown `/` command and a failed `/mcast` set it, but no turn starts. If CHA rejects the input, return `400` with `session.notice` or the failure message as the error message. If `submit()` returns `command_timeout`, the command can still run later, so the daemon shuts down.
4. Every 25 ms, check the connection with `peer_closed(fd)` and call `take_output()`. Keep the text of each entry after the recorded ID:
   - A snapshot replaces that text.
   - An `EntryTextTarget` append extends it.
   - The adapter ignores `ReasoningTextTarget` appends.

   Send the new text, then call `acknowledge_output()`.
5. The turn ends at the first snapshot that has `generation.active == false` and an entry after the recorded ID. `generation.active` stays true through Jev and through all `/mcast` targets.
   - If the only new entry is the human entry, the input was `@-`: send `(recorded)`.
   - If the turn has an entry of kind `error`, send the error event (streaming) or return `502` (non-streaming) when the turn ends.
6. If the client disconnects, call `stop()`. Continue the loop without writes until the turn ends. The next request therefore never finds the session busy.
7. Call `unsubscribe()`. If this request created the session and CHA rejected the input, delete that session. CHA keeps a session before it parses the input, so startup pruning does not remove it.

## Forum features over the protocol

The last user message goes through the existing input parser unchanged. Every turn returns exactly one assistant message, because the protocol has one speaker per reply.

| Input | CHA behavior | Returned assistant message |
| --- | --- | --- |
| Plain text | The session's current target character replies. A new session starts with the forum's `default_character`. If the vault configures Jev, Jev can choose another character or all characters. | `**Name:** reply`, or several labeled replies when Jev chooses all characters. |
| `@Name text` | The addressed character replies. | `**Name:** reply` |
| `/mcast ...` | Several characters reply in foreground order. | Each reply as `**Name:** reply`, separated by a blank line, streamed in order. |
| `@- text` | Recorded in the transcript. No model is called. | The fixed text `(recorded)`. |

- Each returned message in this table starts with the session tag line (see Session matching).
- Every reply carries a speaker label, so the client always shows which character spoke. The adapter renders every turn as a list of labeled replies, for all input types.
- `(recorded)` is not empty on purpose. The session tag line is hidden, so a reply with only the tag would look empty in the client.
- CHA rejects some input without a turn: empty text, a `/` command other than `/mcast`, and an unknown or empty `@Name` prompt. For example, a message that starts with `/etc/hosts` is an unknown command. The daemon returns `400` with the CHA notice.
- Jev also changes the live target for later turns. When a session opens again, its target is the forum's `default_character` again.
- The forum's `default_persona` decides authorship. Clients cannot select a persona.
- The daemon never writes forum configuration. The GUI target selector saves `default_character`; over the API, `@Name` addresses a character for one message instead.
- The process-local Welcome conversation and its built-in Entrance forum are not exposed.

## Changes to the application

The daemon is a new executable over the existing `app/` API. The core layers need no storage or threading changes.

| Area | Change |
| --- | --- |
| `src/daemon/main.cpp` (new) | Parses `--config`, reads `password` from the configuration directory for a protected vault, constructs `Application` with the configured vault, takes fd 3 from `LISTEN_FDS`, handles `SIGTERM`, runs the accept loop. |
| `src/daemon/scgi.*` (new) | Reads the netstring header block and body; writes the status line and headers. |
| `src/daemon/openai_adapter.*` (new) | Maps JSON to application calls: forum listing, `open_session`, `submit`, output subscription; builds chunks and diffs snapshots. |
| `src/daemon/session_tag.*` (new) | Formats the session tag line and finds the newest tag in the `assistant` messages. |
| `CMakeLists.txt` | A `cha-daemon` target for POSIX systems that links `cha_lib` as it is. It builds on macOS for development and is deployed only on Linux. |

- **Linux build.** No Linux build exists yet. Build `cha_lib` on the target distribution and run `cha_tests` and `cha_app_tests`. This check does not block daemon development, because the daemon also builds and runs its tests on macOS. It must pass before the first test on the server. The code has POSIX branches, and CMake selects curl with OpenSSL on Linux.
- **Build.** `cha_lib` is one static library. The linker leaves out the unused `bridge/` objects. The media code stays, because `Application` creates the audio download manager and uses media code at shutdown. Its three workers stay idle, because downloads start only from explicit `start_audio` calls.
- **Session handling.** Each request opens the session with the existing `Application::open_session()`, which calls `select()`. When the next request selects another session, the runtime retires the previous one once it is idle. The last session stays live between requests, which makes its next turn faster. The daemon does not use `LiveSessionManager::open()`: `Application` does not expose it, it never retires sessions, and it fails when 8 sessions are live. No runtime change is needed.
- **Waiting for output.** The adapter polls the session output every 25 ms (see Turn lifecycle). The runtime thread and provider workers keep running as in the GUI.
- **Protected vaults.** The vault database is SQLCipher. When the vault file has `protected = true`, the daemon reads the password from `password` in the configuration directory (mode `0600`). No command-line option or unit change is necessary. The password is next to the vault, so the encryption protects only copies of the vault file, such as backups and R2 uploads. It does not protect data on the server. On the server, an unprotected vault is simpler.
- **Dependencies.** curl, OpenSSL and SQLCipher are already in the build. No HTTP library is added.

## Errors and disconnects

Errors before a turn starts return an HTTP status with the OpenAI error body `{"error": {"message", "type", "code"}}`. Errors after streaming starts are sent as a final SSE event.

| Condition | Where | Response |
| --- | --- | --- |
| Missing or unknown API key | nginx | `401` |
| Request body larger than 16 MiB | nginx | `413` |
| User has no socket, or the daemon fails to start | nginx | `502` |
| Path other than the two endpoints | daemon | `404` |
| Malformed JSON, no `messages`, or last message not `user` | daemon | `400` |
| Unknown forum in `model`, or `builtin-entrance` | daemon | `404` |
| `assistant` messages without a session tag | daemon | `400` |
| Session tag names a forum other than `model` | daemon | `400` |
| Tagged session does not exist | daemon | `404` |
| Input larger than `prompt_limit` (32 KiB) | daemon | `400` |
| Input that CHA rejects | daemon | `400` with the CHA notice |
| Provider fails, non-streaming request | daemon | `502` |
| Provider fails, streaming request | daemon | `data: {"error": {...}}`, then close |
| Storage failure | daemon | `500` |

**Client disconnect while generating.** nginx closes the upstream socket (`scgi_ignore_client_abort` is `off` by default). The output loop finds this with `peer_closed(fd)` on its next pass, or a write fails with `EPIPE`. The adapter cancels the generation, as the GUI Stop action does, and waits until the turn ends. The existing partial-response rules store the partial reply. The partial reply in the client already starts with the session tag, so the client can continue the chat.

**Client disconnect while queued.** nginx writes the complete request before it waits, so the daemon can read a complete request after the client left. `peer_closed(fd)` finds the closed connection after the read, and the daemon closes it without a turn.

**Non-streaming disconnect.** The output loop finds the closed connection with `peer_closed(fd)`, as for streaming. The adapter cancels the generation and waits until the turn ends.

## Operations

Adding a user takes one system account, one configuration directory, one nginx line and one `systemctl` command. No unit file is written per user.

**Per-user layout.**

```text
/var/lib/cha/alice/
├── config/
│   ├── app.toml          # vault = "Alice", [logging]
│   ├── alice.toml        # vault_name = "Alice", data = "/var/lib/cha/alice/workspace.sqlite3"
│   └── password          # only for a protected vault, mode 0600
└── workspace.sqlite3     # the vault file
```

The whole tree belongs to the account `cha-alice`, with mode `0700` on `/var/lib/cha/alice/`. CHA sets mode `0600` on the vault files when it opens them. Provider API keys live in each user's vault, so users do not share provider accounts.

**Adding a user.**

1. Create the account: `useradd --system --shell /usr/sbin/nologin cha-<user>`.
2. Create `/var/lib/cha/<user>/` with the layout above and copy the user's SQLite file into it. Then run `chown -R cha-<user>: /var/lib/cha/<user>` and `chmod 700 /var/lib/cha/<user>`.
3. Generate an API key with `openssl rand -hex 32` and give it to the user.
4. Add `"Bearer <key>" <user>;` to the nginx `map`, then run `nginx -s reload`.
5. Run `systemctl enable --now cha@<user>.socket`.

After step 2, the server copy is the only home of the API sessions. Do not copy a newer desktop vault over it, because that deletes all API sessions.

**Provider access.** A server vault must use providers with API keys. This includes the provider that names new sessions, which is the Assistant's provider by default. ChatGPT subscription providers read `openai-auth.json` from the configuration directory, and the daemon has no login flow. Do not copy `openai-auth.json` from a desktop. Each refresh saves a new refresh token, and OpenAI can reject the old token that the other copy still uses.

**Changing configuration.** The daemon does not write configuration. To change characters, forums, prompts or keys:

1. Stop both units: `systemctl stop cha@<user>.socket cha@<user>.service`. Stop the socket too, because it starts a stopped service again.
2. Copy `workspace.sqlite3` to a desktop, with its `-wal` file if one exists.
3. Open the copy in the desktop app as a separate vault, make the changes, and close the vault.
4. Copy the main file back, and delete any `workspace.sqlite3-wal` and `workspace.sqlite3-shm` files on the server. Run `chown cha-<user>: /var/lib/cha/<user>/workspace.sqlite3`, then `systemctl start cha@<user>.socket`.

**Backups.** Copy the vault only while both units are stopped, as in steps 1 and 2 above. A copy of a live WAL database is not safe.

**Startup failures.** If the daemon fails at startup, for example because of a wrong password or a bad configuration, each new request starts it again. After a few failures, systemd reaches the start limit and also stops the socket, and nginx returns `502`. After the repair, run `systemctl reset-failed cha@<user>.socket cha@<user>.service` and `systemctl start cha@<user>.socket`.

**Removing a user.** Run `systemctl disable --now cha@<user>.socket cha@<user>.service`, remove the `map` line, reload nginx, and run `userdel cha-<user>`. Keep or archive `/var/lib/cha/<user>/`.

**Logs.** Each daemon writes its own log file under its config directory, as set in `app.toml`. systemd records start, stop and crash events in the journal for `cha@<user>.service`.

## Limitations and open questions

The design trades throughput and flexibility for a daemon with no concurrency and no HTTP code.

**Limitations.**

- One request at a time per user. A second chat from the same user waits until the first turn ends.
- `GET /v1/models` also waits behind a running turn. Some clients load the model list when a page opens, and they can show an empty list during a long reply.
- Sessions created outside the API have no tag in any client chat, so clients cannot continue them.
- A client that removes the session tag from its `assistant` messages cannot continue its chats. The daemon returns `400`.
- A client that does not render Markdown shows the session tag line. Copied text and plain-text previews also include it.
- Clients must disable automatic title, chat tag and follow-up requests.
- Deleting request/response pairs and rolling back a session on regenerate or edit are not supported by the headless daemon.
- Regenerate and edit add a new turn. The replaced reply stays in the CHA transcript and in the model context.
- Characters that use ChatGPT subscription providers do not work on the server (see Provider access).
- Sampling fields in requests (`temperature`, `max_tokens` and similar) have no effect.

**Open questions.**

- [ ] Regenerate or edit: the daemon adds a new turn. Should a later version detect these cases exactly (for example, with the entry ID of the turn in the session tag) and fork a new session that keeps the old branch?
- [ ] Subscription access: add a `cha-daemon --login` command that runs the existing device flow (`start_openai_auth()`, `poll_openai_auth()`) and prints the code?
- [ ] Configuration edits: add `--export DIR` and `--import DIR` flags that call the existing offline `export_workspace_configuration()` and `import_workspace_configuration()`?
- [x] Idle exit: not used (see Process lifecycle).
- [x] Vault password: a fixed `password` file in the configuration directory (see Changes to the application).

## Appendix A. Issues

- **Session selection by hash.** Resolved. The design now selects the session by a session tag in each reply (see Session matching).

- **Session lifecycle and output waiting.** Resolved. The daemon uses `Application::open_session()`, and the adapter polls the session output until a defined turn end (see Turn lifecycle and Changes to the application).

- **Media handling.** Resolved. The daemon links `cha_lib` as it is. The media code stays, and its workers stay idle (see Changes to the application).

- **API key matching.** Resolved. API keys are lowercase hex, so case-insensitive matching has no effect (see Authentication and routing). See the [nginx map documentation](https://nginx.org/en/docs/http/ngx_http_map_module.html).

- **Authorization header forwarding.** Resolved. `scgi_pass_request_headers off` keeps all request headers out of the daemon (see Authentication and routing). See the [nginx SCGI documentation](https://nginx.org/en/docs/http/ngx_http_scgi_module.html#scgi_pass_request_headers).

- **Protected vault startup.** Resolved. The daemon reads `password` from the configuration directory when the vault is protected. No command-line option is necessary (see Changes to the application).
