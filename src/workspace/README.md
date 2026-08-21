# Workspace layer

`workspace/` owns the static workspace this process knows about and the one
operation that turns it into a live session. It contains no HTTP transport or
live-session switching policy.

| Component | Responsibility |
| --- | --- |
| `WorkspaceDefinition` | One immutable workspace discovery snapshot; re-resolve a forum's character definitions when a session opens. |
| `WorkspaceRuntime` | Build, publish, and retain complete workspace generations for chaweb reloads. |
| `open_session()` | Combine a model definition with prepared storage to construct a `SessionController`. |
| `builtins` | Defines Guest, Assistant, the reserved built-in IDs, and embeds the application guide. |

`WorkspaceDefinition::load()` performs one all-or-nothing generation pass: it validates
the workspace layout, loads personas, character metadata and Markdown, and forum
membership, builds the Guest-inclusive persona roster and Assistant's inventory,
checks that each forum's default character and `default_persona` name forum
members and personas respectively,
resolves every configured forum's effective `CharacterDefinition` values, and adds
Assistant and Entrance to the public catalogs. Discovery catalogs stay at that
generation copy. A forum's character definitions are re-resolved from disk when a
session opens, so a saved provider or style — and any other file that loader
reads — reaches the next open. The characters directory and each forum directory
are retained so those later reads can find the files.

`WorkspaceRuntime` owns the current immutable generation. Its `reload()` builds
a replacement definition and repository while the previous generation remains
available, then publishes the replacement only after both succeed. An
application route shuts down the existing live sessions after publication; their
opened-session lifetime retains the generation their controller borrows until
the controller has released it. This is why an invalid workspace reload has no
effect on current discovery or live sessions.

A forum's default character is the one setting that stays live.
`forum_default_character()` re-reads it from the forum's retained `config.toml`
whenever a session opens or the lobby is projected, and
`persist_forum_default_character()` writes it back there when `/@` succeeds, so a
change applies to the next session without a restart. The re-read is lenient
where startup is strict: an unreadable file, or one naming a character this forum
did not load, logs a warning and keeps the value loaded at startup, because a
session must still open. Built-in forums have no config file and always keep it.

`character_settings()` likewise re-reads a character's `character.toml` on every
call so a hand edit reaches the next GET, and is forgiving for the same reason:
a file it cannot parse, or one whose `provider` or `style` is not a string,
reports no settings at all rather than throwing. The description that GET also
serves does not depend on these keys and must not fail with them, and settings
that cannot be read leave the character un-writable rather than being published
as unset. `available_providers()` and
`available_styles()` list the named configs that actually load —
`load_named_provider()` / `load_named_style()` — and drop one that throws rather
than failing the list. Labels are derived from the directory name. A provider
option is only an id and a label; a style option also carries the resolved
appearance. `character_config_path()` is empty for the built-in Assistant.

`write_character_settings()` compares and rewrites `provider` and `style` in one
parse-write cycle under the same mutex as the forum-config writes, and reports
which fields that document actually changed. It requires and validates the
provider name before writing, and validates an optional style, so a config that
cannot run is never recorded and the file is left untouched. Only `style` may
be erased. A character with no readable config is rejected rather than created
or overwritten.

When a session opens, `copy_definitions_for()` re-runs the forum's full
definition loader. A broken hand edit logs a warning, returns the startup copy,
and sets a notice on `OpenedSession` so the chat can say the session is running
startup settings. Built-in forums have no directory and keep the startup copy.

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

`open_session()` is deliberately small: find the forum, re-resolve its
definitions (or take the startup copy and notice), ask `SessionRepository` to
prepare storage, build the `SessionDescriptor`, and construct the controller
with the workspace persona roster and that forum's configured current persona. This lets `/!Name` switch
the live session's attribution while preserving the selected ID in its forum
config. It also supplies callbacks that let the live-session owner save changed
character and persona defaults without exposing workspace paths to web routes.
Entrance and Welcome need no branch — Entrance is an ordinary
forum in the model and Welcome an ordinary prepared session in the repository.

This directory may depend on `session/`, `agents/`, `chat/`, and `util/`.
It must not depend on `web/`, executable wiring, or HTTP types.
