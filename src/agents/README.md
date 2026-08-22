# Provider execution

`agents/` loads character definitions, projects immutable model history, and
performs provider HTTP requests. It does not know about sessions, HTTP routes,
or browser presentation.

## Ownership

`Providers` is constructed once by the composition root. `make_request()`
creates one `ProviderRequest`, registers it, and immediately launches one
detached worker. The request owns its immutable input, cancellation flag,
event queue, shared wake notifier, request-local `ProviderClient`, curl easy
handle, and resolved credential. The active registry retains the request until
the worker has published its terminal event and destroyed transport resources.

Sessions retain request handles only while they present events. They never own
a provider client, curl handle, generation thread, or provider queue. The
worker captures only the request and internal registry state, never a session
or a raw `Providers*`; after unregistering, its tail only releases shared
state.

Each request has an independent client, curl handle, cancellation flag, and
`ConcurrentQueue<GenerationEvent>`. There is no generation pool, task queue,
lease, cache, or concurrency limit. A multicast is simply several independent
requests sharing one immutable `SharedModelHistory`; the session chooses the
presentation order.

`shutdown()` closes admission, cancels a snapshot of active requests, and
waits until their transport has quiesced. It is safe to call repeatedly.

## Character and provider input

`CharacterDefinition` carries a `ProviderSelection`: the provider ID plus its
resolved `ModelBackendConfig`. A request retains a shared immutable definition,
so a later reload under the same provider ID cannot change work already in
flight. `CharacterRuntimeInfo` is derived directly from definitions for
session-facing reporting and style-reset baselines; it does not require a
provider client.

Every referenced provider requires a configured model. Workspace loading and
session-open definition reload validate a non-empty `api_key_env` without
retaining its value. Each worker resolves the actual credential independently
when it creates its request-local client. CHA does not discover models or call
`/models`.

## Source map

| Source | Responsibility |
| --- | --- |
| `character_config.*` | Provider and character configuration parsing and static validation. |
| `character.*` | Immutable character definitions, runtime information, prompt assembly, and template expansion. |
| `model_context.*` | Owning model-history projection and request input. |
| `providers.*` | Request-owned workers, registry supervision, cancellation, and event delivery. |
| `provider_client.*` | One request-local OpenAI-compatible curl transport. |
| `model_backend.h` | Small backend seam plus prepared-request and result types. |
| `provider_response.*`, `responses_api.*`, `sse_framer.*` | Provider protocol decoding. |

## Diagnostics

Provider lifecycle logs contain provider ID, request ID, internal registry
token, configured model, active count, and duration. They never contain a
credential, complete configuration, prompt, transcript content, or response
body.

## Tests

`tests/agents/unit_providers.cpp` covers immediate execution, terminal-event
delivery, cancellation, launch/shutdown races, request snapshot isolation,
registry lifetime, notifier lifetime, and transport destruction ordering.
`unit_provider_client.cpp` covers HTTP request/response behavior without model
discovery.
