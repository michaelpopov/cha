# Block 3: Picture panel and complete feature verification

Status: implementation plan; this stage is not implemented.

Prerequisites: [block 1](block1.md) and [block 2](block2.md) are complete and
their checks pass. Read [picture.md](picture.md) and the repository
instructions. This stage delivers the standalone UI and verifies the full
feature across storage, bridge, and native rendering.

## Outcome

The chat shows an optional character picture on the right. Its toggle is
independent of the sidebar, its divider supports dragging and arrow keys,
and it follows the active character without stale images. Narrow windows
keep the sidebar and chat side by side with reachable controls.

## 1. UI state and character selection

Extend `webapp/src/state/view.ts` with only the persistent-for-this-run UI
values the feature needs: picture visibility, preferred width, and temporary
selected character ID. Default to open and 280 CSS pixels. Keep image bytes,
loading status, errors, and drag state local to the panel or drag component.
Do not write preferences to the vault, local storage, or disk.

Preserve visibility and width across navigation, character/session/vault
changes, and closing/reopening. Reset only the character selection when the
conversation context resets. Apply the design's rules in this order:

1. On `conversation-opened`, initialize from the selected recipient; for `*`,
   use the latest character reply in the transcript. `-` selects no picture.
2. For subsequent snapshots of the same session, compare entry IDs with the
   reducer's existing snapshot before replacing it. Newly observed character
   or error entries with a request ID identify their `participant_id`; human
   entries with a request ID identify `addressed_to`. Use the latest such
   generation turn. Ignore self-notes, notices, and updates to existing entry
   text/status. Deletion and cover changes do not select an older speaker.
3. Apply a changed recipient: a character replaces that recovered selection,
   `-` clears it, and `*` leaves it alone. An unchanged default recipient must
   not overwrite the last responding character.
4. Apply an active generation's nonempty character ID last, including during
   preparation, reasoning, and multicast. This takes precedence over the
   recipient and recovered history. Keep the selection when generation ends.
5. Validate the result against current forum members and clear removed or
   unknown members. Ignore snapshots from a different conversation using the
   existing reducer guard.

Use the existing `webapp/src/useLiveSession.ts` delivery and reset paths; add
no backend last-speaker state, second transcript store, remembered-entry
index, or new event stream.

## 2. Loading and rendering

Implement the panel in `webapp/src/components/ChatScreen.tsx`; a small local
component or adjacent component file is appropriate if it keeps the chat
component readable. Call the block 2 client method only while the panel is
open on the chat screen and has a selected character.

Use the existing asynchronous cleanup and vault-context handling patterns.
Clear the previous image immediately when character, session, or vault
changes. Ignore responses and failures from an obsolete request or unmounted
panel. Closing the panel removes the image element and must not be undone by
a late response. Keep image content out of `AppState`; remounting the chat
loads it again, including after import or merge into the same vault.

Render the original bytes with an image data URL, display-name alternative
text, and `object-fit: contain`. Center the image in the available space and
keep it stationary while the transcript scrolls. A null result leaves an
empty panel. A request or image decode failure shows `Picture unavailable`.
The failure must not interrupt chat or select a lower-priority format.

Add the picture icon at the right end of the status row, with `Show picture`
or `Hide picture` as its accessible label and tooltip and matching
`aria-expanded`. Reuse `SidebarToggle` as the control pattern. Add no heading,
legend, biography, loading narration, or explanatory helper text.

## 3. Divider and responsive layout

Extend `webapp/src/styles/app.css`. Keep the transcript/composer column and
picture panel in the main chat area, with a subtle vertical divider. Derive
the actual width from the preferred width and available layout space:

```text
maximum picture width = max(0, available main-area width - 320)
displayed picture width = min(preferred picture width, maximum picture width)
```

Measure the actual area available to the two columns, accounting for padding
and divider layout. Do not assume viewport width equals chat-area width.
Automatic shrinking must not overwrite the preferred width.

Follow the pointer-capture and keyboard pattern in
`webapp/src/components/App.tsx` for the sidebar divider. Start a picture drag
from its displayed width. Moving left increases width; moving right reduces
it. When resizing is available, clamp the requested width between 128 pixels
and the current maximum. Arrow keys move the divider by 16 pixels in the
same direction. Do not change sidebar width or panel visibility.

Give the divider the label `Resize picture`, a vertical separator role,
current width bounds, and a visible keyboard focus state. End dragging on
pointer release, cancellation, or lost capture. Disable resizing when the
maximum is at most 128 pixels. Hide the divider when the panel is closed or
has zero width. Reuse the existing cursor and selection treatment without
building a general resizable-panel framework.

Below 512 CSS pixels, an open sidebar uses its existing 192-pixel minimum;
chat fills the remainder and the picture reaches zero width. Remove the
430-pixel media rule's sidebar width and main-area offset overrides. Allow
chat to shrink below 320 pixels when necessary. With the sidebar closed,
use the available width and the formula above. Make status controls wrap,
keep icon buttons at their normal size, and allow context text to shrink or
wrap. Resizing the window must not toggle either panel.

## Focused automated verification

Extend existing suites where they cover the behavior:

- `webapp/src/state/view.test.ts`: initial recipients, `*`, `-`, explicit
  addressed replies, changed recipients, preparation/reasoning, normal and
  combined multicast snapshots, reconnection after missed replies, failed
  turns, cancellation before a reply, removed members, and context reset.
  Repeated snapshots, entry edits/deletion, and cover changes must not undo
  a later selection. Confirm generation takes precedence over all other rules.
- `webapp/src/components/LiveChat.test.tsx`: loaded/missing/broken pictures,
  data URLs and alternative text, independent toggles, and deferred responses
  arriving after character changes, panel closure, or navigation. Verify no
  request while closed and a fresh request after returning to chat.
- `webapp/src/components/App.test.tsx` and existing resize tests: preferences
  survive screen/session/vault changes; keyboard and pointer resizing obey
  bounds; automatic shrinking preserves preferred width; dragging starts
  without a jump; cancellation ends a drag.

Use real deferred promises for request races. Test behavior rather than CSS
class spelling or helper implementation. DOM unit tests cannot establish
native image decoding, animation, or actual narrow-window geometry; check
those in the hosts below.

During implementation, run the changed UI suites and typecheck. At the final
checkpoint, run the repository checks once from the repository root:

```sh
make test
make web-check
npm --prefix webapp run build
npm --prefix webapp run build:chaweb
```

`build:chaweb` checks that the shared type/storage work leaves the separate
interface buildable; it does not add picture UI there. On a supported daemon
host, also build `cha-daemon` and run its relevant storage/startup tests with
a disposable vault containing picture rows.

## Native and complete-flow verification

Use disposable test vaults. In both WKWebView on macOS and WebView2 on Windows:

1. Export, add pictures to nested character and Assistant folders, and import.
   Display PNG, JPEG with both extensions, WebP, and GIF. Check animated GIF
   and WebP, filename priority, a missing image, and an invalid preferred image.
2. Change recipients, issue an explicit addressed reply, run multicast, and
   switch sessions/vaults while requests are pending. Confirm the final
   portrait and no image from the previous context.
3. Drag and keyboard-resize the panel, close/reopen it, navigate away/back,
   and change vaults. Check preferred-width retention and independent sidebar
   control. With no picture, the open panel still has the same controls.
4. Inspect wide windows and widths of 512, 430, 400, and 320 CSS pixels with
   both panels open and with each closed. Check wrapped controls, accessible
   toggles, independent transcript scrolling, resize bounds, zero-width
   behavior, and restoration after widening the window.
5. Merge or import an updated picture into the same vault, return to chat,
   and verify it reloads. Export again and compare original image bytes.

Reuse the native harnesses in `tests/native/macos/run.sh` and
`tests/native/windows/run.ps1` where useful; build their required host targets
and frontend assets first. Extend a focused native test only where it proves
behavior that the existing unit tests cannot cover.

Report automated results and native checks separately. If a platform is not
available, identify the checks still pending; do not describe that platform
as verified. Before the user imports pictures into shared vaults, all desktop
hosts and the ChaWeb daemon must include block 1's storage support.

## Completion

Review the final diff against `picture.md` and the KISS instructions. Remove
temporary scaffolding and any unused state or abstractions. Summarize the
implemented behavior, checks, and any pending platform verification. No image
upload/editor, watcher, cache, background refresh, animation controls, or
forum-specific picture overrides belong in this feature.
