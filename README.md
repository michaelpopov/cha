# cha

`cha` is a C++20 chat client for servers exposing the OpenAI-compatible
chat-completions API. It provides the full-screen `cha` TUI and the
line-oriented `chacon` console frontend.

## Workspace configuration

Run either executable from a workspace containing `personas/` and `rooms/`.
`rooms/rooms.list` is an ordered list of room names (one per line; blank lines
and `#` comments are ignored). At `cha` startup, a terminal selector lets you
choose a room and then an existing session or **New session**. `chacon` makes
the same selection through command-line options.

Each room contains `personas.list`, an ordered list of one or more personas, and `USER.md`. Every listed persona is loaded from `personas/<persona>/config.toml` and `SYSTEM.md`; each gets its own model connection and effective system prompt (`SYSTEM.md`, followed by the room `USER.md`, followed by generated room context that identifies the agent and the room's other personas). The first persona is the default. Start a prompt with `@Name` to choose another agent; names are matched case-insensitively and an unambiguous prefix works. Use `@@` to send a literal leading `@`, and `/@Name` to change the default for the current run.

Each persona's immutable `id` identifies transcript entries; its `name` is the visible `@mention` handle. Names cannot contain whitespace, start with `@` or `/`, or be `User` (case-insensitively). A room cannot contain duplicate IDs or names. All agents use the session's shared chat transcript. Exchanges involving another agent are supplied as escaped JSON Lines so the receiving agent can treat the other speaker's first-person statements as quoted history rather than its own identity. A session stores only its room and transcript, so it can be reopened even if the room's personas changed.

Each session is stored in one self-contained `sessions/<id>.sqlite3` database. Its embedded version, ID, and room must match the selected room before the transcript can be restored. A new session can be given an optional display name. Its database uses a local-time `YYYY-MM-DD-HH-MM-SS-session` base name (with a numeric suffix only on collision), while the display name is stored inside the database. Each submitted turn and its identified completion, cancellation, or failure is committed as an SQLite transaction. A turn without a terminal state is reported as interrupted when the session is restored. Cancelled partial answers remain visible but are not sent back to the model as completed history. Successful responses require non-empty answer text; streaming responses also require a `[DONE]` marker, after which further data is ignored.

`personas/base_config.toml` may define configuration shared by every persona in
the workspace. A persona's own `config.toml` overrides it field by field. The
precedence is built-in defaults, then `base_config.toml`, then the persona
file. The base file is optional. `id` and `name` are never inherited: they must
appear in every persona file and are not allowed in the base file.

The following top-level configuration fields are supported:

- `host`: required server host name or address.
- `port`: required server port.
- `id`: required stable persona identifier containing only ASCII letters, digits, underscores, and hyphens. Do not change it when renaming or moving the persona directory.
- `name`: required display name and `@mention` handle. It cannot contain whitespace, start with `@` or `/`, or equal `User` case-insensitively.
- `mode`: `net` for llama.cpp or `test` for the built-in echo server; defaults to `test`.
- `model`: optional model name sent in chat-completions requests. If omitted, the first model returned by the endpoint's `/v1/models` API is used.
- `stream`: whether to request streamed SSE responses; defaults to `true`.
- `temperature`: numeric sampling temperature from `0` to `2`; defaults to
  `1.0` and is included in every completion request.
- `api_key`: optional bearer token. An empty string disables authentication.
- `api_key_env`: optional environment-variable name containing a bearer token. It takes precedence over `api_key`.
- `reasoning_effort`: optional reasoning level sent with chat-completions requests, such as `medium`.
- `reasoning_format`: representation used for provider-visible reasoning output; defaults to `auto`. Supported values are `auto`, `none`, `reasoning_content`, and `reasoning`.
- `https`: use HTTPS instead of HTTP; defaults to `false`.

Example:

```toml
# personas/base_config.toml
host = "127.0.0.1"
port = 8080
mode = "net"
model = "local-model"
stream = true
api_key = ""
reasoning_format = "auto"
```

```toml
# personas/local-assistant/config.toml
id = "local-assistant"
name = "Local assistant"
temperature = 0.7
```

`reasoning_effort` requests a generation policy; `reasoning_format` describes
how to interpret the response. They are independent. In `auto` mode, `cha`
recognizes `choices[0].delta.reasoning_content` and `.reasoning` (or the
corresponding non-streaming `message` fields), preferring
`reasoning_content` when both are present. `none` disables extraction, while
the two named formats strictly select one field. Ordinary `content` always
remains answer text.

Reasoning is labeled and dimmed only while its turn is active. It never enters
the chat transcript, SQLite, or later model context, and is cleared when the
turn ends. A cancellation containing only reasoning has no response entry; a
cancellation with reasoning and a partial answer retains only that answer.

Reasoning embedded inside ordinary `content`, including `<think>` tags, is not
parsed. Such content is displayed, stored, and replayed as answer text because
there is no structured semantic boundary. Malformed successful streaming
responses report only sanitized HTTP status, content type, and byte-count
metadata; model-output body bytes are not copied into the error.

In net mode, `cha` sends HTTP requests to:

```text
http://HOST:PORT/v1/chat/completions
```

Chat requests deliberately have no overall or low-speed timeout so long generations can complete. Use `/stop`, Escape, or Ctrl-C to cancel an active request.

HTTPS servers require a libcurl build with a TLS backend. When the bundled libcurl is used, CMake enables OpenSSL automatically when its development files are available.

Before loading server configuration, the application optionally reads `.env` from the working directory. It accepts `NAME=value` entries, ignores blank lines and `#` comments, and does not replace variables already set in the process environment.

## Commands

- `/clear` starts a new visible history while retaining the system prompt.
  Earlier transcript rows remain in the session database.
- `/info` displays the transcript entry count followed by the current room's personas.
- `/agents` displays the current room's personas and marks the default agent.
- `/@Name` changes the default agent for this run only.
- `/stop` cancels the active model response.
- `/exit` exits the application.

## Full-screen UI

The UI has a scrollable chat transcript, a generation-status line, and a persistent multiline input pane. Input remains available while a response is streaming.

The status changes from `generating` to `reasoning` when structured reasoning
arrives, then to `responding` when answer text begins.

- Press `Page Up` and `Page Down` to scroll through the transcript.
- Press `Esc` or `Ctrl-C` while generating to stop the response immediately.
- Press `Ctrl-C` while idle to exit.
- End an input line with `\` to continue on the next visual line. Continued lines are concatenated before being sent.
- Arrow, Home, End, Backspace, and Delete keys edit the input buffer.

## Console frontend

`chacon` is intended for pipes, logs, and simple interactive terminals. It
requires a room except when listing all rooms:

```text
chacon --list-rooms
chacon --room ROOM --list-sessions
chacon --room ROOM [--session ID | --new LABEL] [--color=auto|always|never]
```

If neither `--session` nor `--new` is supplied, `chacon` creates a new session
with the default timestamp label. On an interactive terminal it reports the room
and the resolved session ID before the first prompt, so a session created by
`--new` or by default can be reopened later with `--session`. The interactive
prompt is a bold `@DefaultAgentName> ` marker, for example `@Ismael> `, so it
always identifies the agent that will receive an unaddressed submission.
Running `/@Name` changes both the run-local default and the next prompt marker.
Room listings contain one name per line. Session listings contain exactly three tab-separated
fields—ID, label, and error—with no header, padding, or color. Invalid sessions
remain visible in the listing. Listing modes ignore session-selection flags;
selection validation applies only when opening or creating a session.

Each input line is one submission. A trailing `\` continues the submission on
the next line and the lines are concatenated without a newline. CRLF input is
normalized by removing the trailing carriage return. Piped submissions are
queued and run one at a time. Bare `/stop` and `/exit` take effect immediately;
forms with arguments retain FIFO order and receive the shared “does not accept
arguments” notice. An immediate `/exit` cancels an active response instead of
waiting for it. EOF stops input but lets the active response and queued prompts
finish. Ctrl-C cancels an active response and exits when idle.

Transcript text is an append-only log on stdout; prompts, responses, and
restored history are never rewritten. Notices and the named interactive prompt
go to stderr, keeping stdout suitable for redirection. With `--color=auto`, the
prompt uses stderr's terminal status for bold styling independently of stdout;
`always` and `never` force styling for both streams. Model text is sanitized so
it cannot inject terminal control sequences, including a C1 sequence split
across streaming chunks. Final sanitizer state is emitted before a checked
stdout flush, so a late output failure still produces exit code 1.

## Build and test

The build uses an installed libcurl development package when available. Otherwise CMake downloads and builds a pinned libcurl automatically.

```bash
make
make test
make run
make run-console
```

Live integration tests are built as the `itest` application but are not included in `make test`:

```bash
make itest
```

The console and core library can be built without curses:

```bash
cmake -S . -B build/notui -DCHA_BUILD_TUI=OFF
cmake --build build/notui
ctest --test-dir build/notui
```

## Architecture

The source tree is documented from the inside out, with diagrams:

| Document | Covers |
| --- | --- |
| [`src/README.md`](src/README.md) | High-level architecture: layers, threading, the life of a turn, persistence, and the invariants that hold everywhere. Start here. |
| [`src/transcript/README.md`](src/transcript/README.md) | The transcript model shared by every layer. |
| [`src/agents/README.md`](src/agents/README.md) | Persona loading, the execution thread, and the HTTP transport. |
| [`src/session/README.md`](src/session/README.md) | Workspace and session operations, SQLite persistence, chat coordination. |
| [`src/ui/README.md`](src/ui/README.md) | The UI contract, with shared [`render/`](src/ui/render/README.md) and [`text/`](src/ui/text/README.md), plus the [`tui/`](src/ui/tui/README.md) and [`console/`](src/ui/console/README.md) frontends. |
| [`src/apps/README.md`](src/apps/README.md) | Executable composition roots. |
| [`src/util/README.md`](src/util/README.md) | Shared leaf helpers, including the pollable event channel. |
| [`docs/cha.md`](docs/cha.md) | The exhaustive rule-by-rule design reference. |
