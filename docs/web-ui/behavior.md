# Behavior and state rules

## State model

The browser keeps four related pieces of UI state:

| State | Values |
| --- | --- |
| Sidebar | Open or closed |
| Main area | Chat, Personas, Forums, Sessions, New session, or Settings |
| Current context | Persona ID and forum ID |
| Active conversation | Forum ID and session ID |

The Main area is exclusive: Chat and Navigation are never visible together. Opening the sidebar changes the layout but does not change the active Main-area state.

## Sidebar

The sidebar contains, from top to bottom:

1. `cha` product title.
2. Personas with the current persona as its secondary line.
3. Forums with the current forum as its secondary line.
4. Recent cross-forum sessions. Every entry shows the session name and forum name.
5. A round Settings button with a gear icon at the bottom-right.

The active conversation's Recent entry uses the selected-row treatment. There is no separate Chat button because selecting that Recent entry would perform the same action.

## Controls

| Control | Result | Preserved state |
| --- | --- | --- |
| Two-line button | Toggles the sidebar | Main area and all current context |
| Personas | Closes the sidebar and shows Personas | Current forum and active conversation |
| Persona radio | Selects exactly one persona | Current forum and active conversation |
| Forums | Closes the sidebar and shows Forums | Current persona and active conversation |
| Forum row | Sets the current forum and shows Sessions | Current persona |
| Recent session | Sets that forum/session as active, opens or reattaches it, and shows Chat | Current persona |
| New session row | Shows New session for the current forum | Current persona and forum |
| Settings gear | Closes the sidebar and shows Settings | Current persona, forum, and active conversation |

Selecting a persona affects the next submitted message, including messages in an already-open session. It does not create, close, or switch sessions.

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

## Main-area headers

- Chat has no title, session name, forum, or persona in its header.
- Navigation screens show a single centered title.
- The two-line sidebar control remains at the top-left in every Main-area state.
- New session actions are inside the New session screen, not in the top bar.

## Responsive behavior

The component hierarchy and interaction model are identical at desktop and iPhone widths.

- With the sidebar closed, the Main area fills the browser width.
- With the sidebar open, the Main area is translated to the right and clipped by the browser viewport.
- The two-line button moves with the Main area and remains available in the visible edge.
- On iPhone widths the sidebar leaves only a narrow portion of the pushed Main area visible.

## Empty and failure behavior

- A forum with no sessions shows only the New session row.
- A failed open keeps the user on Sessions and reports the failure without changing the active conversation.
- A failed creation keeps the user on New session and preserves the typed name.
- Detailed loading, error-message, and unavailable-session visuals remain to be designed.

