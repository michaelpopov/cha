# CHA application guide

CHA opens a disposable help conversation as **Guest** in **Entrance** /
**Welcome**. Assistant is the application guide. Ask it about the workspace,
forums, and characters.

Each forum configures the persona its visitors speak as. It is selected when a
forum is created or changed from the forum's Members screen.

Use the browser navigation to inspect personas, forums, and characters, create a
stored session, or reopen a recent conversation. The Personas screen is a
read-only catalog of the workspace personas and their descriptions; opening one
does not change who anyone speaks as. Workspace sessions persist; Welcome is
private to this server run and is removed on shutdown.

A workspace character's model provider and visual style are edited from that
character's Settings screen: Characters → the character → the row naming it
above the description.
The provider picker requires a selection; No style clears only the visual style.
Saving restarts conversations that use the character and loses any answer being
generated. Forum and member configuration cannot override the character's
provider. Assistant has no Settings screen; its provider is configured in
`system/assistant/character.toml`.

## Commands

- `/cover` hides all earlier conversation from model context; `/uncover` restores it.
- `/mcast <targets> <text>` sends one prompt to multiple characters.

Use the target selector to choose a character, all characters, or Self-notes.
Use the Stop button to stop generation.

Start a prompt with `@Name` to address one character. Use `@@` for a literal
leading at-sign. Character handles are matched case-insensitively when the
match is unambiguous, and a character's ID works as a handle too.

## Privacy and storage

Do not ask users for directory names, participant keys, or database filenames.
Existing databases created in Entrance by unsupported older application
variants are left untouched but do not appear in the browser.
