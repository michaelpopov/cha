# cha web UI design

Status: working visual specification, agreed on 2026-08-02 and updated on 2026-08-06.

This directory is the design contract for the browser UI. It documents the visual hierarchy, navigation states, and expected behavior before implementation begins.

Open [mockup.html](mockup.html) in a browser to exercise the design. Its controls switch between desktop and iPhone widths, open or close the sidebar, and select each main-area state.

## Design principles

- Desktop and iPhone browsers use the same component hierarchy, interaction model, and visual language.
- Opening the sidebar pushes the main surface sideways; it does not overlay the main surface.
- The round two-line button is the only control that changes whether the sidebar is open.
- Sidebar references change the main-area state without closing or opening the sidebar.
- The main surface displays either Chat or one Navigation state, never both.
- The sidebar contains Personas, Characters, Forums, and Recent session references. It does not show the current persona or forum.
- The browser starts in the shared Welcome session in Entrance, which speaks as Guest, with Assistant as the current target.
- A persona is configured per forum and is never chosen in the browser. Personas is read-only: it reports the current forum's persona. Personas remain authors, never forum or session members.
- Chat has no title or context header. A compact line below the composer shows `<Forum>   From: <Persona>   To: <current default character>`.
- The composer's lower-left action chooses the target character. It replaces the unsupported attachment button shown in the original mockup.
- The composer's trailing action is Send while idle and becomes Stop while generation is active.
- Characters is informational. It lists workspace characters and opens a read-only rendering of a character's `CHARACTER.md`.
- Forums list member character names beneath each forum title. Forum descriptions do not exist, and member names are not links.
- Sessions show names and compact time metadata, never descriptions.
- Recent contains cross-forum session references. The active session is highlighted.
- There is no Chat shortcut or global New session shortcut in the sidebar.
- Users cannot create forums, personas, or characters in this version.
- There is no Settings screen and no gear button. The mockup and the screenshots in this directory still show one at the bottom-right of the sidebar; v1 removes it, and nothing takes its place.

## Screen catalogue

The screen images and interactive mockup predate the target-character amendment:
where they show an attachment `+`, the production UI shows the target chooser.
Their Send state remains the correct idle state; the same button shows Stop only
while generation is active.

### Chat

| Desktop | iPhone |
| --- | --- |
| ![Desktop chat](screens/chat.png) | ![iPhone chat](screens/iphone-chat.png) |

### Sidebar

| Desktop | iPhone |
| --- | --- |
| ![Desktop sidebar](screens/sidebar-open.png) | ![iPhone sidebar](screens/iphone-sidebar.png) |

### Workspace navigation

| Personas | Characters |
| --- | --- |
| ![Personas](screens/personas.png) | ![Characters](screens/characters.png) |

| Character detail | Forums |
| --- | --- |
| ![Character detail](screens/character-detail.png) | ![Forums](screens/forums.png) |

### Forum navigation

| Sessions | New session |
| --- | --- |
| ![Sessions](screens/sessions.png) | ![New session](screens/new-session.png) |

## Navigation titles

Chat has no title. Navigation screens use one centered title without a subtitle:

- Personas
- Characters
- The inspected character's display name on Character detail
- Forums
- Sessions
- New session

The Sessions title does not repeat the selected forum. Session rows do not contain descriptive excerpts.

## Supporting specifications

- [Behavior and state rules](behavior.md)
- [Navigation flows](flows.md)
- [Web API implications](api-requirements.md)

## Out of scope

- Settings in any form. It is removed from v1 rather than deferred as an entry point.
- Invalid application state has no separate browser presentation contract.
