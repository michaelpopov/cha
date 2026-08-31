# Character request values and model context

`characters/` contains the runtime values retained by one provider request and
the model-history projection used to build that request. Workspace file parsing
belongs exclusively to `Workspace::load()`.

`CharacterDefinition` owns one request's character metadata, resolved provider
configuration, character prompt, description, and system prompt. Production
constructs it from the current `Workspace` when generation starts. The request
keeps that immutable input until it finishes, so a later committed
configuration publication cannot change work already in flight.

| Source | Responsibility |
| --- | --- |
| `character_config.*` | Request-time provider enums, backend configuration, and endpoint projection. |
| `character.*` | Request-owned character definition, runtime reporting, and identity validation. |
| `model_context.*` | Owning model-history projection and request input. |

Tests for workspace parsing and resolution live in
`tests/application/unit_workspace.cpp`; model-history tests live in
`tests/agents/unit_model_context.cpp`.

This directory may depend on `chat/` and `util/`. It must not depend on
`providers/`, `session/`, `workspace/`, or `web/`.
