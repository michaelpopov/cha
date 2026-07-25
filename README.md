# cha

`cha` is a C++20 terminal chat client for servers exposing the OpenAI-compatible chat-completions API.

## Workspace configuration

Run `cha` from a workspace containing `personas/` and `rooms/`. `rooms/rooms.list` is an ordered list of room names (one per line; blank lines and `#` comments are ignored). At startup, a terminal selector lets you choose a room and then an existing session or **New session**.

Each room contains `personas.list`, an ordered list of one or more personas, and `USER.md`. Every listed persona is loaded from `personas/<persona>/config.toml` and `SYSTEM.md`; each gets its own model connection and effective system prompt (`SYSTEM.md` followed by the room `USER.md`). The first persona is the default. Start a prompt with `@Name` to choose another agent; names are matched case-insensitively and an unambiguous prefix works. Use `@@` to send a literal leading `@`, and `/@Name` to change the default for the current run.

Each persona's immutable `id` identifies transcript entries; its `name` is the visible `@mention` handle. Names cannot contain whitespace, start with `@` or `/`, or be `User` (case-insensitively). A room cannot contain duplicate IDs or names. All agents use the session's shared chat transcript: other agents' prior answers are attributed when sent as context. A session stores only its room and transcript, so it can be reopened even if the room's roster changed.

Each session is stored in one self-contained `sessions/<id>.sqlite3` database. Its embedded version, ID, and room must match the selected room before the transcript can be restored. A new session can be given an optional display name. Its database uses a local-time `YYYY-MM-DD-HH-MM-SS-session` base name (with a numeric suffix only on collision), while the display name is stored inside the database. Each submitted turn and its identified completion, cancellation, or failure is committed as an SQLite transaction. A turn without a terminal state is reported as interrupted when the session is restored. Cancelled partial answers remain visible but are not sent back to the model as completed history. Successful responses require non-empty answer text; streaming responses also require a `[DONE]` marker, after which further data is ignored. The following top-level persona configuration fields are supported:

- `host`: required server host name or address.
- `port`: required server port.
- `id`: required stable persona identifier containing only ASCII letters, digits, underscores, and hyphens. Do not change it when renaming or moving the persona directory.
- `name`: required display name and `@mention` handle. It cannot contain whitespace, start with `@` or `/`, or equal `User` case-insensitively.
- `mode`: `net` for llama.cpp or `test` for the built-in echo server; defaults to `test`.
- `model`: optional model name sent in chat-completions requests. If omitted, the first model returned by the endpoint's `/v1/models` API is used.
- `stream`: whether to request streamed SSE responses; defaults to `true`.
- `temperature`: optional numeric sampling temperature. It is omitted from requests when unset.
- `api_key`: optional bearer token. An empty string disables authentication.
- `api_key_env`: optional environment-variable name containing a bearer token. It takes precedence over `api_key`.
- `reasoning_effort`: optional reasoning level sent with chat-completions requests, such as `medium`.
- `reasoning_format`: representation used for provider-visible reasoning output; defaults to `auto`. Supported values are `auto`, `none`, `reasoning_content`, and `reasoning`.
- `https`: use HTTPS instead of HTTP; defaults to `false`.

Example:

```toml
id = "local-assistant"
name = "Local assistant"
host = "127.0.0.1"
port = 8080
mode = "net"
model = "local-model"
stream = true
temperature = 0.7
api_key = ""
reasoning_format = "auto"
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
- `/info` displays the transcript entry count followed by the current room roster.
- `/agents` displays the current room roster and marks the default agent.
- `/@Name` changes the default agent for this run only.
- `/stop` cancels the active model response.
- `/exit` exits the application.

## Terminal UI

The UI has a scrollable chat transcript, a generation-status line, and a persistent multiline input pane. Input remains available while a response is streaming.

The status changes from `generating` to `reasoning` when structured reasoning
arrives, then to `responding` when answer text begins.

- Press `Page Up` and `Page Down` to scroll through the transcript.
- Press `Esc` or `Ctrl-C` while generating to stop the response immediately.
- Press `Ctrl-C` while idle to exit.
- End an input line with `\` to continue on the next visual line. Continued lines are concatenated before being sent.
- Arrow, Home, End, Backspace, and Delete keys edit the input buffer.

## Build and test

The build uses an installed libcurl development package when available. Otherwise CMake downloads and builds a pinned libcurl automatically.

```bash
make
make test
make run
```

Live integration tests are built as the `itest` application but are not included in `make test`:

```bash
make itest
```

## Architecture

The source tree is documented from the inside out, with diagrams:

| Document | Covers |
| --- | --- |
| [`src/README.md`](src/README.md) | High-level architecture: layers, threading, the life of a turn, persistence, and the invariants that hold everywhere. Start here. |
| [`src/transcript/README.md`](src/transcript/README.md) | The transcript model shared by every layer. |
| [`src/agents/README.md`](src/agents/README.md) | Persona loading, rosters, the execution thread, and the HTTP transport. |
| [`src/session/README.md`](src/session/README.md) | Workspace and session operations, SQLite persistence, chat coordination. |
| [`src/ui/README.md`](src/ui/README.md) | The UI contract, with [`text/`](src/ui/text/README.md) and [`terminal/`](src/ui/terminal/README.md) beneath it. |
| [`src/apps/README.md`](src/apps/README.md) | Executable composition roots. |
| [`src/util/README.md`](src/util/README.md) | Shared leaf helpers, including the pollable event channel. |
| [`docs/cha.md`](docs/cha.md) | The exhaustive rule-by-rule design reference. |
