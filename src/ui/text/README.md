# Text input grammar

`ui/text/` owns the textual grammar a chat input box accepts: slash commands and
optional leading `@mentions`. It parses a line, applies the rules about what may
be typed when, and calls `SessionController` operations. It knows nothing about
curses, storage, or providers.

## Contents

| Source | Responsibility |
| --- | --- |
| `command.*` | `parse_command()` — recognizes `/clear`, `/hide-on`, `/hide`, `/hide-off`, `/mcast`, `/info`, `/agents`, `/stop`, `/exit`, and `/@Name`, splitting off any argument. |
| `mcast.*` | `parse_multicast_input()` — parses recipient lists and prompts for `/mcast`. |
| `mention.*` | `parse_addressed_prompt()` — splits a leading `@Name` from the prompt text, with `@@` as the escape for a literal at-sign. |
| `text_input.*` | `handle_text_input()` — the policy layer that turns a parsed line and its author ID into controller calls. |

## The grammar

| Input | Meaning |
| --- | --- |
| `hello` | Prompt for the default agent. |
| `@Ada hello` | Prompt addressed to `@Ada`. The handle may be any unambiguous case-insensitive prefix. |
| `@@channel hi` | Literal text `@channel hi` — no addressing. |
| `@` alone, or `@ ` | Not a mention; sent as ordinary text. |
| `/clear`, `/hide-on`, `/hide`, `/hide-off`, `/info`, `/agents`, `/stop`, `/exit` | Commands. They take no arguments. |
| `/mcast prompt` | Sends `prompt` to every forum persona in order. |
| `/mcast @Ada, @Grace. prompt` | Sends `prompt` to the named personas in that order. `@@` starts a literal `@` prompt. |
| `/@Ada` | Set the default agent for this run. |
| `/anything-else` | Unknown command; produces a notice. |

Mentions are recognized only at the start of a line, after leading whitespace.
Trailing punctuation on a handle (`@Ada, hello`) is tolerated during resolution
in `ForumPersonas`, not here.

The parser never resolves a handle. `handle_text_input()` receives the selected
author's stable user ID and hands it with text and multicast handles through
to `SessionController`. The controller resolves the author against the
session's captured user roster and the handles against `ForumPersonas`, so the
grammar cannot go stale when the personas in a forum change.

## Dispatch

```mermaid
flowchart TD
    input["submitted line"] --> empty{"empty?"}
    empty -->|"yes"| none["no update"]
    empty -->|"no"| parse["parse_command"]
    parse --> busy{"generation active?"}
    busy -->|"yes"| stopq{"bare /stop?"}
    stopq -->|"yes"| stop["controller.request_stop"]
    stopq -->|"no"| notice["in-progress notice,<br/>input left untouched"]
    busy -->|"no"| kind{"command kind"}
    kind -->|"text"| mention["parse_addressed_prompt"]
    mention --> submit["controller.submit_prompt(author, text, handle)"]
    kind -->|"mcast"| multicast["parse_multicast_input, then controller.start_multicast(author, text, handles)"]
    kind -->|"has an argument"| argerr["notice: takes no arguments"]
    kind -->|"clear"| c1["controller.clear_transcript"]
    kind -->|"hide_on"| c2["controller.open_offrecord"]
    kind -->|"hide"| c3["controller.extend_offrecord"]
    kind -->|"hide_off"| c4["controller.restore_offrecord"]
    kind -->|"info"| c5["controller.session_information"]
    kind -->|"agents"| c6["controller.agent_information"]
    kind -->|"set_default"| c7["controller.set_default_agent"]
    kind -->|"stop"| c8["controller.request_stop"]
    kind -->|"exit"| c9["end_session, handled here"]
    kind -->|"unknown"| c10["notice listing the commands"]
```

Two policies live in this file and nowhere else:

- **While a turn is running, only a bare `/stop` is dispatched.** Everything else
  returns the shared in-progress notice *without* clearing the editor, so a
  message typed during generation survives and can be sent afterwards.
- **`/exit` never reaches the session layer.** Ending a session is a front-end
  decision, expressed as `end_session` in the returned `SessionUpdate`.

## Dependencies

- **Depends on:** `session/` for `SessionController`, `SessionUpdate`, and
  the shared generation-in-progress notice; `util/` for byte-oriented whitespace
  handling.
- **Must not depend on:** terminal widgets, curses, session catalogs, or
  agent backends.

## Tests

| Test | Covers |
| --- | --- |
| `tests/ui/text/unit_command.cpp` | Command recognition, argument splitting, `/@Name`. |
| `tests/ui/text/unit_mention.cpp` | Mention splitting, `@@` escaping, whitespace and degenerate cases. |
| `tests/ui/text/unit_text_input.cpp` | Dispatch policy, including the active-generation rules and `/exit`. |
