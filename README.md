# CHA

CHA is a C++20 browser application for chatting with OpenAI-compatible
OpenAI-compatible model servers. The `chaweb` process serves the browser client and its
HTTP/SSE API.

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
| `/!Name` | Change and save the forum's current persona. |
| `/stop` | Stop generation. |
| `/exit` | Close the live session. |

Leading `@Name` addresses a prompt to one character. `@@` starts literal text
with an at-sign. A handle may be a display name, an unambiguous part of one, or
the character's ID — useful when the display name is spelled differently or
written in another script.

## Workspace configuration

A workspace contains `workspace.toml`, `characters/`, `forums/`, and
`personas/`. The `personas/` directory may be empty because the built-in Guest
persona is always available, and is what a forum that names no `default_persona`
speaks as. Persona, character, and forum definitions have a public
`display_name` and may have a one-line `description`.

A forum's `config.toml` can name its starting character with
`default_character = "character-id"`. The ID must be a forum member; when the
setting is omitted, the first member ID in lexicographic order is used. `/@Name`
changes the live session immediately and saves that ID to the forum config, so
the next session in that forum starts with it. The setting is read when a session
opens, so editing the file by hand takes effect without a restart; the rest of
the workspace is still read only at startup.

`/!Name` selects the persona speaking for the current session. It accepts an
unambiguous, case-insensitive full or partial persona ID or display name, then
saves the selected ID as `default_persona` in the forum config. Later prompts
in the session are attributed to that persona.

`workspace.toml` supplies provider and diagnostic logging settings:

```toml
[provider]
host = "api.openai.com"
port = 443
https = true
mode = "net"
model = "gpt-5.6-terra"
stream = true
api_key_env = "OPENAI_API_KEY"

[logging]
file = "logs/cha.log"
level = "info"
```

Optional provider fields select the OpenAI protocol and hosted web search.
By default, every character uses the Responses API with mandatory web search:

```toml
api = "responses"          # responses | chat_completions
web_search = "required"    # required | auto | off
```

Set `base_path` when a compatible provider exposes its API below a path rather
than at the host root. For example, OpenRouter uses `base_path = "/api"`, which
produces `/api/v1/chat/completions`.

`web_search` other than `off` requires `api = "responses"`. With
`web_search = "auto"`, the model may search when the prompt and turn warrant
it; `required` forces a search tool call on every generation. These fields
overlay through workspace `[provider]`, character definitions, forum defaults,
and member overrides. Configuration is loaded at process startup, so changes
take effect only after a restart.

To use the legacy Chat Completions path for a character or provider layer, set
both `api = "chat_completions"` and `web_search = "off"` explicitly.

Search queries, progress, retrieved pages, annotations, and tool-call details
stay inside the provider interaction. Only the character's synthesized answer
text enters the transcript.

Provider secrets belong in the environment or the workspace `.env`, never in
the application directory. `chaweb` reads listener and workspace settings from
`app.toml` beside the executable; `--host`, `--port`, `--workspace`, and
`--root` override them.

Linux deployment packages include a minimal `workspace/` directory containing
the default provider and logging settings. The packaged `start-cha.sh` uses it
automatically, so the application can be started immediately after unpacking.
Set `OPENAI_API_KEY` in the process environment before starting it; no `.env`
file is included. The packaged workspace is also the location for any
workspace customization and stored sessions.

`chaweb` reads the static workspace once, at startup. Edits to `workspace.toml`,
personas, characters, forums, member overrides, or prompts take effect only
after the server is restarted; stored sessions remain dynamic and appear in the
lobby without a restart.

Startup validates every configured forum, not only the ones in use. A forum with
an invalid default character, member override, or prompt therefore prevents the
server from starting; the reported error names that forum and its source.

## Build and test

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
[webapp/README.md](webapp/README.md). Packaging and
upgrade instructions are in
[docs/linux-webapp-package.md](docs/linux-webapp-package.md). See
[src/README.md](src/README.md) for the native architecture.
