# Text input interface

`interfaces/text/` owns the reusable textual protocol accepted by a chat input
box. It recognizes slash commands and optional leading agent mentions, then
dispatches the result to structured `ChatCoordinator` operations.

## Contents

| Source | Responsibility |
| --- | --- |
| `command.*` | Parse supported slash commands, their arguments, and `/@Name` default-agent selection. |
| `mention.*` | Parse an optional leading `@Name` and support `@@` as a literal leading at-sign. |
| `text_input.*` | Apply command and mention policy, enforce active-generation input rules, and call the coordinator. |

## Dispatch behavior

`handle_text_input()` is the ownership boundary for text-specific behavior:

- ordinary text is converted to prompt text plus an optional agent handle;
- `/clear`, `/info`, `/agents`, `/@Name`, and `/stop` call corresponding
  application operations;
- `/exit` is handled entirely as an interface request to end the session;
- unknown commands and unexpected arguments produce transient notices;
- while generation is active, only bare `/stop` is dispatched, and other input
  remains available for editing.

The parser does not resolve agent handles. It preserves the textual handle and
lets `ChatCoordinator` resolve it against the active roster.

## Dependencies

This adapter depends on:

- `application/` for `ChatCoordinator`, `CoordinatorUpdate`, and the shared
  generation-in-progress notice;
- `util/` for byte-oriented whitespace handling.

It does not depend on terminal widgets, curses, storage, or agent backends.
Keeping the adapter terminal-independent allows another text box to reuse the
same command language. A structured HTTP API may bypass it when routes and
request fields already express the intended operation.
