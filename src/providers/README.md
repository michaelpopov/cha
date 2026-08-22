# Provider execution

`providers/` owns request-local provider transport, protocol decoding,
cancellation, event delivery, and process-level supervision. It consumes
immutable character and model-context input from `agents/` and never borrows a
session or browser object.

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
or a raw `Providers*`.

Each request has an independent client, curl handle, cancellation flag, and
`ConcurrentQueue<GenerationEvent>`. There is no generation pool, task queue,
lease, cache, or concurrency limit. A multicast is several independent
requests sharing one immutable `SharedModelHistory`; the session chooses the
presentation order.

`shutdown()` closes admission, cancels a snapshot of active requests, and
waits until their transport and final diagnostics have quiesced. It is safe to
call repeatedly.

## Source map

| Source | Responsibility |
| --- | --- |
| `providers.*` | Request-owned workers, registry supervision, cancellation, and event delivery. |
| `generation_event.h` | Request output events shared by provider workers and session consumers. |
| `provider_client.*` | One request-local OpenAI-compatible curl transport. |
| `model_backend.h` | Backend and streaming-decoder seams plus prepared-request and result types. |
| `chat_completions_api.*` | Chat Completions request encoding and response decoding. |
| `responses_api.*` | Responses API request encoding and response decoding. |
| `sse_framer.*` | Protocol-neutral server-sent event framing. |

## Diagnostics

Provider lifecycle logs contain provider ID, session and request IDs, internal
registry token, configured model, active count, and duration. They never
contain a credential, complete configuration, prompt, transcript content, or
response body.

## Tests

`tests/providers/unit_providers.cpp` covers immediate execution,
terminal-event delivery, cancellation, launch/shutdown races, request snapshot
isolation, registry and notifier lifetime, and transport destruction ordering.
The other tests in `tests/providers/` cover curl request/response behavior and
provider protocol decoding without model discovery.

This directory may depend on `agents/`, `chat/`, and `util/`.
It must not depend on `session/`, `workspace/`, `web/`, or executable wiring.
