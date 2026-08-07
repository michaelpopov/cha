# UI

`ui/` contains the supported browser frontend and the textual chat-input grammar
it reuses.

| Directory | Responsibility |
| --- | --- |
| `web/` | HTTP routes, JSON protocol, SSE projection, session registry, owner-thread runtime, and browser lifecycle. |
| `text/` | Controller commands, `@mention` addressing, and multicast parsing. |

HTTP request threads never call live controllers. Routes validate protocol
input and enqueue commands to `WebSessionRuntime`; the session owner thread
executes those commands and projects state into immutable snapshots or proven
append events. `SseMailbox` provides the bounded handoff to the browser.

Code under `ui/` may depend on application discovery, session and transcript
types, and narrow utility helpers. It must not expose storage internals as API
contracts or access provider backends directly.

See [web/README.md](web/README.md) and [text/README.md](text/README.md).
