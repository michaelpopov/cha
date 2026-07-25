# Text input interface

`interfaces/text/` owns the textual protocol a chat input box accepts: slash
commands and optional leading `@mentions`. It parses a line, applies the rules
about what may be typed when, and calls structured `ChatCoordinator`
operations. It knows nothing about curses, storage, or providers.

## Contents

| Source | Responsibility |
| --- | --- |
| `command.*` | `parse_command()` — recognizes `/clear`, `/info`, `/agents`, `/stop`, `/exit`, and `/@Name`, splitting off any argument. |
| `mention.*` | `parse_addressed_prompt()` — splits a leading `@Name` from the prompt text, with `@@` as the escape for a literal at-sign. |
| `text_input.*` | `handle_text_input()` — the policy layer that turns a parsed line into coordinator calls. |

## The grammar

| Input | Meaning |
| --- | --- |
| `hello` | Prompt for the default agent. |
| `@Ada hello` | Prompt addressed to `@Ada`. The handle may be any unambiguous case-insensitive prefix. |
| `@@channel hi` | Literal text `@channel hi` — no addressing. |
| `@` alone, or `@ ` | Not a mention; sent as ordinary text. |
| `/clear`, `/info`, `/agents`, `/stop`, `/exit` | Commands. They take no arguments. |
| `/@Ada` | Set the default agent for this run. |
| `/anything-else` | Unknown command; produces a notice. |

Mentions are recognized only at the start of a line, after leading whitespace.
Trailing punctuation on a handle (`@Ada, hello`) is tolerated during resolution
in `AgentRoster`, not here.

The parser never resolves a handle. It hands the text through and lets
`ChatCoordinator` match it against the live roster, so the grammar cannot go
stale when a room's roster changes.

## Dispatch

```mermaid
flowchart TD
    input["submitted line"] --> empty{"empty?"}
    empty -->|"yes"| none["no update"]
    empty -->|"no"| parse["parse_command"]
    parse --> busy{"generation active?"}
    busy -->|"yes"| stopq{"bare /stop?"}
    stopq -->|"yes"| stop["coordinator.request_stop"]
    stopq -->|"no"| notice["in-progress notice,<br/>input left untouched"]
    busy -->|"no"| kind{"command kind"}
    kind -->|"text"| mention["parse_addressed_prompt"]
    mention --> submit["coordinator.submit_prompt"]
    kind -->|"has an argument"| argerr["notice: takes no arguments"]
    kind -->|"clear"| c1["coordinator.clear_conversation"]
    kind -->|"info"| c2["coordinator.session_information"]
    kind -->|"agents"| c3["coordinator.agent_information"]
    kind -->|"set_default"| c4["coordinator.set_default_agent"]
    kind -->|"stop"| c5["coordinator.request_stop"]
    kind -->|"exit"| c6["end_session, handled here"]
    kind -->|"unknown"| c7["notice listing the commands"]
```

Two policies live in this file and nowhere else:

- **While a turn is running, only a bare `/stop` is dispatched.** Everything else
  returns the shared in-progress notice *without* clearing the editor, so a
  message typed during generation survives and can be sent afterwards.
- **`/exit` never reaches the application.** Ending a session is an interface
  decision, expressed as `end_session` in the returned `CoordinatorUpdate`.

## Dependencies

- **Depends on:** `application/` for `ChatCoordinator`, `CoordinatorUpdate`, and
  the shared generation-in-progress notice; `util/` for byte-oriented whitespace
  handling.
- **Must not depend on:** terminal widgets, curses, session repositories, or
  agent backends.

## Tests

| Test | Covers |
| --- | --- |
| `tests/interfaces/text/unit_command.cpp` | Command recognition, argument splitting, `/@Name`. |
| `tests/interfaces/text/unit_mention.cpp` | Mention splitting, `@@` escaping, whitespace and degenerate cases. |
| `tests/interfaces/text/unit_text_input.cpp` | Dispatch policy, including the active-generation rules and `/exit`. |
