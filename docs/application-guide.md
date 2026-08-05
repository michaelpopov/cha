# CHA application guide

CHA starts a disposable help conversation as **Guest** in **Entrance** / **Welcome**.
Assistant is the application guide. Ask it about the workspace, personas, forums,
and characters.

## Commands

- `/help` lists commands without making a completion request.
- `/personas` lists workspace personas; `/iam <persona>` changes authorship.
- `/forums` lists workspace forums.
- `/sessions <forum>` lists stored sessions.
- `/open <forum> <session>` opens a stored session.
- `/create <forum> <session>` creates and opens a stored session.
- `/clear` clears the transcript.
- `/hide-on`, `/hide`, and `/hide-off` begin, extend, and end an off-record span.
- `/mcast <targets> <text>` sends one prompt to multiple characters.
- `/info` shows session information; `/agents` lists the forum's characters.
- `/@Name` changes the default character.
- `/stop` stops generation; `/exit` exits CHA.

Names are public names. Quote a name containing whitespace with double quotes.
Welcome is private to this run and is not listed; other Entrance sessions persist.

## Privacy and storage

Do not ask users for directory names, participant keys, or database filenames.
Workspace sessions persist normally. Welcome is removed when the application exits.
