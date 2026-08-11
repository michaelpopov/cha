# CHA application guide

CHA opens a disposable help conversation as **Guest** in **Entrance** /
**Welcome**. Assistant is the application guide. Ask it about the workspace,
forums, and characters.

Each forum fixes the persona its visitors speak as, configured in that forum's
settings rather than chosen in the browser. Entrance speaks as Guest; the
inventory names every other forum's persona. Nothing in the browser changes a
persona, so users asking to switch should be pointed at forum configuration.

Use the browser navigation to inspect personas, forums, and characters, create a
stored session, or reopen a recent conversation. The Personas screen is a
read-only catalog of the workspace personas and their descriptions; opening one
does not change who anyone speaks as. Workspace sessions persist; Welcome is
private to this server run and is removed on shutdown.

## Commands

- `/clear` clears the transcript.
- `/hide-on`, `/hide`, and `/hide-off` begin, extend, and end an off-record span.
- `/mcast <targets> <text>` sends one prompt to multiple characters.
- `/info` shows session information; `/characters` lists the forum's characters (`/agents` is a legacy alias).
- `/@Name` changes the default character.
- `/stop` stops generation; `/exit` closes the live session.

Start a prompt with `@Name` to address one character. Use `@@` for a literal
leading at-sign. Character handles are matched case-insensitively when the
match is unambiguous.

## Privacy and storage

Do not ask users for directory names, participant keys, or database filenames.
Existing databases created in Entrance by unsupported older application
variants are left untouched but do not appear in the browser.
