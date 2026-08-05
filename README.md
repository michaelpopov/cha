# cha

`cha` is a C++20 chat client for OpenAI-compatible chat-completions servers.
It provides the full-screen `cha` terminal UI, the line-oriented `chacon`
console, and the existing key-based `chaweb` frontend.

## Start chatting

Both terminal applications start immediately in the built-in help conversation:

```text
persona: Guest
forum: Entrance
session: Welcome
agent: Assistant
```

`Welcome` is a disposable SQLite-backed conversation for this process run.
It remains available if you switch away and return during that run, but a new
process receives a fresh Welcome. Sessions created with `/create` are durable,
including those in Entrance. Durable Entrance sessions are application-managed
stored chats, separate from workspace forum configuration; Welcome is not a
stored-session list entry.

Assistant has the embedded [application guide](docs/application-guide.md) and
a startup snapshot of the workspace. It can explain the application, but the
following commands are also available without a completion request:

| Command | Purpose |
| --- | --- |
| `/help` | List the complete command set. |
| `/personas` | List workspace personas. |
| `/iam <persona>` | Change the author for future prompts. |
| `/forums` | List workspace forums. |
| `/sessions <forum>` | List stored sessions in a forum. |
| `/open <forum> <session>` | Switch to a stored session. |
| `/create <forum> <session>` | Create a durable session and switch to it. |
| `/clear`, `/hide-on`, `/hide`, `/hide-off` | Manage visible and model context history. |
| `/mcast`, `/agents`, `/info`, `/@Name` | Select and inspect agents. |
| `/stop`, `/exit` | Stop generation or leave the application. |

Commands use public names only and look them up with ASCII case folding. Quote
names containing whitespace, for example:

```text
/iam "Technical Writer"
/create "The Stoics Forum" "Questions about control"
/open Entrance Welcome
```

Directory names, participant keys, and database filenames are implementation
details; terminal commands, listings, notices, and diagnostics do not use them.

## Workspace configuration

A workspace contains `app.toml`, `characters/`, `forums/`, and `personas/`.
The `personas/` directory may be empty: the built-in Guest persona keeps the
terminal roster non-empty. Workspace personas, character definitions, and
forums have a public `display_name` and may have an optional one-line
`description`; Assistant uses those descriptions in its inventory.

`app.toml` keeps the web listener separate from the shared provider layer:

```toml
# Web listener only.
host = "127.0.0.1"
port = 8080

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

`[provider]` is the lowest configuration layer for Assistant and every
workspace character, including web characters. Character definitions and forum
overrides need only specify fields that intentionally differ. Provider secrets
belong in the environment, not in the workspace. The web frontend inherits
this provider configuration but does not gain the terminal built-ins or slash
navigation workflow.

Public names preserve authored spelling and compare with ASCII folding. They
must be valid UTF-8, non-empty, control-free, and free of leading or trailing
whitespace. Session names are unique within a forum. Existing stored sessions
remain compatible; their labels are now their public session names.

## Console

```text
chacon [--color=auto|always|never]
chacon --check
```

`--check` validates the whole workspace and exits before constructing a
provider, built-in environment, or session. The removed `--persona`, `--forum`,
`--session`, `--new`, `--list-forums`, and `--list-sessions` flags are not
supported. For a migrated script, start `chacon` and send `/open` or `/create`
as normal input instead.

Console transcript output is append-only on stdout; prompts and application
notices use stderr. The terminal UI renders application lists in a transient
overlay, so lists never become part of a session transcript.

## Build and test

```bash
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
make itest
```

See [src/README.md](src/README.md) for architecture and the per-layer READMEs
for implementation contracts.
