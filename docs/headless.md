# CHA Headless Daemon — Design

Sep 27, 2026 · @Michael Popov · Updated Sep 30, 2026: the daemon serves only
the ChaWeb API. The OpenAI-compatible API was removed.

## Overview

`cha-daemon` is the headless Linux variant of CHA. It serves the API of the
ChaWeb browser application through nginx. Each user has one vault, one SQLite
file and one daemon process.

This document describes the daemon process: startup, SCGI, request handling and
operations. The [ChaWeb guide](chaweb.md) specifies the API contract and the
browser behavior. [CHA daemon on Linux](../packaging/linux/README.md) is the
installation reference.

Goals:

- Reach CHA conversations (forums, characters, `@Name`, `/mcast`, stored
  sessions and voice playback) from a phone browser.
- Keep HTTP out of the application. nginx handles HTTP, TLS, static files and
  routing.
- Add no request concurrency to the application. One process serves one user,
  and it handles one request at a time.
- Reuse the existing core (`app/`, `runtime/`, `session/`, `storage/`,
  `providers/`, `media/`) without changes to its threading model.

Non-goals:

- Sharing one vault or one session between users.
- Authentication. The nginx listening port selects the user, but it does not
  authenticate the user.
- General vault selection and configuration editing over the API. Narrow R2
  upload, download, and parent-merge operations are supported.
- An OpenAI-compatible API. An earlier version served `/v1/models` and
  `/v1/chat/completions`. These paths now return `404`.

## Architecture

nginx is the only HTTP component. It serves the static ChaWeb files. It
forwards `/api/cha/v1/` over SCGI to the Unix socket of the user that owns the
listening port. systemd owns each socket and starts the user's daemon on the
first connection.

```text
browser ──HTTPS──► nginx ──SCGI──► /run/cha/<user>.sock ──fd 3──► cha-daemon ──► model and voice providers
        (one port per user)          (owned by systemd)               │
                                                                      └──► the user's vault (SQLite)
```

The daemon code is in `src/daemon/`:

| File | Job |
| --- | --- |
| `main.cpp` | Parses `--config`, checks socket activation, reads the vault password, opens `Application`, runs the accept loop and shuts down. |
| `scgi.*` | Reads the SCGI request and writes the CGI-style response. |
| `chaweb_adapter.*` | Maps the `/api/cha/v1/` routes to `Application` calls and writes JSON, audio or empty responses. |

The CHA core keeps its threads: one runtime thread, the provider workers and
three audio download workers. ChaWeb voice playback uses the audio workers.

## Request flow

1. The browser sends a request, for example
   `POST /api/cha/v1/forums/lobby/sessions/<id>/input` with the JSON body
   `{"text": "Hello"}`.
2. nginx receives the request on the user's port. The server block of that
   port has a fixed `scgi_pass` to the user's socket. nginx reads the whole
   body (at most 256 KiB), then connects to `unix:/run/cha/<user>.sock`.
3. If the user's daemon is not running, systemd starts `cha@<user>.service` and
   passes it the listening socket as file descriptor 3. The connection waits in
   the socket backlog meanwhile.
4. The daemon accepts the connection. If it is busy with another request, the
   connection waits in the backlog until that request ends.
5. The SCGI loop reads the header block and the body. If nginx closed the
   connection in the meantime, the loop drops the request. Otherwise it gives
   the request to the ChaWeb adapter.
6. The adapter parses the route, checks the body, captures the application
   context epoch and opens the named session. Then it calls one `Application`
   operation.
7. The adapter writes the response: JSON, audio bytes, or an empty `204`. The
   loop closes the connection and accepts the next one.

No handler waits for a model reply. When CHA accepts input, the adapter returns
`204` at once. The runtime thread and the provider workers continue the turn.
The browser reads the progress with later snapshot requests.

## Routing and access

nginx selects the user from the listening port. The daemon never sees a user
name or a key.

`packaging/linux/nginx-chaweb.conf.example` contains one server block. Each
user gets a copy with a different port and a literal socket path:

```nginx
server {
    listen 192.168.1.10:8443 ssl;
    root /srv/cha/chaweb;

    location /api/cha/v1/ {
        add_header Cache-Control "no-store" always;
        client_max_body_size      256k;
        include                   scgi_params;
        scgi_param HTTP_X_CHA_AUDIO_OFFSET $http_x_cha_audio_offset;
        scgi_pass                 unix:/run/cha/alice.sock;
        scgi_pass_request_headers off;
        scgi_request_buffering    on;
        scgi_buffering            on;
        scgi_read_timeout         75s;
        # ...
    }
}
```

- The socket path is a literal in the server block. Request data cannot select
  another socket.
- ChaWeb has no login and no API key. Anyone who can reach a user's port can
  use that user's conversations. Listen only on a trusted private network.
- `scgi_pass_request_headers off` stops nginx from sending the HTTP headers to
  the daemon. The daemon gets the variables from `scgi_params`, which include
  `CONTENT_TYPE`, and the audio offset header that the block passes with
  `scgi_param`.
- Request and response buffering stay on. ChaWeb exchanges are complete JSON
  documents or audio chunks, not streams.
- The static files and the API have the same origin. The daemon sends no CORS
  headers, and `OPTIONS` returns `404`.
- A user with no socket unit gets `502` from nginx.

## Process lifecycle

systemd socket activation starts each user's daemon on the first connection.
One pair of template units serves all users; `%i` is the user name. The
installer fills in the paths and the service account.

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
[Unit]
Requires=cha@%i.socket
After=cha@%i.socket

[Service]
ExecStart=@CHA_DEPLOY_PATH@/cha-daemon --config @CHA_DATA_PATH@/%i/config
User=@CHA_INSTALL_USER@
PrivateTmp=yes
TimeoutStopSec=90s
```

- **Socket ownership.** systemd creates the socket and keeps it open even while
  no daemon runs. `Accept=no` gives the one listening socket to one service
  instance. Only members of the socket group, which includes nginx, can
  connect.
- **Account.** All daemons run as the user who installed the package. Each
  daemon has its own configuration directory and vault. Earlier packages used
  one `cha-<user>` system account for each user; the Linux README tells how to
  migrate.
- **Private `/tmp`.** CHA writes the materialized workspace, with the provider
  API keys as plain TOML, to the temporary directory. `PrivateTmp=yes` gives
  each instance its own `/tmp`, and systemd removes it when the instance stops.
  A crash therefore leaves no key files behind.
- **Start.** On the first connection, systemd starts the service with
  `LISTEN_FDS=1`. The daemon uses file descriptor 3 as its listening socket and
  does not bind one itself.
- **Crash.** If the daemon exits, the socket stays open. The next connection
  starts a new daemon.
- **Stop.** On `SIGTERM` or `SIGINT`, the signal handler sets an atomic stop
  flag. Socket waits see the flag within 50 ms. The loop leaves the current
  handler and requests application shutdown, which cancels running
  generations. The daemon then waits up to the 10 s shutdown grace. If the
  application does not stop in that time, the process calls `_exit()`.
  `TimeoutStopSec=90s` also covers an application call that waits for the 30 s
  command deadline.
- **No idle exit.** The daemon keeps running after its first start. Each start
  opens the vault, writes the workspace to `/tmp` and validates every forum.
- **One daemon per file.** The existing SQLite database lease stops a second
  process, such as the desktop application, from opening the same vault.

## SCGI bridge and serialization

The daemon handles one SCGI connection at a time. Serialization comes from the
accept loop itself, so the daemon adds no locks, queues or threads.

**Request format.** Each request arrives on its own connection:

```text
<len>:CONTENT_LENGTH\0<n>\0SCGI\01\0REQUEST_METHOD\0POST\0DOCUMENT_URI\0/api/cha/v1/forums/lobby/sessions\0CONTENT_TYPE\0application/json\0...,<n bytes of JSON body>
```

The header block is a netstring of NUL-separated name/value pairs, with
`CONTENT_LENGTH` first. The body of `CONTENT_LENGTH` bytes follows the comma.
The daemon uses `CONTENT_LENGTH`, `SCGI` (must be `1`), `REQUEST_METHOD`,
`DOCUMENT_URI`, `CONTENT_TYPE` and `HTTP_X_CHA_AUDIO_OFFSET`. It ignores other
names. A repeated name that the daemon uses makes the request invalid.

**Response format.** CGI-style headers, a blank line, then the body. Closing
the socket ends the response.

```text
Status: 201 Created\r\n
Content-Type: application/json\r\n
\r\n
{"id":"...","label":"..."}
```

**Accept loop.**

```text
listen_fd = 3                                // from systemd
loop until stop:
    fd = accept(listen_fd)
    request = read_scgi(fd)                  // headers + full body
    if request is malformed: write 400; close(fd); continue
    if request is too large: write 413; close(fd); continue
    if peer_closed(fd): close(fd); continue  // client left while queued
    handle_chaweb_request(request, fd)       // adapter writes to fd
    close(fd)
```

- While one request runs, new connections wait in the kernel backlog. Each
  request makes one short application call, so the wait is usually short.
- nginx cannot do this queuing itself. `max_conns=1` without the commercial
  `queue` directive returns `502` instead of waiting.
- nginx writes the complete request into the socket before it waits. When the
  client leaves, nginx closes the connection. `peer_closed(fd)` finds this:
  `recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT)` returns 0.
- `read_scgi` refuses a header block above 64 KiB, and a `CONTENT_LENGTH` above
  256 KiB with `413` before it allocates memory. The body limit is the same as
  `client_max_body_size` in nginx.
- Writes use `send(..., MSG_NOSIGNAL)` on Linux and `SO_NOSIGPIPE` on macOS, so
  a closed connection gives `EPIPE` instead of killing the process.
- The SCGI code needs no library.

## ChaWeb API

All paths are below `/api/cha/v1`. POST bodies, and the body of `DELETE .../audio`,
must use `application/json`. The [ChaWeb guide](chaweb.md) gives the full
contract: body validation, statuses and browser recovery.

| Method and path | Result |
| --- | --- |
| `GET /bootstrap` | The bootstrap document: forums, characters, personas, recent sessions and the vault name. |
| `GET /voice-input` | Dictation settings without credentials, or `null`. |
| `POST /voice-input/connect` | OpenAI SDP exchange; body `{"sdp", "languages"}`. |
| `POST /voice-input/xai/start` | Start dictation; body `{"session_id", "languages"}`. |
| `POST /voice-input/xai/audio` | Send PCM16; body `{"session_id", "pcm_base64"}`. |
| `POST /voice-input/xai/stop` | Finish dictation; body `{"session_id", "remaining_ms"}`. |
| `POST /voice-input/xai/cancel` | Release dictation; body `{"session_id"}`. |
| `POST /vault/upload-check` | Body `{}`; remote ETag, version status, and context epoch. |
| `POST /vault/upload` | Body `{"etag", "context_epoch"}` from the check; upload the database without cached speech and its companion definition. |
| `POST /vault/download` | Body `{}`; stage, validate, and replace the local pair with `.bac` backups. |
| `POST /vault/merge-parent` | Body `{}` or `{"password"}`; download the inactive parent from R2, then merge its configuration. |
| `GET /voice-output` | The voice output settings without credentials, or `null` when voice output is not configured. |
| `GET /forums/{forum}/sessions` | The stored sessions of the forum. |
| `POST /forums/{forum}/sessions` | Body `{"text"}`. Creates a session and submits the first input. `201` with `{"id", "label"}`. |
| `GET /forums/{forum}/sessions/{session}` | The session snapshot: transcript, generation state, characters and cached audio entries. |
| `DELETE /forums/{forum}/sessions/{session}` | Stops generation and deletes the session. `204`. |
| `POST .../{session}/input` | Body `{"text"}`. `204` when CHA accepts the input, `422` when CHA rejects it. |
| `POST .../{session}/stop` | Body `{}`. Stops the current generation. `204`. |
| `GET .../{session}/audio` | Audio status: cached entries and running downloads. |
| `POST .../{session}/audio` | Body `{"vault_name", "entry_ids"}`. Starts audio for several replies. |
| `DELETE .../{session}/audio` | Body `{"vault_name"}`. Clears the cached audio of the session. `204`. |
| `POST .../{session}/entries/{entry}/audio` | Body `{"vault_name"}`. Starts audio for one reply, or reports it as cached or running. |
| `GET .../{session}/entries/{entry}/audio` | Without `X-CHA-Audio-Offset`: the complete cached clip. With it: the next chunk (at most 64 KiB) and `X-CHA-Audio-Complete: 0` or `1`. `204` when no new bytes are ready yet, `502` when the download failed. |

Every other path or method returns `404`, including the former `/v1/` paths.

Errors use the `ErrorResponse` body from `resources/dto.yaml`:
`{"error": {"code", "message"}}`. The SCGI loop uses the same body for `400`
and `413`.

| Status | Cause |
| --- | --- |
| `400` | Malformed SCGI or JSON, unknown fields, or input larger than `prompt_limit` (32 KiB). |
| `404` | Unknown route, forum, session or entry. |
| `401` | Parent vault password required or rejected. |
| `409` | Stale vault context or a mismatched audio vault. |
| `413` | Body larger than 256 KiB. |
| `415` | A request body that is not `application/json`. |
| `422` | CHA rejected the input, with the CHA notice as the message. |
| `500` | Unexpected failure, or cleanup after a rejected first input failed. |
| `502` | Audio generation failed. |
| `503` | The application is stopping or unavailable, or audio downloads are busy. |

## Session handling

- **Open.** Every request that names a session first calls
  `Application::open_session()`. It selects that session in the live-session
  manager. The runtime retires a previously selected session when it is idle.
  At most 8 sessions are live at the same time.
- **First input.** `POST .../sessions` calls `create_session()`,
  `open_session()` and `submit()`. If CHA rejects the input, the adapter
  deletes the new session and returns `422`. If the outcome is unknown, for
  example after the 30 s command deadline, the adapter keeps the session and
  returns an error. The browser then finds the session in the session list.
- **Accepted input.** CHA accepted the input only if `submit()` returns a
  `CommandResult` with `session.input_consumed == true`. `clear_input` alone is
  not enough: an unknown `/` command also sets it.
- **Progress.** The browser reads the session snapshot about once a second
  while generation is active. `POST .../stop` stops the current generation.
- **Epoch.** The adapter captures `context_epoch()` once per request and passes
  it to every application call of that request. The daemon has no operation
  that changes the epoch while it runs.
- **Configuration.** The daemon never writes configuration.

## Voice input and vault maintenance

The browser supports configured OpenAI WebRTC and xAI dictation over HTTPS.
The adapter routes dictation messages to `Application` and retains a stable
resource scope across xAI HTTP batches. Keys stay in the daemon. Capture pauses
during generation, audio loading, speech playback, and the echo tail.

ChaWeb's session list exposes ETag-checked Upload, confirmed Download, and
Parent merge. The active vault supplies R2 credentials. A `parent` field names
a locally registered inactive vault; Parent merge downloads its latest R2 pair
before merging configuration without source sessions. Protected parents prompt
for a password. A successful parent download remains committed even if the
later merge fails. Download and merge refresh bootstrap and session lists.
These operations use normal application maintenance and context invalidation;
they do not expose arbitrary configuration editing or vault switching.

R2 uploads strip cached audio from a temporary copy and compact it. The live
vault retains its clips, and protected uploads remain encrypted.

## Voice playback

The application's audio download manager makes FishAudio clips for character
replies and stores them in the vault. Its three workers run the downloads.

- `POST .../entries/{entry}/audio` starts one download, or reports that the
  clip is cached or already running. `POST .../audio` starts downloads for
  several replies for automatic playback.
- The browser reads a running download in chunks with `X-CHA-Audio-Offset`.
  Chunks come from a growing in-memory clip. A read never waits for the
  provider, so audio requests do not block the accept loop.
- After the download, the clip is cached in the vault. A later request returns
  the cached clip.
- Automatic audio plays only newly completed replies, in transcript order.
  New automatic MP3 clips have a playback-only 2.5-second silent prefix; manual
  and resumed clips have none. Stored bytes and speech positions exclude it.
- Audio POST and DELETE requests name the vault. A request for another vault
  fails with `409`.

## Errors and disconnects

| Condition | Where | Response |
| --- | --- | --- |
| Body larger than 256 KiB | nginx | `413` |
| User has no socket, or the daemon fails to start | nginx | `502` |
| No daemon response within 75 s | nginx | `504` |
| Malformed SCGI request | daemon | `400` |
| SCGI body larger than 256 KiB | daemon | `413` |
| API errors | daemon | See the status table above. |

**Client disconnect while queued.** nginx writes the complete request before it
waits. `peer_closed(fd)` finds the closed connection after the read, and the
daemon closes it without an application call.

**Client disconnect during a request.** The handler finishes its application
call. The write then fails with `EPIPE`, and the daemon continues with the next
connection. Nothing is rolled back: accepted input stays accepted. The browser
never repeats a write automatically. It reads the snapshot or the session list
to find the outcome.

**Unexpected failures.** The adapter logs the exception and returns `500`. The
daemon continues to serve requests. Only `SIGTERM` and `SIGINT` set the stop
flag.

## Operations

[CHA daemon on Linux](../packaging/linux/README.md) and section 16 of the
[maintainer guide](MaintainerGuide.md#16-chaweb-deployment-on-linux) give the
complete steps. In summary:

**Per-user layout.**

```text
$CHA_DATA_PATH/alice/
└── config/                # the --config directory
    ├── app.toml           # vault = "Personal", [logging]
    ├── personal.toml      # vault_name = "Personal", data = "cha.sqlite3"
    ├── cha.sqlite3        # the vault file
    ├── logs/cha.log       # the daemon log
    └── password           # only for a protected vault, mode 0600
```

**Adding a user.** Run `"$CHA_DEPLOY_PATH/add_user.sh" alice`. It copies the
example vault, unless the configuration directory already exists, and enables
`cha@alice.socket`. Then add a server block with a new port to the ChaWeb
nginx site, run `sudo nginx -t` and reload nginx.

**Provider access.** A server vault must use providers with API keys. This
includes the provider that names new sessions, which is the Assistant's
provider by default. ChatGPT subscription providers read `openai-auth.json`
from the configuration directory, and the daemon has no login flow. Do not copy
`openai-auth.json` from a desktop. Each refresh saves a new refresh token, and
OpenAI can reject the old token that the other copy still uses.

**Changing configuration.** ChaWeb and the daemon have no settings editor. Stop
both units (`sudo systemctl stop cha@alice.socket cha@alice.service`), because
the socket starts a stopped service again. Edit the vault with the desktop
application, then start the socket again.

**Backups.** Copy the vault only while both units are stopped. A copy of a live
WAL database is not safe.

**Startup failures.** If the daemon fails at startup, for example because of a
wrong password or a bad configuration, each new request starts it again. After
a few failures, systemd reaches the start limit and also stops the socket, and
nginx returns `502`. After the repair, run
`sudo systemctl reset-failed cha@alice.socket cha@alice.service` and
`sudo systemctl start cha@alice.socket`.

**Logs.** Each daemon writes its own log file, as set in `app.toml`. systemd
records start, stop and crash events in the journal for `cha@<user>.service`.

**Development on macOS.** The daemon builds and runs on macOS, which has no
systemd. `scripts/run_daemon.py` does the socket activation: it binds the Unix
socket, passes it as file descriptor 3 with `LISTEN_FDS` and `LISTEN_PID`, and
starts the daemon. A local nginx can serve `webapp/dist-chaweb` and forward
`/api/cha/v1/` to that socket. macOS returns `ENOPROTOOPT` for `SO_ACCEPTCONN`,
so the daemon skips that one check there. Nothing restarts the daemon after it
exits, and a crash can leave the temporary workspace files. See
[Running on macOS](head-tutorial.html#macos).

## Limitations and open questions

The design trades throughput and flexibility for a daemon with no request
concurrency and no HTTP code.

**Limitations.**

- One request at a time per user. Requests are short, but a slow application
  call, such as a session open that waits for its 10 s deadline, delays the
  requests behind it.
- No authentication. Use the listeners only on a trusted private network.
- No configuration editing over the API. Stop the daemon and use the desktop
  application.
- The desktop application cannot open a vault while the daemon has it open.
- Characters that use ChatGPT subscription providers do not work on the server
  (see Provider access).

**Open questions.**

- [ ] Subscription access: add a `cha-daemon --login` command that runs the
  existing device flow (`start_openai_auth()`, `poll_openai_auth()`) and prints
  the code?
- [ ] Configuration edits: add `--export DIR` and `--import DIR` flags that call
  the existing offline `export_workspace_configuration()` and
  `import_workspace_configuration()`?
- [x] Idle exit: not used (see Process lifecycle).
- [x] Vault password: a fixed `password` file in the configuration directory
  (see Process lifecycle and the Linux README).
