# Application discovery

`application/` supplies immutable, browser-facing startup data and the built-in
Welcome conversation. It contains no HTTP transport or live-session switching
policy.

| Component | Responsibility |
| --- | --- |
| `WorkspaceSnapshot` | Validates and captures workspace personas, characters, and forums once. |
| `EffectivePersonas` | Adds the built-in Guest to the workspace persona roster. |
| `WorkspaceInventory` | Produces deterministic public reference data for Assistant. |
| `builtins` | Defines Guest, Assistant, Entrance, and embeds the application guide. |
| `WebDiscovery` | Combines the validated snapshot with built-ins for browser discovery. |
| `WelcomeStorage` | Owns the process-local SQLite storage used by Entrance / Welcome. |

`SessionRegistry` constructs Welcome from these values and opens ordinary
workspace sessions through the keyed `Workspace` APIs. Existing stored
conversations formerly created in Entrance are intentionally outside the web
discovery and lifecycle.

This directory may depend on `session/`, `agents/`, `transcript/`, and `util/`.
It must not depend on `ui/`, `apps/`, or HTTP types.
