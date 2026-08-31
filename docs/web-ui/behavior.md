# Behavior and state rules

## State model

The browser keeps these related pieces of UI state:

| State | Values |
| --- | --- |
| Sidebar | Open or closed |
| Main area | Chat, Personas, Persona detail, Characters, Character detail, Character settings, Forums, Sessions, or New session |
| Current context | Forum ID |
| Active conversation | Forum ID and session ID |
| Inspected persona | Persona ID, used only by Persona detail |
| Inspected character | Character ID, used by Character detail and Character settings |
| Current default character | Character ID from the active live session |

The Main area is exclusive: Chat and Navigation are never visible together. Opening or closing the sidebar changes layout only; it does not change the Main-area state or any current context.

On startup the active conversation is the shared built-in Welcome session in
Entrance, whose configured persona is Guest, and the current default character
is Assistant.

## Sidebar

The sidebar contains, from top to bottom:

1. `cha` product title.
2. Personas.
3. Characters.
4. Forums.
5. Recent cross-forum sessions. Every entry shows the session name and forum name.

Each mutable Recent row has an ellipsis action and the same menu on right-click.
Rename changes its display label without changing its stable session ID. Delete
requires confirmation, stops an open runtime if necessary, and removes the row
from CHA. The built-in Welcome row exposes neither action.

The browser has no whole-configuration operation. Operators stop CHA and use
offline database export/edit/import to add or remove personas, characters, or
forums. The mockup's round gear button at the bottom-right is removed in v1.

Personas and Forums have no current-selection secondary lines. Personas is
read-only: it catalogs the workspace personas and selects nothing. The active conversation's Recent entry uses the selected-row treatment. There is no separate Chat button because selecting the active Recent entry already returns to that conversation.

## Controls

Only the two-line button changes sidebar visibility. Every other sidebar action preserves whether the sidebar is currently open or closed.

| Control | Result | Preserved state |
| --- | --- | --- |
| Two-line button | Toggles the sidebar | Main area and all current context |
| Personas | Shows Personas | Sidebar state and all conversation context |
| Persona row | Shows read-only Persona detail | Sidebar state and all conversation context |
| Persona-detail back row | Returns to Personas | Sidebar state and all conversation context |
| Characters | Shows Characters | Sidebar state and all conversation context |
| Character row | Shows Character detail | Sidebar state and all conversation context |
| Character-detail back row | Returns to Characters | Sidebar state and all conversation context |
| Character-detail chevron | Shows Character settings for a writable character | Sidebar state and all conversation context |
| Character-settings back row or Cancel | Returns to Character detail | Sidebar state and all conversation context |
| Forums | Shows Forums | Sidebar state and active conversation |
| Forum row | Sets the current forum and shows Sessions directly | Sidebar state |
| Recent session | Sets that forum/session as active, opens or reattaches it, and shows Chat | Sidebar state |
| New session row | Shows New session for the current forum | Sidebar state and forum |
| Target-character chooser | Lists the active session's characters; selecting one requests it as the default | Sidebar state, forum, session, and draft |
| Send | Submits the draft while generation is inactive | Sidebar state and active conversation |
| Stop | Replaces Send while generation is active and requests that generation stop | Sidebar state, active conversation, and draft |

The browser never chooses an author. A submission carries text only, and the
session attributes it to the persona its forum configures, resolved against the
single-persona roster it received when it opened. Entering a different forum is
therefore the only thing that changes who a message comes from, and it does so
by opening a session there rather than by any client-side selection. Personas
remain authors, never members of a forum or session.

## Chat context line

Chat places one compact, read-only status line directly below the prompt composer:

`<Forum>   From: <Persona>   To: <Current default character in the forum>`

- Forum is the active conversation's forum.
- From is the active conversation's current persona, taken from the session snapshot.
- To is the current default character for the live session.
- A live change to the session's default character updates To from the authoritative session state.
- The line is not an editor or navigation control.

The persona and current forum do not appear in the sidebar or Chat header.

## Chat composer controls

The lower-left composer action is the target-character chooser. It occupies the
position used by the attachment `+` in the original mockup because attachments
are not supported. It lists only the active session's character summaries.
Selecting a character calls the default-character action; the `To` status does
not change until an authoritative snapshot or event confirms the new
`default_character_id`.

A message sent as `@- …`, or any plain message while the session is in
recording mode (entered with `/@-`), is saved to the transcript as an ordinary
human entry addressed to `-` with no reply. While `default_character_id` is
`-`, the composer placeholder reads `Recording — saved, not sent` and the
target chooser shows a selected `Recording` option in place of a character;
choosing a real character leaves recording mode.

The trailing composer action is Send while generation is inactive. While
`generation.active` is true, the same location and button become Stop with a
square icon. Stop requests cancellation and remains visible until authoritative
session state reports that generation is inactive. A failed Send preserves the
draft. Target selection and Stop never change sidebar visibility.

## Chat transcript

Each message shows its creation time on a subdued line beneath its text: the
date and the time, carrying the year only outside the current one, with the
full local timestamp as the element's tooltip. The time is absolute rather
than relative, so it stays correct while a conversation sits open. Entries
stored before timestamps existed show no time at all.

## Personas

Personas is a workspace-level, informational navigation area. It never changes
who a message comes from; that follows the forum, as described above.

- The list contains the application-wide persona roster: the built-in Guest first, then the configured personas in display-name order.
- Each row shows the persona display name and its short configured description, which personas may omit.
- Selecting a row opens Persona detail without changing persona, forum, session, or default character.
- Persona detail renders that persona's `PERSONA.md` as formatted Markdown, under the same restricted presentation Character detail uses.
- `PERSONA.md` is optional. A persona configuring none reports that in place of the description rather than showing an empty screen.

## Characters

Characters is a workspace-level navigation area.

- The list contains the application-wide character roster, including Assistant.
- Each row shows the character display name and its short configured description.
- Selecting a row opens Character detail without changing persona, forum, session, or default character.
- Character detail renders `CHARACTER.md` as formatted Markdown, including headings, paragraphs, emphasis, lists, and code blocks.
- Links are not interactive and images are not rendered or fetched.
- Character detail does not show forum membership, the current provider or style, or links to forums.
- Character detail shows a header row above the description carrying the character display name, in the shape the Sessions forum header uses. After the detail loads, a writable character's row opens Character settings. Assistant's row is plain text with no chevron because its system config is not writable from the browser.
- Users cannot create characters in this version. Provider and style are edited
  on Character settings; every other character field remains an offline
  export/edit/import operation.

## Character settings

Character settings is one form, reached only from a writable Character detail.

- The title is `Settings`. The back row names the character and returns to Character detail.
- Provider lists the workspace options that resolve and requires one selection. Style lists the same way, plus No style.
- A sample line shows the selected style before save. No style uses the plain default appearance.
- A fixed line above Save says that saving restarts the sessions using this character and loses any answer being generated.
- Save is disabled while nothing has changed and while a save is in flight. Cancel returns to Character detail without writing.
- Failures are reported in place.
- A successful save stays on the settings screen. The server validates and
  commits the database configuration before publishing it. Affected live
  sessions shut down with `reloading`; the existing stream recovery reopens
  them in the background. The chat shows "Applying settings…" and no Retry
  buttons. The same message appears after a `/!Name` persona switch, which uses
  the same durable mutation path for the forum default.

## Forums and sessions

- Each forum row shows the forum display name and, beneath it, the forum's short configured description. A forum configuring none shows a plain-text list of member character display names instead, so the row is never bare.
- Member names are not clickable.
- Selecting a forum opens its Sessions screen directly.
- Sessions uses the title `Sessions`, not the forum display name.
- Sessions shows a forum header row above the list, carrying the forum display name and its member names. It is the only place the forum is named once the sidebar is hidden.
- Selecting the forum header opens Forum detail without changing persona, forum, session, or default character.
- Forum detail renders the forum's `FORUM.md` as formatted Markdown, under the same restricted presentation Character detail uses. It also shows the member names and the persona the forum speaks as, both already known from bootstrap.
- `FORUM.md` is published whole. The same file is the forum's system prompt, so anything written in it is written for readers as well as for the characters. Template placeholders such as `$${character.display_name}` and `$$(include.md)` appear literally, because a description belongs to no single member and is not expanded.
- A forum with no `FORUM.md` reports that in place of the description rather than showing an empty screen. In practice only the built-in Entrance is in that position, since a configured forum fails to load without one.
- Back from Forum detail returns to Sessions for the same forum.
- A stored-session row shows its name and compact time metadata at the trailing edge. It never shows a description or transcript excerpt because sessions have no description field.
- New session is an action row, not a stored session, and may carry the helper text `Enter a name to begin`.
- Session labels are single-line trimmed Unicode text of at most 200 characters.
- A forum with no stored sessions shows only New session.

## Session creation

The only explicit creation path is:

`Forums → select forum → Sessions → New session → enter name → Start session`

Rules:

- The session name is required after trimming surrounding whitespace.
- Start session remains disabled while the trimmed name is empty.
- Opening or cancelling New session creates nothing.
- Cancel returns to Sessions for the same forum.
- After successful creation, the new session is opened, becomes the active conversation, appears in Recent, and Chat becomes visible.
- Users cannot create forums, personas, or characters in this version.
  A character's provider and style are edited from Character settings, not
  created here.

## Main-area headers

- Chat has no title, session name, forum, persona, or character in its header.
- Navigation screens show one centered title without a subtitle.
- Persona detail uses the persona's display name as its title. Character detail has no title: its own header row names the character, so a title would repeat it.
- Character settings uses the title `Settings`. The trailing top-bar slot is an empty spacer, present wherever there is a title to keep centred.
- The two-line sidebar control remains at the top-left in every Main-area state.
- New session actions are inside the New session screen, not in the top bar.

## Responsive behavior

The component hierarchy and interaction model are identical at desktop and iPhone widths.

- With the sidebar closed, the Main area fills the browser width.
- With the sidebar open, the Main area is translated to the right and clipped by the browser viewport.
- The two-line button moves with the Main area and remains available in the visible edge.
- On iPhone widths the sidebar leaves only a narrow portion of the pushed Main area visible.
- The Chat context line remains one compact line beneath the composer at both widths.
- Long Persona and Character detail content scrolls within the main content region.

## Empty behavior

- A forum with no sessions shows only the New session row.
- A persona with no `PERSONA.md` shows a short message on Persona detail saying so, never an empty screen.
