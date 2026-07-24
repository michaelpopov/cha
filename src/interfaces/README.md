# Interfaces

`interfaces/` contains adapters between external interaction protocols and the
structured application API. An interface may interpret input, manage
transport-specific state, and render application or conversation values, but it
does not own chat, agent-execution, or persistence policy.

## Current adapters

| Directory | Responsibility |
| --- | --- |
| `text/` | Shared slash-command and mention syntax, with application dispatch. |
| `terminal/` | Ncurses startup selection, event polling, input editing, transcript rendering, and the interactive session loop. |

The split lets terminal input reuse a textual protocol without making command
grammar part of the application layer. It also leaves future interfaces free
to use structured operations directly.

## Dependency contract

Interfaces may depend on:

- `application/` for use cases, coordinator operations, status, and
  interface-safe summaries;
- `conversation/` for presentation-ready transcript values;
- lower-level interface adapters, such as `terminal/` using `text/`;
- narrowly scoped `util/` helpers where protocol parsing needs them.

Interfaces must not depend directly on `agents/` or `storage/`. If an adapter
needs a new operation, that operation should normally be introduced through
`application/` rather than implemented by loading files, opening a repository,
or calling a completion backend from the adapter.

## Adding another interface

A future `http/` adapter should translate HTTP routes, authentication, request
bodies, and response/event framing around the existing application operations.
Browser-specific DTOs belong in that adapter. Conversation entries and
generation status may be read directly when their shape is already suitable;
storage and agent-runtime types should not be exposed as transport contracts.

Executable wiring belongs in `apps/`, not in an interface directory.
