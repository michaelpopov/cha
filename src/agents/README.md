# Character definitions and model context

`agents/` loads character and provider configuration, assembles immutable
character definitions, and projects owned model history for provider requests.
It performs no provider transport and knows nothing about sessions, workspaces,
HTTP routes, or browser presentation.

## Character and provider input

`CharacterDefinition` carries a `ProviderSelection`: the provider ID plus its
resolved `ModelBackendConfig`. A request retains a shared immutable definition,
so a later reload under the same provider ID cannot change work already in
flight. `CharacterRuntimeInfo` is derived directly from definitions for
session-facing provider reporting; it does not require a provider client.

Every referenced provider requires a configured model. Workspace loading and
session-open definition reload validate a non-empty `api_key_env` without
retaining its value. Request execution resolves the actual credential from the
immutable configuration snapshot. CHA does not discover models or call
`/models`.

## Source map

| Source | Responsibility |
| --- | --- |
| `character_config.*` | Provider and character configuration parsing, endpoint projection, and static validation. |
| `character.*` | Immutable character definitions, runtime information, prompt assembly, and template expansion. |
| `model_context.*` | Owning model-history projection and request input. |

## Tests

`tests/agents/unit_config_loader.cpp` covers provider and character
configuration. `unit_character_definition_loader.cpp` covers definitions,
prompt assembly, and public runtime information. `unit_model_context.cpp`
covers immutable history projection.

This directory may depend on `chat/` and `util/`.
It must not depend on `providers/`, `session/`, `workspace/`, or `web/`.
