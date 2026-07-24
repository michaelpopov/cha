# Application entry points

`apps/` contains executable composition roots. Files here assemble concrete
services and interfaces, define process-level error handling, and control
top-level object lifetime. They should contain wiring and workflow, not
reusable business logic.

## Current entry point

`tui_main.cpp` builds the terminal application:

1. load optional environment settings;
2. construct `WorkspaceService` and the process-wide `Terminal`;
3. select and prepare a room;
4. list, create, or open a session through `PreparedRoom`;
5. retain selection errors for another attempt;
6. run the selected `ChatCoordinator` through the terminal user loop;
7. report uncaught failures after terminal resources have been restored.

The prepared room remains alive through selection so agent definitions and the
room repository are reused rather than reloaded on each retry.

## Dependencies

Composition roots may depend on any concrete component required to assemble an
executable. The TUI entry point currently uses:

- `application/` for workspace and chat use cases;
- `interfaces/terminal/` for selection, terminal ownership, and the chat loop;
- `util/` for `.env` loading.

It does not reach into `storage/` or `agents/` because the application services
already compose those implementation details.

Future executables, such as an HTTP server, should receive their own source file
in this directory and reuse the application layer. Shared server or browser
protocol code belongs under a corresponding `interfaces/` directory rather
than in the entry point.
