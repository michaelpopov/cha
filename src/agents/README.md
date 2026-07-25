# Agent runtime

`agents/` owns configured chat agents: configuration loading, public identity,
roster selection, completion backends, and the single execution thread that
talks to providers.

## Structure

| Source | Responsibility |
| --- | --- |
| `config.*` | Connection, model, streaming, authentication, and reasoning settings, plus TOML loading into `Config`. |
| `agent.*` | `AgentDefinition` / `AgentInfo` / context projection, ID and name validation, and typed completion request/event protocol. |
| `agent_registry.*` | `AgentRoster` (ordered validation, lookup, handle resolution) and `AgentRegistry` (execution thread, backend routing, cancellation, request/event channels). |
| `completion_backend.h` | Synchronous backend interface and prepared request/result types. |
| `completion_client.*` | HTTP completion transport, model discovery, JSON/SSE parsing, and protocol diagnostics. |

## Runtime behavior

`AgentRoster` is the authority for a non-empty roster, unique IDs, unique
case-folded names, and user handle resolution. `AgentRegistry` keeps the same
ordering between roster entries and completion backends.

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

`project_agent_context()` turns typed transcript entries into provider-facing
system, user, and assistant messages for one agent participant.

## Dependencies

The agent runtime may depend on:

- `conversation/` for transcript views, request IDs, and completion content;
- `util/` for shared text helpers and the pollable event channel;
- libcurl and nlohmann/json in the concrete HTTP client;
- toml++ in the persona configuration loader.

It must not depend on `application/` or `interfaces/`. Workspace path
discovery stays in `application/`; once persona and room directories are known,
agents own loading `Config` and `AgentDefinition` from those paths.
