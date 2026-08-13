# Workspace layer

`workspace/` owns the static workspace this process knows about and the one
operation that turns it into a live session. It contains no HTTP transport or
live-session switching policy.

| Component | Responsibility |
| --- | --- |
| `WorkspaceDefinition` | Load and own the process's immutable workspace configuration. |
| `open_session()` | Combine a model definition with prepared storage to construct a `SessionController`. |
| `builtins` | Defines Guest, Assistant, the reserved built-in IDs, and embeds the application guide. |

`WorkspaceDefinition::load()` performs one all-or-nothing startup pass: it validates
the workspace layout, loads personas, character metadata and Markdown, and forum
membership, builds the Guest-inclusive persona roster and Assistant's inventory,
checks that each forum's default character and `default_persona` name forum
members and personas respectively,
resolves every configured forum's effective `CharacterDefinition` values, and adds
Assistant and Entrance to the public catalogs. Definitions are never re-read, so
every discovery response and every session sees the same characters, personas
and prompts.

A forum's default character is the one setting that stays live.
`forum_default_character()` re-reads it from the forum's retained `config.toml`
whenever a session opens or the lobby is projected, and
`persist_forum_default_character()` writes it back there when `/@` succeeds, so a
change applies to the next session without a restart. The re-read is lenient
where startup is strict: an unreadable file, or one naming a character this forum
did not load, logs a warning and keeps the value loaded at startup, because a
session must still open. Built-in forums have no config file and always keep it.

Because every configured forum is resolved at startup, an invalid member
override or prompt fails the server's startup rather than waiting for someone to
open that forum. The error names the forum and its source directory.

Full `CharacterDefinition` values contain separate public `CharacterMetadata`
and private `ModelBackendConfig` members. Backend configuration may carry inline
keys, so they are not on the public model API. `open_session()` is a friend of
`WorkspaceDefinition` and the one production caller allowed to copy them; the public
API exposes only personas, character metadata and Markdown, forum information,
and the startup-only `ForumSessionDirectory` values used to build
`SessionRepository`.

`open_session()` is deliberately small: find the forum, copy its preloaded
definitions, ask `SessionRepository` to prepare storage, build the
`SessionDescriptor`, and construct the controller with the workspace persona
roster and that forum's configured current persona. This lets `/!Name` switch
the live session's attribution while preserving the selected ID in its forum
config. It also supplies callbacks that let the live-session owner save changed
character and persona defaults without exposing workspace paths to web routes.
Entrance and Welcome need no branch — Entrance is an ordinary
forum in the model and Welcome an ordinary prepared session in the repository.

This directory may depend on `session/`, `agents/`, `chat/`, and `util/`.
It must not depend on `web/`, executable wiring, or HTTP types.
