# Application entry points

`apps/` contains executable composition roots. Files here assemble concrete
services and interfaces, define process-level error handling, and control
top-level object lifetime. They should contain wiring and workflow, not
reusable business logic.

## Current entry point

`tui_main.cpp` builds the terminal application:

1. load optional `.env` settings;
2. construct `Workspace` and the process-wide `Terminal`;
3. let `StartupSelector` choose a room from `Workspace::rooms()`;
4. let the selector choose a new or existing session from
   `Workspace::sessions(room)`;
5. create or open that session through `Workspace`, which returns a
   `ChatCoordinator`;
6. run the coordinator through `run_user()`;
7. report uncaught failures after terminal resources have been restored.

Room selection, session listing, definition loading, and database open/create
all go through `Workspace`. The entry point does not touch session repositories
or agent loaders directly.

## Dependencies

Composition roots may depend on any concrete component required to assemble an
executable. The TUI entry point currently uses:

- `application/` for workspace and chat use cases;
- `interfaces/terminal/` for selection, terminal ownership, and the chat loop;
- `util/` for `.env` loading.

Future executables, such as an HTTP server, should receive their own source file
in this directory and reuse the application layer. Shared server or browser
protocol code belongs under a corresponding `interfaces/` directory rather
than in the entry point.
