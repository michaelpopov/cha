# Agent runtime

`agents/` owns the definition, identity, selection, and execution of configured
chat agents. It also contains the completion-provider boundary and the concrete
HTTP client used by production agents.

## Structure

### Agent metadata and identity

| Source | Responsibility |
| --- | --- |
| `config.*` | Connection, model, streaming, authentication, and reasoning settings, plus TOML loading into `Config`. |
| `agent.*` | Agent configuration with system prompt (loaded from persona/room files), public `AgentInfo`, ID/name validation, transcript projection, and typed completion request/event protocol. |

### Execution and protocol

| Source | Responsibility |
| --- | --- |
| `agent_registry.*` | Ordered roster validation/lookup/handle resolution, the single execution thread, backend routing, cancellation, and request/event channels. |

### Completion backends

| Source | Responsibility |
| --- | --- |
| `completion_backend.h` | Synchronous backend interface and prepared request/result types. |
| `completion_client.*` | HTTP completion transport, model discovery, JSON/SSE parsing, and protocol diagnostics. |

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
- `util/` for shared text helpers and the pollable event channel;
- libcurl and nlohmann/json in the concrete HTTP client;
- toml++ in the persona configuration loader.

It must not depend on `storage/`, `application/`, or `interfaces/`. Workspace
path discovery stays in `storage/`; once persona and room directories are known,
agents own loading `Config` and `AgentDefinition` from those paths.
