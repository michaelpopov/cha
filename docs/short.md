# Create a note from a URL

Date: October 31, 2026

Status: Technical design for review. No application changes are included.

## 1. Behavior

The user opens **New note**, pastes a web or YouTube URL into the name field,
and selects **Create** or presses Enter. CHA creates the note in the selected
folder, loads the content, and names the note from the returned content title.
The user does not need to copy the URL into the prompt or submit it again.

Creation starts on dialog submission, not on paste, typing, or loss of focus.
Cancel closes the dialog without creating a note or making a network request.
Ordinary names keep the current creation behavior. **New folder** and
**Rename** continue to treat their input as a name. A full recognized URL in
**New note** always requests an import; to use that URL as a literal name,
create the note with another name and then use **Rename**.

Use the current page-import behavior for content formatting, pictures, title
normalization, sibling-name conflicts, saving, and read-only state. In the
normal case, an import into an empty new note saves the content and makes the
note read-only. Existing text in the notes prompt remains unchanged, including
when that prompt is hidden.

This shortcut uses the current editor-bound import. Opening another note,
including creating the next note, abandons an unfinished import. It does not
support loading several new notes in the background. Section 7 describes the
result and manual recovery.

## 2. Proposed UI

Keep the existing dialog layout, focus, selected default name, and buttons.
Change the field label to **Name or URL** for new notes only. Keep the input
type as text so ordinary names remain valid. Do not add helper text, a mode
selector, or another dialog.

Form area of the existing new-note dialog:

```text
┌────────────────────────────────────────────────────┐
│ Name or URL                                        │
│ ┌────────────────────────────────────────────────┐ │
│ │ https://example.org/article                     │ │
│ └────────────────────────────────────────────────┘ │
│                                [Cancel] [Create]   │
└────────────────────────────────────────────────────┘
```

After successful creation and list refresh, close the dialog and open the new
note. Show **Loading content…** in the existing notes status area, below the
document and above the prompt:

```text
┌────────────────────────────────────────────────────┐
│                 Existing note editor               │
│                                                    │
├────────────────────────────────────────────────────┤
│ Loading content…                                   │
├────────────────────────────────────────────────────┤
│                 Existing prompt, if visible        │
└────────────────────────────────────────────────────┘
```

The status appears for all note source imports: the new-note shortcut, a URL
submitted through the prompt, and a file import through the prompt or Upload.
This is an explicit visible addition to the existing manual import flows. One
shared loading flag controls the status and submission availability. Hiding
only the prompt does not hide the status or abandon the import.

If loading fails, show the existing import error with its existing dismiss
button. Recovery uses the existing prompt submission. No new page Retry
button is needed. After success, the status disappears and the tree shows the
content title, when one is available.

These wireframes are the proposed visible changes. They are not implemented
by this document.

## 3. Existing implementation to reuse

| Location | Existing responsibility |
| --- | --- |
| `webapp/src/components/NotesTree.tsx` | `beginCreate`, `submitCreate`, `finishMutation`, and `NameDialog` create and open notes in the selected parent. |
| `webapp/src/components/App.tsx` | `beforeCreateNote` saves the previous note; `requestOpenNote` navigates to the new note; `setNoteReadOnly` flushes pending saves before changing read-only state. |
| `webapp/src/state/view.ts` | The `open-note` action selects a note, carries the optional `destination` payload, and increments `noteRequest`; `notesEpoch` identifies the vault context. |
| `webapp/src/components/NotesScreen.tsx` | The URL branch of the prompt submission reads a page, imports it, renames an empty default-named note, and calls `lockImportedNote`. |
| `webapp/src/notes/pageImport.ts` | `promptPageUrl`, `notePageHtml`, and `notePageName` detect URLs, sanitize imported Markdown, and normalize titles. |
| `webapp/src/notes/editor.ts` | `appendNoteHtml` embeds pictures, checks document limits, and applies content to the editor. |
| `webapp/src/notes/tree.ts` | `availableNoteName` chooses a name among siblings, including reserved names. |
| `webapp/src/api/nativeClient.ts` | `readNotePage` calls the existing `notes.readPage` bridge method. |
| `src/notes/notes_operations.cpp` | `NotesService::read_note_page` reads the page and applies existing page or YouTube transcript formatting. |

Implement this shortcut in the frontend. Keep the current bridge methods,
storage format, reader settings, and backend URL validation. No database
migration or new import endpoint is needed.

## 4. URL detection and temporary name

In `submitCreate`, call `promptPageUrl(captured.name)` only when the item kind
is `note`, before sending the name to `createNoteItem`.

Use that helper without changing its recognition rules:

| Input | Result |
| --- | --- |
| `https://example.org/article` | Import the URL. |
| ` http://example.org/article ` | Trim and import the URL. |
| `www.example.org/article` | Import using the helper's existing HTTP default. |
| `https://www.youtube.com/watch?v=…` or `https://youtu.be/…` | Import through the same page reader. |
| `example.org`, `README.md`, or `My ideas` | Create a normally named note. |
| `Read https://example.org` or an unsupported scheme | Follow normal name handling; do not fetch. |

The helper requires the whole trimmed value to be one supported address. Do
not add a second URL parser or frontend YouTube detection. The backend already
recognizes YouTube from the requested or returned URL.

For a recognized URL, create the note with
`availableNoteName(items, parentId, 'New note')`. Do not store the URL as its
name. This preserves automatic title naming and allows URLs longer than the
200-character note-name limit. The existing reader validates URL length,
credentials, and other request constraints.

## 5. Creation and editor handoff

1. Capture the selected parent and input. Keep the existing submission lock
   and `onBeforeCreateNote` save/leave check. If the user stays in the previous
   note, stop without creating or loading anything.
2. Detect the URL and choose either the ordinary entered name or the available
   default name. Call `createNoteItem` once.
3. Pass the optional URL through `finishMutation`. Keep its current rule:
   open the note only when `refreshListing` returns `'applied'`. Pass the URL
   to `onOpenNote` together with the new note ID. Keep the existing unmount
   and vault-context guards.
4. Extend `requestOpenNote` and the `open-note` action with an optional
   `importUrl`. Store it as one optional `noteImportUrl` field beside
   `activeNoteId` in `AppState`, and pass it to `NotesScreen`.
5. Once the target editor is ready, editable, and registered with the existing
   save path, handle the opening request once. If the editor is empty, call
   the shared page-import function below. This must work with a hidden prompt.

The optional URL is necessary because the tree creates the note while the
screen owns its editor. Follow the existing `destination` / `noteDestination`
pattern: set it from `open-note`, and clear it on an ordinary open without
that payload, `close-note`, `vault-context-reset`, or release of the selected
note. Do not use the prompt draft as transport, serialize the URL into the
route, or add a persistent import record or request queue.

One additional clearing rule is required: clear `noteImportUrl` whenever the
note view becomes hidden, even when `activeNoteId` is retained. Today,
`releaseClosedNote` retains the selected note when a layout change hides it,
and `Screen` unmounts its editor. Copying the destination rules exactly would
let an empty note remount with its old URL and start another request. Apply
this clearing rule in the existing reducer; no separate clear action is
needed. A chat navigation that leaves the note visible in Split View does
not abandon the import.

Use the existing `(notesEpoch, noteRequest)` identity to distinguish opening
requests. A small ref in `NotesScreen` records the handled request. Set it
synchronously at the first eligible evaluation, before checking emptiness
and starting async work. If the note already has content, skip the shortcut
for that opening; later edits must not trigger it. Re-renders, error dismissal,
and effect replay must not start a second load. Do not reset this ref in the
effect cleanup. The empty-editor check is an additional content guard, not
a substitute for the ref or the hidden-view clearing rule.

The URL can remain inert in the opening payload after success or failure;
there is no completion action or page retry state. If opening the note itself
fails before the shortcut can start, the existing open Retry passes `importUrl`
alongside `destination` in its `open-note` action.

## 6. Shared page import

Extract the current URL branch in `NotesScreen` into a local async function,
for example `loadNotePage(url)`. Call it from both prompt submission and the
new-note shortcut. Keep it in the screen so it continues to use the current
editor, save registration, `renameImportedNote`, and `lockImportedNote`.

The function must:

1. Capture the editor, note ID, and notes epoch, and verify the current context.
2. Call `client.readNotePage(url)`.
3. Recheck the context. Compute `notePageName(page.title)` and the existing
   empty-note/default-name rename eligibility.
4. Import with `notePageHtml(page)` and `appendNoteHtml`, using
   `client.readNotePicture` and the existing context predicate.
5. Rename eligible notes with `renameImportedNote`, which uses
   `availableNoteName` and updates the tree. If the normalized title is empty,
   keep the available default name. Use the title already returned by the
   reader; do not add a separate title-generation request.
6. For an import into an empty note, call `lockImportedNote`. Its existing
   `onSetNoteReadOnly` path flushes the document before making it read-only.
   Keep save failures in the existing save/retry flow.

Return the existing prompt submission result, `{ clear_input: boolean }`.
Manual submissions keep their current input-clearing rules. The shortcut
ignores the return value, never calls `onDraftChange`, and never clears a
prompt draft. No separate content-accepted result is needed.

Rename the existing `importingFile` boolean to `importing` and set it in both
file and page imports. Use it for the shared loading status and to prevent
overlapping prompt/file submissions while an import runs. Initialize it for
each new editor context, and let an async completion reset it only for its
matching context. Final status cleanup must check identity without requiring
editability, because successful locking makes the editor read-only.

Keep current editing behavior during page loading: preserve text or a custom
name entered while the fetch is pending. Re-evaluate the existing rename and
read-only conditions after the fetch. Do not freeze the editor for the whole
network request; the import helper requires an editable editor.

## 7. Errors and navigation

| Event | Behavior |
| --- | --- |
| Note creation fails | Keep the dialog and entered URL. Show the existing creation error. No page request has started. |
| List refresh fails after creation | Keep the existing list error and list-only Retry. The empty default-named note exists, but it does not open and its URL does not load. Refreshing the list does not resume the shortcut or repeat creation. |
| Reader unavailable, network/YouTube failure, invalid reader URL, unreadable content, or size rejection before insertion | Keep the new note and show the existing reader/import error with its dismiss button. Without user edits, the note stays empty with its default name. Paste the URL into the prompt and submit it to try again, or delete the note and create it again. |
| A picture cannot load | Keep the existing importer policy for omitted pictures and retained content. |
| Title is missing | Keep the default name and complete the content import. |
| Rename fails or another window takes the chosen title | Keep imported content and show the existing rename warning. Use normal Rename to recover. |
| Saving or making read-only fails after insertion | Keep the imported editor content and existing save/read-only error actions. Do not reload the page. |
| User opens or creates another note, hides the note view, deletes the target, or changes vault | Abandon the unfinished import. A late load result must not start insertion, renaming, or locking, clear a draft, or show errors in the new context. |

If navigation abandons the load before insertion and the user made no edits,
the original note stays empty with its default name. No message is shown in
the new view, and the URL is not copied to the prompt. The user must return
to that note and paste the URL into the prompt to try again, or delete it and
create it again. Thus, users must wait for one load to finish before creating
the next URL note if they want both to complete. Any content already inserted
uses the normal save/leave path; it is not rolled back.

Use the existing editor identity, note ID, epoch, and editability checks after
asynchronous work and before starting further mutations. Page reading and
provider formatting can continue after navigation and consume provider usage
even though the result is ignored. A mutation already sent can finish against
its original note; a late result must not update the new editor's state. Keep
this existing behavior. Do not add navigation blocking, cancellation endpoints,
rollback deletion, background retry, or recovery timers.

Use the reader's existing configuration validation and errors. Do not add a
settings preflight that could block a valid import because unrelated or unused
settings are incomplete.

## 8. Implementation and validation

Change `NotesTree.tsx` for the field label, URL detection, temporary name,
and optional URL handoff through `finishMutation`. Change `App.tsx` and
`state/view.ts` for the opening payload and navigation lifecycle. Change
`NotesScreen.tsx` for shared loading, automatic start, and status.
Reuse `pageImport.ts`, editor helpers, and backend APIs as they are.

Extend the existing focused tests:

- `NotesTree.test.tsx`: plain names and folders keep current behavior; a URL
  creates one default-named note in the captured parent and passes its URL;
  Cancel and a refused leave do no work. Include a URL over 200 characters.
  Retain the existing failed-refresh behavior and its tests.
- `state/view.test.ts`: carry the source with its opening request; clear it
  on another open, close, and vault reset. Extend the existing hidden-note
  layout test to verify that hiding the view clears the URL while retaining
  the selected note.
- `NotesScreen.test.tsx`: automatically import only after editor/save setup;
  run once across re-renders/effect replay; work with a hidden prompt; preserve
  prompt drafts; skip a nonempty note; do not restart after a failed fetch or
  error dismissal; preserve the source through an opening failure's existing
  Retry; ignore late results after navigation. Verify loading status for page
  and file imports. Retain existing tests for edits during load.
- `Notes.test.tsx`: one integration flow pastes a URL into the new-note
  dialog and verifies content, final tree title, saved document, read-only
  state, and no second prompt submission. Cover an article and a YouTube URL
  with mocked reader responses. Also cover creating a second note while the
  first load is pending, and hiding/remounting an empty note: old results must
  not affect the new editor, and remounting must not restart the old load.

Run from `webapp/` after implementation:

```sh
npm run typecheck
npm test -- src/components/NotesTree.test.tsx src/components/NotesScreen.test.tsx src/components/Notes.test.tsx src/state/view.test.ts src/notes/pageImport.test.ts
```

Manually check an article and a YouTube video with configured reader settings,
including a hidden prompt and a failed load followed by manual prompt recovery.
Existing reader tests cover backend parsing and transcript formatting; this
shortcut does not require new provider calls in automated tests.
