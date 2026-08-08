# CHA

CHA is a C++20 browser application for chatting with OpenAI-compatible
chat-completions servers. The `chaweb` process serves the browser client and its
HTTP/SSE API.

## Start chatting

For a staged development run:

```sh
make run
```

Open the address printed by the launcher. CHA starts in the process-local
**Entrance / Welcome** conversation as **Guest** with **Assistant**. Use the
browser to choose a persona, inspect forums, create or reopen a stored session,
and select a forum character.

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
| `/@Name` | Change the default character. |
| `/stop` | Stop generation. |
| `/exit` | Close the live session. |

Leading `@Name` addresses a prompt to one character. `@@` starts literal text
with an at-sign.

## Workspace configuration

A workspace contains `workspace.toml`, `characters/`, `forums/`, and
`personas/`. The `personas/` directory may be empty because the built-in Guest
persona is always available. Persona, character, and forum definitions have a
public `display_name` and may have a one-line `description`.

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

Provider secrets belong in the environment or the workspace `.env`, never in
the application directory. `chaweb` reads listener and workspace settings from
`app.toml` beside the executable; `--host`, `--port`, `--workspace`, and
`--root` override them.

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
