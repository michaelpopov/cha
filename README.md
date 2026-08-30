# CHA

CHA is a C++20 browser application for chatting with OpenAI-compatible model
servers. The `chaweb` process serves the browser client and its HTTP/SSE API.

## Start chatting

For a staged development run:

```sh
make run
```

Open the address printed by the launcher. CHA starts in the process-local
**Entrance / Welcome** conversation as **Guest** with **Assistant**. Use the
browser to inspect forums, create or reopen a stored session, and select a forum
character. Which persona you speak as follows the forum you are in and is set in
that forum's configuration, not in the browser.

Welcome is private to the running server and is deleted on shutdown. Stored
sessions in workspace forums remain in their SQLite databases. Conversation
databases formerly created in Entrance by removed application variants are left
on disk but are not discoverable or supported by `chaweb`.

The chat input also accepts these controller-level commands:

| Command | Purpose |
| --- | --- |
| `/clear` | Clear the transcript. |
| `/hide-on`, `/hide`, `/hide-off` | Manage the off-record model-context span. |
| `/mcast` | Send one prompt to multiple forum characters. |
| `/info`, `/characters` | Inspect the session and its characters (`/agents` remains an alias). |
| `/@Name` | Change and save the forum's default character. |
| `/@-` | Enter recording mode: plain messages are saved to the transcript but not sent to a model. Session-local — `-` is never saved as the forum default. |
| `/!Name` | Change and save the forum's current persona. |
| `/style <name>` | Override the current character's appearance (font, slant, weight, size) for this session only; `/style` reports the override and `/style default` restores the configured style. Nothing is saved. |
| `/stop` | Stop generation. |
| `/exit` | Close the live session. |

Leading `@Name` addresses a prompt to one character. `@@` starts literal text
with an at-sign. A handle may be a display name, an unambiguous part of one, or
the character's ID — useful when the display name is spelled differently or
written in another script.

The reserved handle `-` is the null target: `@- <text>` records one message in
the transcript without calling a model or producing a reply, and `/@-` switches
the session into recording mode so plain messages are recorded the same way
until `/@Name` or the target selector switches back. Recorded messages are
durable and reach a later character as shared conversation history, never as a
message addressed to it.

## Workspace configuration

A workspace contains `workspace.toml`, `characters/`, `forums/`, and
`personas/`. `characters/` and `personas/` may use nested grouping directories;
the directory containing a definition file supplies that character or persona's ID.
The `personas/` directory may be empty because the built-in Guest
persona is always available, and is what a forum that names no `default_persona`
speaks as. Persona, character, and forum definitions have a public
`display_name` and may have a one-line `description`.

A forum's `config.toml` can name its starting character with
`default_character = "character-id"`. The ID must be a forum member; when the
setting is omitted, the first member ID in lexicographic order is used. `/@Name`
changes the live session immediately and saves that ID to the forum config, so
the next session in that forum starts with it. The setting is read when a session
opens, so editing the file by hand takes effect without a restart.

`/!Name` selects the persona speaking for the current session. It accepts an
unambiguous, case-insensitive full or partial persona ID or display name, then
saves the selected ID as `default_persona` in the forum config. Later prompts
in the session are attributed to that persona.

`workspace.toml` supplies diagnostic logging settings:

```toml
[logging]
file = "logs/cha.log"
level = "info"
```

Each completed provider request writes its HTTP metadata, provider request ID,
and reported `input_tokens`, `output_tokens`, `cache_read_tokens`, and
`cache_write_tokens` to this diagnostic log. Chat Completions streaming
requests ask for the final usage block explicitly; Responses includes usage in
its completion object. A compatible provider that omits a field is logged as
`unreported` rather than estimated locally.

Each provider lives in `system/providers/<id>/config.toml`. Its file contains
the connection, model, protocol, and authentication-environment settings:

```toml
host = "api.openai.com"
port = 443
https = true
mode = "net"
model = "gpt-5.6-terra"
stream = true
api = "responses"          # responses | chat_completions
web_search = "required"    # required | auto | off
api_key_env = "OPENAI_API_KEY"
cache_retention = "short"  # off | short | long
timeout_s = 600
idle_timeout_s = 60
max_tokens = 4096
# temperature = 0.7        # omitted from requests when unset
```

Generation stops after `timeout_s` overall, or after `idle_timeout_s` without a
single received byte. That idle timer starts at the first byte of the response,
so a model that thinks for minutes before answering is bounded by `timeout_s`
alone. Both timeout values and `max_tokens` must be positive. `max_tokens` is
sent under that name for Chat Completions and as `max_output_tokens` for
Responses, whose value is clamped to at least 16.

For the direct `api.openai.com` host, `cache_retention` other than `off` sends a
stable prompt-cache key for each forum/session/character. For GPT-5.6 and later,
`long` additionally requests implicit caching with the longest currently
supported minimum TTL, 30 minutes, from the Responses API. For `openrouter.ai`,
the same stable value is sent as `session_id` so successive requests retain
provider affinity. These cache fields are omitted for other hosts. `off` omits
CHA's cache hints but does not disable a provider's automatic caching.

Set `base_path` when a compatible provider exposes its API below a path rather
than at the host root. For example, OpenRouter uses `base_path = "/api"`, which
produces `/api/v1/chat/completions`.

A provider config is the only place connection and model settings may appear.
Each character selects exactly one of those configs in its own `character.toml`
with `provider = "<id>"`. Provider keys in workspace, forum-default, and member
configuration are ignored; there is no provider inheritance or override chain.
A missing character provider or provider config stops startup. Provider config
files are loaded with each workspace generation, so edits to their host, model,
or other settings take effect after a workspace reload. The workspace `.env`
is loaded only at process startup, so changing a secret there requires a
restart.

A character's chosen `provider` and `style` can be changed from the browser:
Characters → the character → the row naming it above the description → Settings. Save writes
those keys in the character's `character.toml` (under `characters/`, including
through any grouping directories) and restarts live sessions containing that
character. The built-in Assistant reads `system/assistant/character.toml` but
has no settings screen.

Character appearance is selected in the character definition with
`style = "<id>"`. The matching config lives at
`system/styles/<id>/config.toml`:

```toml
# characters/margaret/character.toml
style = "sans-bold"

# system/styles/sans-bold/config.toml
font = "sans"
weight = "bold"
```

Style configs may contain `font`, `style`, `weight`, `size`, and `text_color`;
omitted fields use the interface defaults. A style reference must resolve during startup.

`web_search` other than `off` requires `api = "responses"`. With
`web_search = "auto"`, the model may search when the prompt and turn warrant
it; `required` forces a search tool call on every generation. To use the Chat
Completions path, set both `api = "chat_completions"` and `web_search = "off"`
explicitly.

Search queries, progress, retrieved pages, annotations, and tool-call details
stay inside the provider interaction. Only the character's synthesized answer
text enters the transcript.

Provider secrets belong in the environment or the workspace `.env`, never in
the application directory. By default, `chaweb` treats the executable directory
as its application root and reads `app.toml` there. `--root` selects another
application root, `--config` selects another config file, and `--host`,
`--port`, and `--workspace` override individual settings. On Reload workspace,
chaweb writes a `chaweb-YYYY-MM-DD-HH-MM.tar.gz` archive before loading the
replacement workspace. Set `backup_dir` in `app.toml` to choose its
destination; omitted, it defaults to the user's home directory.

Linux deployment packages include a minimal `workspace/` directory containing
the default character providers and logging settings. The packaged `start-cha.sh` uses it
automatically, so the application can be started immediately after unpacking.
It starts `chaweb` in the background on port 8086 and returns, so the server
outlives the terminal session; it prints the PID to stop it with and writes
`chaweb.log` beside the executable.
Set `OPENAI_API_KEY` in the process environment before starting it; no `.env`
file is included. The packaged workspace is also the location for any
workspace customization and stored sessions.

`chaweb` loads discovery — the roster, descriptions, and Markdown shown in
Personas, Characters, and Forums — as one validated workspace generation. A
session open re-resolves that forum's character definitions from disk, so a
saved provider or style, and a hand edit to `CHARACTER.md`, member configs, or
the forum context, reach the next session without a reload. To publish added or
removed characters, personas, and forums, send `POST /api/v1/workspace/reload`
with an empty JSON object. It validates and atomically publishes a complete new
generation. Reload first closes every live session; the browser recovery flow
reopens a selected stored session against the published generation. Stored
sessions remain on disk, while Welcome belongs to the generation that created
it and starts empty after a successful reload. A failed reload retains the old
published generation, but the live sessions have already been closed.

Startup validates every configured forum, not only the ones in use. A forum with
an invalid default character, member override, or prompt therefore prevents the
server from starting; the reported error names that forum and its source.

## Build and test

Native configuration currently requires OpenSSL development headers and
libraries on every platform. The Ninja preset fetches the other vendored
dependencies when needed.

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
make web-check
make web-e2e
# Requires configured live-provider credentials:
make itest
```

Browser development is documented in
[webapp/README.md](webapp/README.md). Build a validated Linux release with
`make package-linux VERSION=<version>`; it writes an application directory and
`.tar.gz` archive under `packages/`. See [src/README.md](src/README.md) for the
native architecture.
