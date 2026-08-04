# Behavior and state rules

## State model

The browser keeps these related pieces of UI state:

| State | Values |
| --- | --- |
| Sidebar | Open or closed |
| Main area | Chat, Personas, Characters, Character detail, Forums, Sessions, New session, or Settings |
| Current context | Persona ID and forum ID |
| Active conversation | Forum ID and session ID |
| Inspected character | Character ID, used only by Character detail |
| Current default character | Character ID resolved for the current live session or selected forum |

The Main area is exclusive: Chat and Navigation are never visible together. Opening or closing the sidebar changes layout only; it does not change the Main-area state or any current context.

## Sidebar

The sidebar contains, from top to bottom:

1. `cha` product title.
2. Personas.
3. Characters.
4. Forums.
5. Recent cross-forum sessions. Every entry shows the session name and forum name.
6. A round Settings button with a gear icon at the bottom-right.

Personas and Forums have no current-selection secondary lines. The active conversation's Recent entry uses the selected-row treatment. There is no separate Chat button because selecting the active Recent entry already returns to that conversation.

## Controls

Only the two-line button changes sidebar visibility. Every other sidebar action preserves whether the sidebar is currently open or closed.

| Control | Result | Preserved state |
| --- | --- | --- |
| Two-line button | Toggles the sidebar | Main area and all current context |
| Personas | Shows Personas | Sidebar state, current forum, and active conversation |
| Persona radio | Selects exactly one persona | Sidebar state, current forum, and active conversation |
| Characters | Shows Characters | Sidebar state and all conversation context |
| Character row | Shows read-only Character detail | Sidebar state and all conversation context |
| Character-detail back row | Returns to Characters | Sidebar state and all conversation context |
| Forums | Shows Forums | Sidebar state, current persona, and active conversation |
| Forum row | Sets the current forum and shows Sessions directly | Sidebar state and current persona |
| Recent session | Sets that forum/session as active, opens or reattaches it, and shows Chat | Sidebar state and current persona |
| New session row | Shows New session for the current forum | Sidebar state, current persona, and forum |
| Settings gear | Shows Settings | Sidebar state and all current context |

Selecting a persona affects the next submitted message, including messages in an already-open session. It does not create, close, or switch sessions.

## Chat context line

Chat places one compact, read-only status line directly below the prompt composer:

`<Forum>   From: <Persona>   To: <Current default character in the forum>`

- Forum is the active conversation's forum, or the currently selected/default forum before a conversation is active.
- From is the currently selected persona and updates immediately after a persona radio selection.
- To is the current default character for the live session. Before a session is live, it is the selected forum's configured default character.
- A live change to the session's default character updates To from the authoritative session state.
- The line is not an editor or navigation control.

The current persona and forum do not appear in the sidebar or Chat header.

## Characters

Characters is a workspace-level, informational navigation area.

- The list contains every character registered in the workspace.
- Each row shows the character display name and its short configured description.
- Selecting a row opens Character detail without changing persona, forum, session, or default character.
- Character detail renders `CHARACTER.md` as formatted Markdown, including headings, paragraphs, emphasis, lists, and code blocks.
- Links are not interactive and images are not rendered or fetched.
- Character detail does not show forum membership or links to forums.
- Users cannot create or edit characters in this version.

## Forums and sessions

- Each forum row shows the forum display name and a plain-text list of member character display names beneath it.
- Forums have no descriptions.
- Member names are not clickable.
- Selecting a forum opens its Sessions screen directly.
- Sessions uses the title `Sessions`, not the forum display name.
- A stored-session row shows its name and compact time metadata at the trailing edge. It never shows a description or transcript excerpt because sessions have no description field.
- New session is an action row, not a stored session, and may carry the helper text `Enter a name to begin`.
- A forum with no stored sessions shows only New session.

## Session creation

The only explicit creation path is:

`Forums → select forum → Sessions → New session → enter name → Start session`

Rules:

- The session name is required after trimming surrounding whitespace.
- Start session remains disabled while the trimmed name is empty.
- Opening or cancelling New session creates nothing.
- Cancel returns to Sessions for the same forum.
- A creation error keeps the entered name and stays on New session.
- After successful creation, the new session is opened, becomes the active conversation, appears in Recent, and Chat becomes visible.
- Session-name uniqueness has not been specified; stable session IDs remain the authoritative identity.
- Users cannot create forums, personas, or characters in this version.

## Main-area headers

- Chat has no title, session name, forum, persona, or character in its header.
- Navigation screens show one centered title without a subtitle.
- Character detail uses the character's display name as its title.
- The two-line sidebar control remains at the top-left in every Main-area state.
- New session actions are inside the New session screen, not in the top bar.

## Responsive behavior

The component hierarchy and interaction model are identical at desktop and iPhone widths.

- With the sidebar closed, the Main area fills the browser width.
- With the sidebar open, the Main area is translated to the right and clipped by the browser viewport.
- The two-line button moves with the Main area and remains available in the visible edge.
- On iPhone widths the sidebar leaves only a narrow portion of the pushed Main area visible.
- The Chat context line remains one compact line beneath the composer at both widths.
- Long Character detail content scrolls within the main content region.

## Empty and failure behavior

- An empty Characters roster is a workspace configuration error rather than a character-creation prompt.
- A forum with no sessions shows only the New session row.
- A failed open keeps the user on Sessions and reports the failure without changing the active conversation.
- A failed creation keeps the user on New session and preserves the typed name.
- Detailed loading, error-message, and unavailable-session visuals remain to be designed.

