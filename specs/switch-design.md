# Switch between Chat and Notes

Date: November 12, 2026

Status: Technical design. This document describes the proposed implementation;
it does not change the application.

## 1. Purpose and scope

The sidebar's Chat and Notes tabs select both the sidebar content and the
main work area.

Required behavior:

- Chat shows the Chat panel and the existing Chat sidebar content.
- Notes shows the note editor and the existing Notes sidebar content.
- Notes → Chat → Notes shows the same note at the same scroll position.
- Chat → Notes → Chat shows the same chat session.
- The Chat and Notes prompt drafts keep their text.
- Notes → Chat saves a modified note.

This design covers the desktop React application in `webapp/src`. It does not
change ChaWeb, backend APIs, the database schema, or startup restoration.

The design reuses the path that a layout change uses today: when Notes becomes
hidden, CHA saves the note and its scroll position, unmounts the editor, and
keeps the note ID. When Notes shows again, the editor reloads the same note at
the saved scroll position. Panels are not kept mounted while hidden. The design
adds no cache, store, timer, or navigation system.

## 2. User-visible behavior

The existing controls and panel layouts remain. Add no controls, titles, or
labels.

### Single view

| Situation | Behavior |
| --- | --- |
| Notes → Chat | Save the modified note and its scroll position, then show Chat and the Chat sidebar together. |
| Chat → Notes | Show Notes and the Notes sidebar together. The editor reloads the same note in the same text mode at the saved scroll position. |
| Selected tab clicked again | Nothing changes when the main area already shows that work area. If another screen is showing, the click shows the work area. |
| No note selected | Show the existing Notes empty state. |
| No current chat session | Show the existing Chat empty state. Do not create a session. |
| Note save fails | Keep the sidebar and main area unchanged. Show the existing Retry / Stay / Discard dialog. Discard closes the note and then completes the switch. |
| Note is still loading | Switch at once. The unmounted editor cancels the load. Notes loads the note again on return. |
| Chat response is being generated | Allow the switch. Generation and live updates continue. |
| Session open/create, note leave, note action, or compression is pending | Ignore the click, as a layout change does today. |

### What is kept

- Note: the same note, text mode, and scroll position. The existing picture-load
  restoration keeps the position while pictures load. The cursor goes to the
  end of the note, and the existing note focus rule applies. Undo history and
  find/replace state are not kept.
- Chat: the same session. The transcript shows the latest messages, as after a
  layout change today.
- Drafts: the text of both prompt drafts. Textarea height and selection are not
  kept.
- Other panel-local state, active voice input, and audio playback follow the
  existing unmount behavior, the same as for a layout change today.

### Split view

The tab click changes the sidebar and `mainView`. Both panels stay visible and
the layout stays split. `mainView` decides which panel shows if the user later
changes to single view; that layout change saves the note through the existing
path.

A tab click in split view does not save the note. The note stays visible and
its normal autosave continues. This agrees with other chat actions in split
view, which do not leave the note.

### Other screens

From Settings, forum screens, and other non-workspace screens, a tab click
shows its work area. Existing navigation to those screens already closes the
open note, so Notes then shows the empty state. This design does not change
that.

Vault change, note deletion, session deletion, Return to start, and app close
keep their existing behavior.

## 3. Current implementation

| Location | Current behavior and relevance |
| --- | --- |
| `components/Sidebar.tsx` | The tabs dispatch `show-chat-tab` and `show-notes-tab` directly. |
| `state/view.ts` | These actions change only `sidebarTab`. `mainView` and `layoutMode` decide which panels show (`workspaceVisibility`). |
| `state/view.ts`, `releaseClosedNote` | Keeps `activeNoteId` when `set-layout` hides Notes and Chat stays visible. Clears it for other actions that hide a visible note. Clears `noteImportSource` whenever Notes is hidden. |
| `components/App.tsx`, native layout handler | Ignores a layout change while compression, a session open/create, or a note leave/action is pending. If Notes becomes hidden, calls `leaveNoteThen`, checks that the vault context did not change, then dispatches `set-layout`. |
| `notes/useNoteLeave.ts` | `leaveNoteThen` saves pending edits, saves the scroll position, and shows Retry / Stay / Discard on failure. |
| `components/NotesScreen.tsx` | On mount, loads `activeNoteId` and restores its saved scroll position. If `noteDestination` is set, selects that annotation instead. |
| `components/App.tsx`, drafts | Keeps both prompt drafts above the panels. `PromptInput` is a controlled input. |
| `useLiveSession.ts` | Keeps the live session and its stream independent of `ChatScreen`. Ordinary navigation (`navigate`) can discard an unused session. |

## 4. Changes

### Reducer (`state/view.ts`)

- `show-chat-tab` sets `sidebarTab` and `mainView` to `chat`.
  `show-notes-tab` sets both to `notes`. If both already have that value,
  return the same state.
- Do not change the conversation, session operation, note ID, text mode, note
  request number, or drafts.
- In `releaseClosedNote`, keep `activeNoteId` for `show-chat-tab`, as for
  `set-layout`.
- In `releaseClosedNote`, clear `noteDestination` whenever Notes is hidden,
  as for `noteImportSource`. Otherwise the reload selects the annotation
  again instead of the saved scroll position. This also applies to a layout
  change.

### Tab handler (`components/App.tsx`)

Add one handler for the two tabs. It uses the guard and the leave code of the
native layout handler; share that code in one function instead of a copy:

1. Ignore the click if compression, a session open/create, or a note
   leave/action is pending, or bootstrap is not ready.
2. If Notes is visible and the tab action hides it, call `leaveNoteThen`.
   In the callback, dispatch the tab action only if the vault context did not
   change.
3. Otherwise dispatch the tab action.

The handler does not call `navigate`, `beginNavigation`, `openConversation`,
or route-writing functions. Thus an unused session is not discarded, and the
URL does not change.

### Sidebar (`components/Sidebar.tsx`)

The tab buttons call the new handler through a prop instead of dispatching the
actions.

No other file needs a change. `NotesScreen`, `ChatScreen`, `PromptInput`,
`useNoteLeave`, `useLiveSession`, and the panel mounting in `Screen` stay as
they are.

## 5. Tests

Update the tests in `state/view.test.ts` that expect a tab to change only the
sidebar. Keep the tests that release the note on other navigation, close,
deletion, and vault change.

- Reducer: each tab sets `sidebarTab` and `mainView`; `show-chat-tab` keeps the
  note ID, text mode, and conversation; hiding Notes clears `noteDestination`;
  a repeated click returns the same state.
- Sidebar: the tab buttons call the handler prop.
- App, single view: open note A, type both drafts, edit the note, click Chat.
  The note and its scroll position are saved before Chat shows session X. Click
  Notes: note A loads at the saved scroll position. Both drafts keep their text.
- App, save failure: Retry, Stay, and Discard give the outcomes in section 2;
  the view does not change before the save completes. A vault change during the
  save does not apply the tab change.
- App, guards: a click during a pending session open/create or note action does
  nothing.
- App, unused session: Chat → Notes → Chat keeps the unused session.
- App, split view: a tab click keeps the split layout and does not save the note.

Run `npm run typecheck`, the frontend test suite, and `npm run build` from
`webapp`. In the desktop app, check one long note with pictures: after
Notes → Chat → Notes it shows the same scroll position.

## 6. Decisions

- Reload the note on return instead of keeping hidden panels mounted. Hidden
  mounted panels would need extra work for frozen editors, zero-size scroll
  measurements, focus, the note context menu, stale note content, and audio.
  Accepted by the user on November 12, 2026.
- No save on a tab click in split view. Accepted by the user on November 12,
  2026.
