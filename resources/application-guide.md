# CHA application guide

CHA opens a disposable help conversation as **Guest** in **Entrance** /
**Welcome**. Assistant is the application guide. Ask it about the workspace,
forums, and characters.

Each forum configures the persona its visitors start out speaking as. A session
changes persona with the `/!Name` chat command, which also saves the choice as
that forum's setting; nothing else in the browser changes a persona, so users
asking to switch should be pointed at `/!Name`.

Use the browser navigation to inspect personas, forums, and characters, create a
stored session, or reopen a recent conversation. The Personas screen is a
read-only catalog of the workspace personas and their descriptions; opening one
does not change who anyone speaks as. Workspace sessions persist; Welcome is
private to this server run and is removed on shutdown.

A workspace character's model provider and visual style are edited from that
character's Settings screen: Characters → the character → the row naming it
above the description.
The first picker entry is Workspace default or No style, which clears the
setting. Saving restarts conversations that use the character and loses any
answer being generated. A forum that sets its own provider is named under the
picker; changing the character's provider does not affect that forum. Assistant
has no Settings screen — it is built in and has no `character.toml`.

## Commands

- `/clear` clears the transcript.
- `/hide-on`, `/hide`, and `/hide-off` begin, extend, and end an off-record span.
- `/mcast <targets> <text>` sends one prompt to multiple characters.
- `/info` shows session information; `/characters` lists the forum's characters (`/agents` is a legacy alias).
- `/@Name` changes the default character and saves it as the forum's default.
- `/!Name` changes the current persona and saves it as the forum's default.
- `/provider <name>` switches the current character's provider for this session only; `/provider` reports the override and `/provider default` restores the configured provider. Nothing is saved.
- `/stop` stops generation; `/exit` closes the live session.

Start a prompt with `@Name` to address one character. Use `@@` for a literal
leading at-sign. Character handles are matched case-insensitively when the
match is unambiguous, and a character's ID works as a handle too.

## Privacy and storage

Do not ask users for directory names, participant keys, or database filenames.
Existing databases created in Entrance by unsupported older application
variants are left untouched but do not appear in the browser.
