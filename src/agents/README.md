# Agent runtime

`agents/` owns the definition, identity, selection, and execution of configured
chat agents. It also contains the completion-provider boundary and the concrete
HTTP client used by production agents.

## Structure

### Agent metadata and identity

| Source | Responsibility |
| --- | --- |
| `config.h` | Connection, model, streaming, authentication, and reasoning settings. |
| `agent_definition.h` | A complete agent configuration paired with its effective system prompt. |
| `agent_info.h` | The public identity and descriptive information exposed by a backend. |
| `agent_identity.*` | Validation of stable agent IDs and display names. |
| `agent_roster.*` | Ordered roster validation, lookup, and handle resolution. |

### Execution and protocol

| Source | Responsibility |
| --- | --- |
| `agent_protocol.h` | Typed completion requests and correlated delta, completion, cancellation, and failure events. |
| `agent_registry.*` | The single execution thread, backend routing, cancellation, and request/event channels. |
| `event_channel.h` | A typed thread-safe queue with a pollable Linux notification descriptor. |

### Completion backends

| Source | Responsibility |
| --- | --- |
| `completion_backend.h` | Synchronous backend interface and prepared request/result types. |
| `completion_client.*` | HTTP completion transport, model discovery, JSON/SSE parsing, and protocol diagnostics. |
| `agent_context.*` | Projection of typed transcript entries into provider-facing system, user, and assistant messages. |

## Runtime behavior

`AgentRoster` is the authority for a non-empty roster, unique IDs, unique
case-folded names, and user handle resolution. `AgentRegistry` maintains the
same ordering between roster entries and completion backends.

The registry accepts at most one outstanding request. Its worker thread
prepares a request from a short-lived conversation read view, performs one
synchronous backend call, and publishes typed events for the application
thread. Cancellation is cooperative. Shutdown closes the request side, joins
the worker, and only then closes the event side so accepted work receives a
terminal event.

`CompletionBackend` is the test and provider abstraction. `CompletionClient`
implements it for OpenAI-compatible HTTP endpoints, including streaming SSE
and non-streaming responses. Provider fragments remain uncorrelated
`CompletionDelta` values until the registry attaches the active request ID.

## Dependencies

The agent runtime may depend on:

- `conversation/` for transcript views, request IDs, and completion content;
- `util/` for shared text and identity rules;
- libcurl and nlohmann/json in the concrete HTTP client;
- Linux `eventfd` and `poll` in `EventChannel`.

It must not depend on `storage/`, `application/`, or `interfaces/`. In
particular, agent value types live here, but reading those values from disk is
the responsibility of `storage/`.
