# Interfaces

`interfaces/` contains adapters between external interaction protocols and the
structured application API. An interface may interpret input, manage
transport-specific state, and render application or conversation values, but it
does not own chat, agent-execution, or persistence policy.

## Current adapters

| Directory | Responsibility |
| --- | --- |
| `text/` | Shared slash-command and mention syntax, with dispatch to `ChatCoordinator`. |
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
- narrowly scoped `util/` helpers where protocol parsing needs them;
- roster values from `agents/` when a presentation needs agent names or
  identity (for example addressed multi-agent transcripts).

Interfaces must not open session repositories, load workspace layout files, or
call completion backends. If an adapter needs a new operation, introduce it
through `application/` rather than reimplementing persistence or execution in
the adapter.

## Adding another interface

A future `http/` adapter should translate HTTP routes, authentication, request
bodies, and response/event framing around the existing application operations.
Browser-specific DTOs belong in that adapter. Conversation entries and
generation status may be read directly when their shape is already suitable;
session-persistence and agent-runtime execution types should not become
transport contracts.

Executable wiring belongs in `apps/`, not in an interface directory.
