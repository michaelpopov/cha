# Application layer

`application/` owns the static workspace this process knows about and the one
operation that turns it into a live session. It contains no HTTP transport or
live-session switching policy.

| Component | Responsibility |
| --- | --- |
| `WorkspaceModel` | Load and own the process's immutable workspace configuration. |
| `open_session()` | Combine a model definition with prepared storage to construct a `SessionController`. |
| `builtins` | Defines Guest, Assistant, the reserved built-in IDs, and embeds the application guide. |

`WorkspaceModel::load()` performs one all-or-nothing startup pass: it validates
the workspace layout, loads personas, character metadata and Markdown, and forum
membership, builds the Guest-inclusive persona roster and Assistant's inventory,
resolves every configured forum's effective `CharacterDefinition` values, and adds
Assistant and Entrance to the public catalogs. After construction it performs no
filesystem reads, so discovery responses and newly opened sessions can never
disagree.

Because every configured forum is resolved at startup, an invalid member
override or prompt fails the server's startup rather than waiting for someone to
open that forum. The error names the forum and its source directory.

Full `CharacterDefinition` values contain separate public `CharacterMetadata`
and private `CompletionConfig` members. Completion configuration may carry inline
keys, so they are not on the public model API. `open_session()` is a friend of
`WorkspaceModel` and the one production caller allowed to copy them; the public
API exposes only personas, character metadata and Markdown, forum information,
and the startup-only `ForumSessionDirectory` values used to build
`SessionRepository`.

`open_session()` is deliberately small: find the forum, copy its preloaded
definitions, ask `SessionRepository` to prepare storage, build the
`SessionDescriptor`, and construct the controller with the model's shared
persona roster. Entrance and Welcome need no branch — Entrance is an ordinary
forum in the model and Welcome an ordinary prepared session in the repository.

This directory may depend on `session/`, `agents/`, `transcript/`, and `util/`.
It must not depend on `ui/`, `apps/`, or HTTP types.
