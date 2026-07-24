# Source tree

This tree contains the production C++ sources for Cha. It is organized by
logical responsibility so ownership and dependency direction remain visible as
the application grows.

The directories are not separate libraries. CMake compiles every production
translation unit except `apps/tui_main.cpp` into the single `cha_core` target;
the directory boundaries are architectural rather than link-time boundaries.

## Directory map

| Directory | Responsibility |
| --- | --- |
| `util/` | Small, low-level helpers shared by otherwise unrelated components. |
| `conversation/` | The typed transcript model and completion-content values. |
| `agents/` | Agent identity, selection, execution, and completion transport. |
| `storage/` | Workspace loading and durable SQLite session storage. |
| `application/` | Reusable use cases and chat-session coordination. |
| `interfaces/` | User-facing adapters that translate external input and render application state. |
| `apps/` | Executable composition roots. |

Each directory has its own `README.md` with a more detailed inventory and
dependency contract.

## Dependency direction

An arrow means “depends on”:

```text
apps
  |--> interfaces/terminal --+--> interfaces/text --> application
  |                          +--> application
  |                          `--> conversation
  |
  `--> application ----------+--> agents -------> conversation
                             +--> storage ------> conversation
                             |       `---------> agents
                             `--> conversation

agents, storage, interfaces/text, and apps may also depend on util.
```

Interfaces may consume presentation-safe application and conversation values,
but must not load workspace files, access session repositories, or invoke agent
backends directly. The application layer owns those use-case boundaries.

## Source conventions

- Project headers are included from the `src/` include root, for example
  `"application/chat_coordinator.h"`.
- Headers include the standard and project headers required by their own public
  declarations; they do not rely on transitive includes.
- Public data types live with the behavior that owns their meaning. Disk
  loaders for agent types therefore live in `storage/`, while `Config` and
  `AgentDefinition` live in `agents/`.
- Tests mirror this tree under `tests/`. Cross-layer scenarios belong under
  `tests/integration/`.

The broader runtime and persistence design is documented in
[`docs/cha.md`](../docs/cha.md).
