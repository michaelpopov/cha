# cha

`cha` is a C++20 chat client for servers exposing the OpenAI-compatible
chat-completions API. It provides the full-screen `cha` TUI and the
line-oriented `chacon` console frontend.

## Workspace configuration

Run either executable from a workspace containing `app.toml`, `forums/`, and a
`personas/` roster. `Workspace` itself requires `forums/`; starting a session
requires a non-empty `personas/` directory, while forum listing remains available
before a roster has been created.
Each immediate subdirectory of `forums/` is a forum; forums are presented in
lexicographic name order. At `cha` startup, a terminal selector lets you choose
a persona, then a forum, and then an existing session or **New session**. `chacon` makes the same
selection through command-line options.

`app.toml` configures application-wide behavior. Its top-level `host` and
`port` settings define the web frontend's listener address. The logging section
requires a log file and level; relative log paths are resolved from the
workspace root. `chaweb` also uses that host and port as its allowed browser
authority to prevent DNS rebinding; loopback configurations additionally allow
`localhost`, `127.0.0.1`, and `[::1]` with the configured port. For network
access, configure the concrete address or hostname clients will use rather than
a wildcard listener address.

```toml
# app.toml
host = "127.0.0.1"
port = 8080

[logging]
file = "logs/cha.log"
level = "info"
```

Supported levels are `trace`, `debug`, `info`, `warn`, `error`, `critical`,
and `off`. When logging is enabled, missing parent directories for `file` are
created automatically. Logs rotate at 10 MiB and retain three files.

Each direct subdirectory of `personas/` is a persona. Its directory name is the
stable ID recorded in transcript rows and must be a C++ identifier
`[A-Za-z_][A-Za-z0-9_]*`; `persona.toml` must contain only a `display_name`, and
an optional `PERSONA.md` is fixed verbatim prompt text (not a template). Persona display
names may contain ordinary spaces and punctuation, but cannot be empty or
edge-whitespace, start with `@` or `/`, contain controls or line breaks, be
reserved, or duplicate another persona case-insensitively. The shared reserved
names are `persona`, `system`, `error`, `human`, `assistant`, `agent`, and `you`;
they are rejected case-insensitively for both persona IDs and display names, and for character display
names. Personas are loaded in lexicographic ID order. There is no `--list-personas`:
the console's `--persona` takes the directory ID, while the TUI and web lobby show
display names. A persona ID cannot equal a character ID, and a persona display name
cannot equal a character display name case-insensitively.

Each forum has `config.toml` with a required `display_name` string, a `characters/` directory with one or more character subdirectories, and `FORUM.md`. The directory name is the stable forum ID used by `chacon --forum`; it may contain only RFC 3986 unreserved ASCII characters (letters, digits, `-`, `.`, `_`, and `~`), excluding the complete names `.` and `..`. The display name is shown in the UI and forum listings and is not subject to the ID restriction. A forum is also the unit of distribution: it can be zipped and unpacked into another workspace. Each character is loaded from `forums/<forum>/characters/<character>/character.toml` and `SYSTEM.md`; each gets its own model connection and effective system prompt. Its four sections, in order, are expanded `SYSTEM.md`, expanded `FORUM.md`, the complete static persona roster, and generated forum context that identifies the agent and the forum's other characters. Character directories are loaded in lexicographic name order; the first is the default. Start a prompt with `@Name` to choose another agent; names are matched case-insensitively and an unambiguous prefix works. Use `@@` to send a literal leading `@`, and `/@Name` to change the default for the current run.

Each character directory's name is its stable ID and identifies transcript entries;
its `display_name` is the visible `@mention` handle. Display names cannot start
or end with whitespace, start with `@` or `/`, or be a reserved participant
name (case-insensitively). A multi-word display name can be addressed through any
unique word or word prefix—for example, `@Winston` or `@Churchill` for
`Winston Churchill`. A forum cannot contain duplicate IDs or display names. All
agents use the session's shared chat transcript. Exchanges involving another
agent are supplied as escaped JSON Lines so the receiving agent can treat the
other speaker's first-person statements as quoted history rather than its own
identity. A session stores only its forum and transcript, so it can be reopened
even if the forum's characters changed.

Each session is stored in one self-contained `sessions/<id>.sqlite3` database. Session IDs use the same URL-safe character set as forum IDs; files whose stems do not follow that rule are ignored. Its embedded version, ID, and forum must match the selected forum before the transcript can be restored. A new session can be given an optional display name. Its database uses a local-time `YYYY-MM-DD-HH-MM-SS-session` base name (with a numeric suffix only on collision), while the display name is stored inside the database and is not subject to the ID restriction. Each submitted turn and its identified completion, cancellation, or failure is committed as an SQLite transaction. A turn without a terminal state is reported as interrupted when the session is restored. Cancelled partial answers remain visible but are not sent back to the model as completed history. Successful responses require non-empty answer text; streaming responses also require a `[DONE]` marker, after which further data is ignored.

`forums/<forum>/characters/character_defaults.toml` may define configuration shared by every character in
that forum. A character's own `character.toml` overrides it field by field. The
precedence is built-in defaults, then `character_defaults.toml`, then the character
file. The defaults file is optional; it cannot define `display_name`.

### Prompt templates

`SYSTEM.md` and `FORUM.md` are expanded separately for each character before they
are concatenated. Two expansion forms and one escape are recognized; everything
else is copied unchanged:

| Macro | Meaning |
| --- | --- |
| `$$(path)` | Include another file, relative to the file containing the macro. |
| `$${name}` | Substitute a variable. |
| `$$$` | Escape the next macro prefix by producing a literal `$$`. For example, `$$$(file.md)` becomes `$$(file.md)`. |

Include paths are trimmed, resolved relative to the including file, and inserted
without adding separators or newlines. They must name regular files inside the
forum directory, including after symlink resolution. That keeps a zipped forum
self-contained: it cannot depend on sibling forums or anything outside itself.

Variables come from a `[prompt]` table (never from top-level connection fields
such as `api_key`) and from reserved names supplied by the loader:

```toml
# forums/example/characters/character_defaults.toml
[prompt]
register = "measured"

# forums/example/characters/local-assistant/character.toml
[prompt]
register = "energetic"
```

```markdown
$$(../shared/voice.md)
You are portraying $${character.display_name} in $${forum.display_name}.
```

The initial variable scope is the base `[prompt]` table overlaid by the
character's `[prompt]` table. When a template or included file is read, a
`[prompt]` table in a template directory's adjacent `config.toml` overlays the inherited
scope for that file and its descendants. Values may be strings, integers,
floating-point numbers, or booleans. Variable names may contain ASCII letters,
digits, `_`, `-`, and `.`.

Reserved names are `character.id`, `character.display_name`, `forum.id`, and
`forum.display_name`. They always come from the loader and cannot be overridden
by a `[prompt]` table. Expansion rejects unknown variables, malformed macros,
missing or cyclic includes, paths outside the forum, more than 256 includes, an
active file depth above 16, or output above 1 MiB. Each `SYSTEM.md` and
`FORUM.md` expansion has its own counters.

The following top-level configuration fields are supported:

- `host`: required server host name or address.
- `port`: required server port.
- `display_name`: required display name and `@mention` handle. It cannot start or end with whitespace, start with `@` or `/`, or be a reserved participant name case-insensitively. Internal whitespace is allowed. The character directory name is its stable identifier and must contain only ASCII letters, digits, underscores, and hyphens.
- `mode`: `net` for an OpenAI-compatible HTTP server or `test` for the built-in echo backend; defaults to `test`.
- `model`: optional model name sent in chat-completions requests. If omitted, the first model returned by the endpoint's `/v1/models` API is used.
- `stream`: whether to request streamed SSE responses; defaults to `true`.
- `temperature`: numeric sampling temperature from `0` to `2`; defaults to
  `1.0` and is included in every completion request.
- `api_key`: optional bearer token. An empty string disables authentication.
- `api_key_env`: optional environment-variable name containing a bearer token. It takes precedence over `api_key`.
- `reasoning_effort`: optional reasoning level sent with chat-completions requests, such as `medium`.
- `reasoning_format`: representation used for provider-visible reasoning output; defaults to `auto`. Supported values are `auto`, `none`, `reasoning_content`, and `reasoning`.
- `https`: use HTTPS instead of HTTP; defaults to `false`.

Character files use `display_name` and their directory-derived ID; the removed
`name` and `id` fields are rejected.

Example:

```toml
# forums/example/characters/character_defaults.toml
host = "127.0.0.1"
port = 8080
mode = "net"
model = "local-model"
stream = true
api_key = ""
reasoning_format = "auto"
```

```toml
# forums/example/characters/local-assistant/character.toml
display_name = "Local assistant"
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

Chat requests deliberately have no overall or low-speed timeout so long
generations can complete. Use `/stop`, Escape, or Ctrl-C to cancel the current
request, including every live runner in a multicast batch.

HTTPS servers require a libcurl build with a TLS backend. The bundled libcurl
uses Schannel on Windows, Secure Transport on macOS, and OpenSSL on other
platforms. Unix-like builds require OpenSSL so HTTPS is always available.

Before loading server configuration, the application optionally reads `.env` from the working directory. It accepts `NAME=value` entries, ignores blank lines and `#` comments, and does not replace variables already set in the process environment.

### Diagnostic logging

Diagnostic logging is configured by the workspace's `app.toml`. Set
`[logging].level` to `off` to disable it; otherwise logs append to
`[logging].file`. The application records lifecycle events at `info`, routine
cancellations at `info`, agent and persistence failures at `error`, and
unhandled top-level failures at `critical`. `debug` and `trace` are available
for detailed transport diagnostics.

Logs never write to the transcript or terminal streams. They do not record
prompts, response text, credentials, or provider error bodies.

## Commands

- `/clear` starts a new visible history while retaining the system prompt.
  Earlier transcript rows remain in the session database.
- `/hide-on` marks the start of a span to remove from later model context.
- `/hide` closes or extends that span to the current boundary; the transcript
  remains visible, but the enclosed entries are omitted from later requests.
- `/hide-off` removes the span and returns its entries to later model context.
- `/mcast [@Name, ... .] prompt` sends the same prompt to every character, or the
  selected characters in order, while keeping earlier multicast answers out of
  later multicast requests.
- `/info` displays the transcript entry count followed by the current forum's characters.
- `/agents` displays the current forum's characters and marks the default agent.
- `/@Name` changes the default agent for this run only.
- `/stop` cancels every live model run in the current request. If a
  foreground response completed before the stop request was processed, that
  completion is committed normally while the remaining multicast work is
  cancelled.
- `/exit` exits the application.

## Full-screen UI

The UI has a scrollable chat transcript, a generation-status line, and a persistent multiline input pane. Input remains available while a response is streaming.

The status changes from `generating` to `reasoning` when structured reasoning
arrives, then to `responding` when answer text begins. After cancellation it
shows `stopping` until background cleanup has retired the batch's runners.

- Press `Page Up` and `Page Down` to scroll through the transcript.
- Press `Esc` or `Ctrl-C` while generating to request cancellation immediately.
- Press `Ctrl-C` while idle to exit.
- End an input line with `\` to continue on the next visual line. Continued lines are concatenated before being sent.
- Arrow, Home, End, Backspace, and Delete keys edit the input buffer.

## Console frontend

`chacon` is intended for pipes, logs, and simple interactive terminals. It
requires a forum except when listing all forums:

```text
chacon --list-forums
chacon --forum FORUM --list-sessions
chacon --forum FORUM --check
chacon --persona PERSONA --forum FORUM [--session ID | --new LABEL] [--color=auto|always|never]
```

`--persona` is required for chat startup and is not accepted by listing or
`--check` modes. It selects the stable persona-directory ID that attributes every
prompt in that console run.

`--check` performs a read-only validation of the forum and exits. It checks the
forum and character directories and required files, forum/character TOML settings,
effective required connection settings, character ID and display-name validity
and uniqueness, the validated persona roster and persona/character collision rules,
and complete expansion of every character's `SYSTEM.md` and
`FORUM.md`—including variables, includes, containment, cycles, and limits. A
successful check prints, for example, `Forum 'stoics' is valid (3 characters).`
and exits with status 0; a validation failure uses the normal `Failed: ...`
diagnostic and exits with status 1.

The check does not inspect `sessions/` or arbitrary Markdown hyperlinks. It
does not create a session, require the environment variable named by
`api_key_env` to be set, initialize provider clients, discover a model, or make
network requests.

If neither `--session` nor `--new` is supplied, `chacon` creates a new session
with the default timestamp label. On an interactive terminal it reports the forum
and the resolved session ID before the first prompt, so a session created by
`--new` or by default can be reopened later with `--session`. The interactive
prompt is a bold `@DefaultAgentName> ` marker, for example `@Ismael> `, so it
always identifies the agent that will receive an unaddressed submission.
Running `/@Name` changes both the run-local default and the next prompt marker.
Forum listings contain one display name per line. Session listings contain exactly three tab-separated
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
`always` and `never` force styling for both streams. Console and redirected
streams use UTF-8 bytes; on Windows, the attached console is configured to
interpret those bytes as UTF-8. Windows command-line arguments are converted
from UTF-16 to UTF-8 before parsing. Automatic color enables
virtual-terminal processing and stays off if the console host does not support
it. Model text is sanitized so it cannot inject terminal
control sequences, including a C1 sequence split across streaming chunks.
Final sanitizer state is emitted before a checked
stdout flush, so a late output failure still produces exit code 1.

Human prompt text remains clean in the transcript and database, but each plain
human message sent to a model is projected as `from <display name>:\n<text>`.
The same prefix is used for the live request and replayed history; quoted
shared-history JSONL already has a speaker field and is not prefixed.

Pre-existing checked-in session databases were deleted rather than migrated:
their historical `human` participant IDs do not denote a workspace persona.

## Build and test

The build uses an installed libcurl development package when available.
Otherwise CMake downloads and builds a pinned libcurl automatically. It also
downloads a pinned libuv for portable console input, signal handling, and
cross-thread wakeups, and cpp-httplib for the web frontend.

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
cmake --preset console
cmake --build --preset console
ctest --test-dir build/console
```

The `console` preset explicitly sets `CHA_BUILD_TUI=OFF`, making it the
appropriate preset for console-only CI on every platform.

`chaweb` is built by the same preset as one process with the configured
`app.toml` host and port. It has one listener and no child-session or
per-session-port mode. The ordinary suite excludes the socket/process and
long-concurrency groups; run all three groups for web changes:

```bash
ctest --test-dir build/console --output-on-failure -LE "web_process|web_stress"
ctest --test-dir build/console --output-on-failure -L web_process
ctest --test-dir build/console --output-on-failure -L web_stress
```

GNU- and Clang-family toolchains also provide reproducible hardening presets,
which take the same three test groups:

```bash
cmake --preset console-asan-ubsan
cmake --build --preset console-asan-ubsan
ctest --test-dir build/console-asan-ubsan --output-on-failure -LE "web_process|web_stress"

cmake --preset console-tsan
cmake --build --preset console-tsan
ctest --test-dir build/console-tsan --output-on-failure -LE "web_process|web_stress"
```

The sanitizer presets require a compiler that supports the selected runtime and
are intentionally off by default. They instrument the fetched dependencies as
well as this project, because ThreadSanitizer reports false races against
synchronization it cannot see. The native companion-file lease backend is
exercised by the portable unit tests on every supported platform; the POSIX
process harness adds cross-process crash-release coverage on Linux and macOS.

[`docs/web-verification.md`](docs/web-verification.md) records which design
test bullets each suite covers, the two deliberate differences in instrumented
builds, and which platforms and sanitizers were and were not exercised.

On macOS and Windows the TUI option defaults to off, so the default build
produces the console frontend only. The ncurses TUI remains Linux-only.

## Architecture

The source tree is documented from the inside out, with diagrams:

| Document | Covers |
| --- | --- |
| [`src/README.md`](src/README.md) | High-level architecture: layers, threading, the life of a turn, persistence, and the invariants that hold everywhere. Start here. |
| [`src/transcript/README.md`](src/transcript/README.md) | The transcript model shared by every layer. |
| [`src/agents/README.md`](src/agents/README.md) | Character loading, staged runner threads, and the HTTP transport. |
| [`src/session/README.md`](src/session/README.md) | Workspace and session operations, SQLite persistence, chat coordination. |
| [`src/ui/README.md`](src/ui/README.md) | The UI contract, with shared [`render/`](src/ui/render/README.md) and [`text/`](src/ui/text/README.md), plus the [`tui/`](src/ui/tui/README.md) and [`console/`](src/ui/console/README.md) frontends. |
| [`src/apps/README.md`](src/apps/README.md) | Executable composition roots. |
| [`src/ui/web/README.md`](src/ui/web/README.md) | The one-listener web transport, session ownership, SSE, and shutdown boundary. |
| [`src/util/README.md`](src/util/README.md) | Shared leaf helpers, including prompt-template expansion, the concurrent queue, and wake adapters. |
