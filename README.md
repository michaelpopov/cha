# cha

`cha` is a C++20 terminal chat client for a llama.cpp server exposing the OpenAI-compatible chat-completions API.

## Workspace configuration

Run `cha` from a workspace containing `personas/` and `rooms/`. `rooms/rooms.list` is an ordered list of room names (one per line; blank lines and `#` comments are ignored). At startup, a terminal selector lets you choose a room and then an existing session or **New session**.

Each room contains `personas.list`, which must name exactly one persona, and `USER.md`. The selected persona is loaded from `personas/<persona>/config.toml` and `SYSTEM.md`. The effective system prompt is the persona `SYSTEM.md` followed by the room `USER.md`.

Existing sessions are discovered only when both `sessions/<id>.data` and `sessions/<id>.meta` exist; their conversation data is restored when selected. A new session can be given an optional display name. Its files use a local-time `YYYY-MM-DD-HH-MM-SS-session` base name (with a numeric suffix only on collision), while the display name is stored in its metadata. A new session creates its metadata immediately and saves its conversation when the chat closes. The following top-level persona configuration fields are supported:

- `host`: required server host name or address.
- `port`: required server port.
- `mode`: `net` for llama.cpp or `test` for the built-in echo server; defaults to `test`.
- `model`: optional model name sent in chat-completions requests. If omitted, the first model returned by the endpoint's `/v1/models` API is used.
- `stream`: whether to request streamed SSE responses; defaults to `true`.
- `temperature`: optional numeric sampling temperature. It is omitted from requests when unset.
- `api_key`: optional bearer token. An empty string disables authentication.
- `api_key_env`: optional environment-variable name containing a bearer token. It takes precedence over `api_key`.
- `reasoning_effort`: optional reasoning level sent with chat-completions requests, such as `medium`.
- `https`: use HTTPS instead of HTTP; defaults to `false`.

Example:

```toml
host = "127.0.0.1"
port = 8080
mode = "net"
model = "local-model"
stream = true
temperature = 0.7
api_key = ""
```

In net mode, `cha` sends HTTP requests to:

```text
http://HOST:PORT/v1/chat/completions
```

Chat requests deliberately have no overall or low-speed timeout so long generations can complete. Use `/stop`, Escape, or Ctrl-C to cancel an active request.

HTTPS servers require a libcurl build with a TLS backend. When the bundled libcurl is used, CMake enables OpenSSL automatically when its development files are available.

Before loading server configuration, the application optionally reads `.env` from the working directory. It accepts `NAME=value` entries, ignores blank lines and `#` comments, and does not replace variables already set in the process environment.

## Commands

- `/clear` clears conversation history while retaining the system prompt.
- `/info` displays the endpoint, configured model, streaming mode, and history size.
- `/stop` cancels the active model response.
- `/exit` exits the application.

## Terminal interface

The interface has a scrollable conversation transcript, a generation-status line, and a persistent multiline input pane. Input remains available while a response is streaming.

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
