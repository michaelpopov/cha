# cha web UI design

Status: working visual specification, agreed on 2026-08-02.

This directory is the design contract for the browser UI. It documents the visual hierarchy, navigation states, and expected behavior before implementation begins.

Open [mockup.html](mockup.html) in a browser to exercise the design. Its controls switch between desktop and iPhone widths, open or close the sidebar, and select each main-area state.

## Design principles

- Desktop browsers and iPhone browsers use the same interface structure and visual language.
- Opening the sidebar pushes the main surface sideways; it does not overlay the main surface.
- The main surface displays either Chat or one Navigation state, never both.
- The round two-line button changes only the sidebar layout state.
- Current context lives in the sidebar: the selected persona appears beneath Personas and the selected forum beneath Forums.
- Chat has no title or context header. Its top bar contains only the two-line sidebar control.
- Recent contains cross-forum session references. The active session is highlighted.
- There is no Chat shortcut and no global New session shortcut in the sidebar.
- The round gear button at the bottom-right of the sidebar opens Settings.

## Screen catalogue

### Chat

| Desktop | iPhone |
| --- | --- |
| ![Desktop chat](screens/chat.png) | ![iPhone chat](screens/iphone-chat.png) |

### Sidebar

| Desktop | iPhone |
| --- | --- |
| ![Desktop sidebar](screens/sidebar-open.png) | ![iPhone sidebar](screens/iphone-sidebar.png) |

### Navigation

| Personas | Forums |
| --- | --- |
| ![Personas](screens/personas.png) | ![Forums](screens/forums.png) |

| Sessions | New session |
| --- | --- |
| ![Sessions](screens/sessions.png) | ![New session](screens/new-session.png) |

## Navigation titles

Navigation screens use one title without a subtitle:

- Personas
- Forums
- Sessions
- New session
- Settings

The Sessions title does not repeat the selected forum. The forum remains visible beneath Forums in the sidebar.

## Supporting specifications

- [Behavior and state rules](behavior.md)
- [Navigation flows](flows.md)
- [Web API implications](api-requirements.md)

## Pending visual decisions

- Settings content beyond its entry point.
- Loading, unavailable-session, and recoverable-error presentation.
- The initial chat lifecycle before a persisted session has been selected or created.
- Recent-session count, ordering details, and overflow behavior.

