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
| `agents/` | Agent configuration, roster/execution, and completion transport. |
| `application/` | Workspace/session use cases, chat coordination, and SQLite persistence. |
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
  |                          +--> conversation
  |                          `--> agents   (roster values for addressed rendering)
  |
  `--> application ----------+--> agents -------> conversation
                             `--> conversation

agents, application, interfaces/text, and apps may also depend on util.
```

Interfaces consume application use cases and presentation-safe conversation
values. They do not load workspace files, open session repositories, or call
completion backends. Terminal rendering may read roster values from `agents/`
when labeling multi-agent transcripts.

## Source conventions

- Project headers are included from the `src/` include root, for example
  `"application/chat_coordinator.h"`.
- Headers include the standard and project headers required by their own public
  declarations; they do not rely on transitive includes.
- Public data types live with the behavior that owns their meaning:
  - agent config, definitions, protocol events, and roster types in `agents/`;
  - workspace, session summaries, journals, and repositories in `application/`;
  - transcript semantics in `conversation/`.
- Tests mirror this tree under `tests/`. Cross-layer scenarios belong under
  `tests/integration/`.

The broader runtime and persistence design is documented in
[`docs/cha.md`](../docs/cha.md).
