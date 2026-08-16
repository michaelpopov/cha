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
| `/@-` | Enter recording mode: plain messages are saved to the transcript but not sent to a model. Session-local — `-` is never saved as the forum default. |
| `/!Name` | Change and save the forum's current persona. |
| `/provider <name>` | Override the current character's provider for this session only; `/provider` reports the override and `/provider default` restores the configured provider. Nothing is saved. |
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
`personas/`. The `personas/` directory may be empty because the built-in Guest
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

`workspace.toml` selects the default provider and supplies diagnostic logging
settings:

```toml
[provider]
provider = "terra"

[logging]
file = "logs/cha.log"
level = "info"
```

Each completed provider request writes its HTTP metadata and the provider's
`input_tokens` and `output_tokens` to this diagnostic log. Chat Completions
streaming requests ask for the final usage block explicitly; Responses includes
usage in its completion object. A compatible provider that omits either field
is logged as `unreported` rather than estimated locally.

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
```

Set `base_path` when a compatible provider exposes its API below a path rather
than at the host root. For example, OpenRouter uses `base_path = "/api"`, which
produces `/api/v1/chat/completions`.

A provider config is the only place these settings may appear. Character
definitions, forum `character_defaults.toml`, and member overrides select one
with `provider = "<id>"` and nothing else; writing `host`, `model`, or any other
provider setting into those files is rejected at startup, as is a reference
whose config file is missing.

Selections are read in order — workspace `[provider]`, character definition,
forum defaults, member override — and the highest one that names a provider
supplies the whole backend. Selecting a provider replaces the layer below
outright rather than merging with it, so each provider config must be complete
on its own. Provider config files themselves are loaded at process startup, so
edits to host, model, or credentials take effect only after a restart.

A character's chosen `provider` and `style` can be changed from the browser:
Characters → the character → the row naming it above the description → Settings. Save writes
those keys in `characters/<id>/character.toml` and restarts live sessions the
change can affect. A forum that sets its own provider is named under the picker
and is not restarted for a provider-only save. The built-in Assistant has no
config file and no settings screen.

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
the application directory. `chaweb` reads listener and workspace settings from
`app.toml` beside the executable; `--host`, `--port`, `--workspace`, and
`--root` override them.

Linux deployment packages include a minimal `workspace/` directory containing
the default provider and logging settings. The packaged `start-cha.sh` uses it
automatically, so the application can be started immediately after unpacking.
Set `OPENAI_API_KEY` in the process environment before starting it; no `.env`
file is included. The packaged workspace is also the location for any
workspace customization and stored sessions.

`chaweb` loads discovery — the roster, descriptions, and Markdown shown in
Personas, Characters, and Forums — once at startup. A session open re-resolves
that forum's character definitions from disk, so a saved provider or style, and
a hand edit to `CHARACTER.md`, member configs, or the forum context, reach the
next session without a restart. Discovery does not follow those files: the
roster and detail Markdown stay at the startup copy until `chaweb` is restarted.
Stored sessions remain dynamic and appear in the lobby without a restart.

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
