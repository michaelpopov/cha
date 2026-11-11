# Editing compressed notes

Date: November 10, 2026

Status: Proposed feature design. This document specifies the change; it does
not implement it. The read-only, startup-mode, ANNOTATIONS, and blocking
compression decisions below were confirmed by the user. The toolbar and
progress-dialog sketches are the proposed visual design.

## 1. Purpose and assessment

Let the user view and edit a note's compressed document in the existing Notes
editor. Add one application-wide choice: `full_text` or `compressed`. Keep one
entry for each logical note in the tree.

The proposed approach is consistent with the current storage. A compressed
version already has its own `note_items` row and Tiptap JSON document. It does
not need another editor, another document format, or another database table.

There is no fundamental contradiction. The design must distinguish two things:

- **Mode:** which version the user prefers when opening a note.
- **Open document:** the actual stored row loaded into the editor.

In compressed mode, a note without a compressed version opens its full text.
Edits must then save to the full-text row. Saving must never resolve the global
mode again, because a compressed version could have appeared in the meantime.

Other interactions need explicit rules: changing modes with unsaved edits,
regenerating compressed text, shared read-only state, background imports,
annotations, and audio. The following sections define those rules.

## 2. Decisions and scope

| Subject | Decision |
| --- | --- |
| Mode values | `full_text` and `compressed`. |
| Startup | Start in `full_text` each time the application starts. Do not persist the mode. Confirmed by the user. |
| Scope of mode | One value in the desktop application's existing React app state. It applies to all Notes editor openings in that app instance, in single and split layouts. |
| Navigation and vault changes | Preserve the mode while that app instance runs, including navigation away from Notes and vault changes. Clear the open document on a vault change as today. Separate app instances have independent modes. |
| Read-only state | Both documents use the original note's `read_only` setting. Confirmed by the user. |
| Folder ANNOTATIONS | Continue to collect full-text highlights and comments only. Compressed highlights and comments remain in their own document. Confirmed by the user. |
| Editing | Use the existing editor, formatting, search/replace, clipboard, pictures, prompt insertion, imports, autosave, and leave handling on the loaded document. |
| Tree identity | Keep the original note ID, name, path, and tree entry. Do not expose compressed rows as children in the tree. |
| Compression | A blocking operation with a modal progress dialog, as requested by the user. Save current edits before starting and prevent editing until the job ends or the Cancel request returns. Always generate from saved full text. |
| Compression completion | Wait for the existing end-of-job notice. Refresh tree metadata. If the selected note was a target and the mode is `compressed`, reload its saved document before restoring access. Keep the global mode unchanged. The dialog does not report the outcome; Settings → Events records it, as today. The backend compression operations do not change. |
| Models and chat | Preserve the existing preference for compressed text when attaching notes. The editor mode does not change this policy or existing chat snapshots. |
| ChaWeb | Keep its existing read-only, full-text notes behavior. This feature changes the native Notes editor. |

Do not add automatic compression, conversion of one version into the other,
per-note mode preferences, independent compressed-note names or permissions,
version history, automatic expiry, or a new save timer. Editing full text does
not delete or regenerate its compressed version. Editing compressed text does
not change full text.

## 3. Opening and selecting documents

The backend resolves the requested mode when it reads a note. Tree metadata
such as `has_compressed` is useful for display, but can be stale and must not
determine the save destination.

| Requested mode | Compressed row exists | Document shown | Save destination |
| --- | --- | --- | --- |
| `full_text` | No | Full text | Original row |
| `full_text` | Yes | Full text | Original row |
| `compressed` | No | Full text | Original row |
| `compressed` | Yes | Compressed text | Compressed row |

Selecting a different mode applies immediately to the selected note, after the
current document passes the existing save/leave workflow. The user does not
need to select another tree item first. Selecting the already active mode is
a no-op. Reselecting a tree note remains an explicit reopen, as today.

Fallback is a read decision only. It does not change the global mode, create a
compressed row, or copy full text into one. An existing empty compressed
document is still a compressed version; show it without falling back.

An invalid compressed document produces an opening error. Do not silently
fall back when a row exists but cannot be decoded. Full-text mode reads only
the original document, so damage to an unused compressed document does not
prevent the original from opening. The reverse also applies when reading an
existing compressed document.

Folders do not open editable documents. Generated `ANNOTATIONS` items have no
compressed version and retain their existing immutable behavior. Session
summary notes retain their existing compression restrictions. If a stored
summary already has a compressed row, the normal selection rule can read it;
this feature does not make summaries eligible for new compression.

## 4. Toolbar and navigation

Add a compact text button with a downward chevron at the start of the Notes
toolbar, immediately before the existing Content button. Reuse existing
button, menu, spacing, and focus styles. The other toolbar controls retain
their order. Do not add a panel heading, legend, or explanatory paragraph.

The button reports the mode and, when needed, the full-text fallback:

```text
Full-text mode

┌────────────────────────────────────────────────────────────────────┐
│ [Full text ▾]  [Content]  [History]  [Edit]  [Highlight]  …          │
├────────────────────────────────────────────────────────────────────┤
│ The original note document                                         │
└────────────────────────────────────────────────────────────────────┘

Compressed mode, compressed document loaded, menu open

┌────────────────────────────────────────────────────────────────────┐
│ [Compressed ▾]  [Content]  [History]  [Edit]  [Highlight]  …         │
│ ┌──────────────────┐                                               │
│ │   Full text      │                                               │
│ │ ✓ Compressed     │                                               │
│ └──────────────────┘                                               │
│ The compressed note document                                       │
└────────────────────────────────────────────────────────────────────┘

Compressed mode, full text loaded as fallback

┌────────────────────────────────────────────────────────────────────┐
│ [Compressed → Full text ▾]  [Content]  [History]  [Edit]  …          │
├────────────────────────────────────────────────────────────────────┤
│ The original note document                                         │
└────────────────────────────────────────────────────────────────────┘
```

The square-bracket names for existing controls represent their current icons;
they do not propose text labels for those controls.

The menu has exactly two options, **Full text** and **Compressed**. Its check
mark shows the global mode, including during fallback. Both options remain
available when the selected note lacks a compressed version.

Use the loaded document identity for the fallback label. The tooltip can say
`Compressed mode; showing full text`. When compression of the selected note
ends, compressed mode reloads that note before the progress dialog closes. If
a compressed document now exists, the button changes from the fallback label
to `Compressed`.

When no document is loaded, show the mode name alone. The mode control remains
available in empty, loading, and opening-error states, so the user can choose
a mode before selecting a note or recover from a damaged compressed document.
It can cancel an obsolete load through the existing load-cancellation
mechanism. Do not require an `Editor` instance just to render this control.
The normal Notes toolbar visibility setting still applies. Today the toolbar
is not shown in these states; the other toolbar controls stay hidden in them:

```text
No note selected

┌────────────────────────────────────────────────────────────────────┐
│ [Full text ▾]                                                      │
├────────────────────────────────────────────────────────────────────┤
│                                                                    │
└────────────────────────────────────────────────────────────────────┘

Compressed mode, note opening

┌────────────────────────────────────────────────────────────────────┐
│ [Compressed ▾]                                                     │
├────────────────────────────────────────────────────────────────────┤
│ Opening…                                                           │
└────────────────────────────────────────────────────────────────────┘

Compressed mode, opening error

┌────────────────────────────────────────────────────────────────────┐
│ [Compressed ▾]                                                     │
├────────────────────────────────────────────────────────────────────┤
│ The existing opening error text                                    │
│ [Retry]                                                            │
└────────────────────────────────────────────────────────────────────┘
```

Reuse the existing `ToolbarMenu` button and menu. Give it the label
`Note text mode` and a title that states the mode and the actual document,
as the Playback speed control does. Build the two items like the existing
Headings menu items: `role="menuitemcheckbox"`, `aria-checked`, and
`CheckIcon` on the current mode. The existing `menuKeys()` already handles
these items, Escape, and focus movement; do not change it. Without an editor,
closing the menu returns focus to the mode button. Disable mode changes during
an existing save/leave decision, compression, or another exclusive note
operation.

Keep the tree's **Show compressed** action as a shortcut. Change it to select
compressed mode and open that logical note through one guarded transition.
Remove its separate read-only dialog, `<pre>` rendering, and dialog-only
compression-ratio code. This gives the action the same editor behavior as the
new toolbar control. Keep its existing availability condition.

A link from the generated ANNOTATIONS document addresses an original
highlight or comment. Follow it by switching to `full_text` and opening the
source note in one guarded transition. Do not interpret a full-text ordinal
against compressed content or introduce a hidden per-note mode override.

## 5. App state and safe switching

Add `noteTextMode: 'full_text' | 'compressed'` to `AppState`, initialized to
`full_text`. Do not add it to persisted `ViewPreferences`, the vault, sync,
or URL parameters. Normal note URLs continue to identify the logical note.

The app owns the mode change handler. Reuse `useNoteLeave.leaveNoteThen()`:

1. Capture the requested mode and current vault context.
2. Freeze the current editor and wait for any admitted save.
3. Flush remaining changes to the loaded document's ID.
4. If saving fails, keep the old mode and document. Use the existing Retry,
   Stay, and Discard choices. Retry retries that same document; Stay cancels
   the switch; Discard continues with the requested switch.
5. After a successful flush or explicit discard, verify the vault context,
   commit the new mode, and reopen the same logical note. A shortcut that also
   chooses a different note commits the mode and note selection together.
6. Dispose of the old editor and create the new editor through the existing
   note-load path. Do not carry undo history, selection, or document-relative
   positions between versions.

With no open editor, only update the mode and, if a note is selected, restart
its load. A destination load failure leaves the chosen mode active and shows
the normal opening error. It does not expose the old document under the new
mode label.

A mode change increments `noteRequest` in the same state update, as the
existing `reload-note` action does. The existing load path, its cancellation,
and editor cleanup then run unchanged and read the new mode. Do not add the
mode as a second load key. Keep the existing `notesEpoch` for vault context;
do not increment it for a mode change, because it also retires background
imports. Like `reload-note`, a mode change clears a previous annotation
destination or one-shot import source.

Store the loaded `document_id` beside the editor. That value, the editor
instance, and the vault epoch identify an editing context. Pass the document
ID to `useNoteSave` as its save identity, while App's `activeNoteId` remains the
original note ID. Save and scroll callbacks capture this loaded context; they
must not read a later mode or logical selection to choose their destination.

Late loads, save results, clipboard/picture completions, and import completions
must not mutate the replacement editor. Preserve the existing five-second
autosave and native-close workflow. Do not maintain two editor instances or a
second draft cache.

## 6. Note API and storage

### Reads

Extend `notes.get` with an optional `mode`, defaulting to `full_text`. `id`
continues to mean the original note ID. Reject values outside the two modes.

```json
{ "id": 42, "mode": "compressed" }
```

Return the logical ID and the actual document row ID:

```json
{
  "id": 42,
  "document_id": 87,
  "document": {
    "type": "doc",
    "content": [{ "type": "paragraph" }]
  },
  "scroll_top": 0
}
```

For full text, including fallback, `document_id == id`. For compressed text,
`document_id` is the compressed child row ID. The frontend can derive the
actual version from this comparison; no separate effective-mode field is
needed. For an ANNOTATIONS read, both IDs identify its existing row.

Resolve the row and read its document and scroll position in one transaction.
Use row existence for fallback, not an empty or NULL content test. Decode and
validate only the selected document. Keep `notes.get` entry IDs restricted to
original/listed items; a child ID is not another tree identity.

Replace `NoteContent.compressed_text` with the required `document_id` in the
native read contract. The old text projection is no longer needed after
removing the separate compressed dialog. Update `NoteContent::to_json`, DTOs,
generated TypeScript, exact-key guards, fixtures, and consumers together.
Retain legacy `compressed_text` handling at migration/import/sync boundaries;
that handles stored data, not this removed response field.

Thread the optional mode through `Application`, `NotesService`, and
`WorkspaceConfigStore` to `note_get()`. Default full-text reads preserve audio,
ChaWeb, and other callers that do not request a mode. ChaWeb still returns its
existing `{ item, document }` response and does not expose `document_id`.

### Saves

Reuse `notes.save`. Its `id` is the loaded document row ID:

```json
{
  "id": 87,
  "document": {
    "type": "doc",
    "content": [{ "type": "paragraph", "content": [
      { "type": "text", "text": "Edited compressed text." }
    ] }]
  }
}
```

The ID has always identified the row being saved; extend the allowed targets
to compressed document rows. Original callers keep their existing behavior.
Do not add a mode to writes or create a compressed row from an editor save.
If the loaded row was deleted, saving fails. It cannot redirect to the
original or to a newly created compressed row with a different ID.

Add a small internal document-row lookup for save and scroll operations.
For a compressed row, join its original parent and require that the parent is
an ordinary `compressed = 0`, `kind = 'note'` row. Keep `find_item()`,
`note_find_item()`, tree listing, rename, delete, and read-only setters scoped
to original items. Do not remove their compressed-row filter globally.

Within the existing save transaction:

1. Resolve the document row and its owning original note.
2. Enforce the original note's read-only setting against the document being
   edited. Apply the existing `read_only_note_change()` rules: permitted
   highlights/comments remain available, but ordinary text changes require
   making the logical note editable.
3. Apply the same document validation, compact JSON encoding, zlib storage,
   and 12 MiB uncompressed JSON limit. A valid empty edited document is allowed;
   the provider's nonempty-answer requirement does not apply to manual edits.
4. Update only the selected row's `content_json`.
5. For full text, retain folder annotation rebuilding and its return value.
   For compressed text, skip that rebuilding and return
   `annotation_created: false`.
6. Flag the **original note ID** for sync, using `sync_flag_note()`. Existing
   `note_fields()` already includes its child's `compressed_content`.

Known effect: sync sends the normal `note.update` record for the original
note. A machine that applies this record deletes the note's audio, as it does
for a full-text edit, although the full text did not change
(`apply_note_update()` in `note_storage.cpp`). This design accepts that
effect. It does not add a separate record kind or a content comparison.

The compressed row's stored name and `read_only` column are not authoritative
UI metadata. Use the original note's name and permission. Do not start copying
those fields between rows on every rename or permission change.

### Reading positions

Use each document row's existing `scroll_top`. `notes.saveScrollPosition`
receives the loaded document ID and uses the same narrow row resolver.
Switching saves the old document's position and restores the new document's
position. Scroll-save failure remains nonfatal to navigation.

No schema migration is needed. This feature persists the compressed reading
position locally; it does not extend sync or folder import/export contracts
to preserve that position. Existing transfer behavior remains in place.

## 7. Which operations use which identity

| Operation | Target and behavior |
| --- | --- |
| Type, paste, cut, formatting, find/replace, undo/redo | The loaded editor document, subject to shared read-only rules. |
| Copy, Copy All, Copy JSON | The loaded document or selection. |
| Highlights, comments, local headings/highlight navigation | The loaded document. Compressed annotations are saved locally in that document. |
| Prompt paragraph, file upload, file path, web-page import, inserted picture | The document selected when the operation starts. |
| Save and scroll | The loaded `document_id`. |
| Rename, Make editable/read-only, tree Delete, Copy Ref ID | The original note identity. Delete removes both versions through the existing cascade. |
| Compress / Cancel compression | The original note. Compression input remains full text. Use the blocking dialog and its Cancel action. |
| Folder compression | One blocking operation, processing notes sequentially with existing missing-only eligibility. Do not replace existing manually edited compressed versions. |
| Chat attachment and model context | Original note reference, with the existing compressed-preferred text reader. |
| Generate audio | Existing original-note operation using full text. |
| Play note audio in the editor | Available only when the loaded document is full text and existing audio conditions are met. |

Full-text audio uses block positions from a different document. Disable its
editor playback actions and stop active note playback when switching to a
compressed document. Do not pass compressed blocks to the original audio
cache. Existing audio settings can remain, but cannot start playback of the
wrong text. Separate audio for compressed documents is outside this feature.

Audit helpers that currently equate `activeNoteId` with the displayed content.
In particular, `App.getNoteLength()` must not use a short compressed document
to bypass confirmation when deleting a long original. It uses the open
editor's text only when the loaded document is the full text. Otherwise it
reads the full text. Keep the existing 16-character confirmation rule. Reading
failure continues to require confirmation as today.

### Imports that finish after navigation

Extend the existing import target to carry `{ noteId, documentId }`. Capture
it at invocation. Use `documentId` as the existing run map's key and for save
matching. Do not introduce another import manager.

If the same document is still open, merge or replace through that editor and
its save handle. Otherwise, read the appropriate mode for the original note
and verify that the returned `document_id` still equals the captured ID before
using a temporary editor. A missing/replaced compressed row is an error, not
permission to import into full text.

Changing modes during retrieval does not redirect or cancel the import. Its
short merge/save phase still uses the existing note-action guard. Reload only
an open editor for that exact document, not the other version of the same
logical note. Imports into newly created notes target their full-text row.

Retain existing import side effects, including applicable automatic renaming
and making an imported note read-only. Apply those operations to `noteId`,
because the name and permission belong to both versions. This means a file
import into compressed text can make the logical note read-only, just as a
file import into full text does today.

## 8. Blocking compression

### Interaction and progress dialog

A deliberate Compress request replaces the existing compressed version,
including manual changes saved before the request. It does not merge them
with the model result. The progress dialog is an operation window, not an
additional confirmation step.

Block interaction with the current application window for the duration of
compression. This includes the editor, tree, mode control, chat input,
navigation, and vault changes. Use a modal `<dialog>` and the existing app
operation guards so keyboard shortcuts and native menu commands cannot bypass
the modal. The backend job stays asynchronous, as today; do not block the UI
thread.

```text
Single note                         Folder operation

┌────────────────────────────────┐  ┌────────────────────────────────┐
│ ◌ Compressing “Research”…       │  │ ◌ Compressing “Chapter 3”…      │
│                                │  │   Note 3 of 8                  │
│                      [Cancel]  │  │                      [Cancel]  │
└────────────────────────────────┘  └────────────────────────────────┘
```

Show only actionable operation state and the Cancel button. Do not add a
separate title, legend, or helper paragraph. Use an indeterminate indicator;
the provider does not supply a reliable percentage or remaining time.
Client-side stages are sufficient: `Saving note…`, `Compressing “name”…`,
`Cancelling…`, and `Opening note…`. Use an accessible dialog name, focus trap,
and a polite live status region. Cancel receives initial focus.

Escape invokes Cancel. A backdrop click does not dismiss an active operation.
While cancelling, disable repeated cancellation and keep the dialog open
until `notes.compressCancel` returns. When the job ends, the dialog reloads
the selected document if needed and closes. The dialog does not report
success or failure. Settings → Events records the outcome of each job, as
today. Cancel during the initial save prevents the start after that save
settles; it does not undo or interrupt an admitted save. During the final
`Opening note…` stage, disable Cancel because the job has already ended.

### Save, run, and restore

1. Acquire the existing note-action guard and show the modal. Freeze and flush
   the open editor, including when a different note is being compressed.
   Wait for any admitted save. If saving fails, do not start compression.
   Close the dialog, preserve the editor, and use its existing save-error
   handling. The user can retry the command after resolving the save.
2. Keep the action guard for the whole operation. Background imports, also
   for the target notes, may retrieve content but wait at their normal
   merge/save guard. After the operation, they insert into the document that
   is current then, so they cannot overwrite the new result with an older
   copy. Prevent new user actions while the dialog is open.
3. Start compression using the saved original text, and wait until the job
   ends, as described below. Keep the existing minimum size, summary
   exclusion, provider validation, source-change check, and atomic commit of
   the result and success Event. A refused start closes the dialog and shows
   the error in the tree, as today.
4. After the last job ends, refresh the tree. If the selected logical note was
   a target and mode is `compressed`, dispose of its old editor and load the
   document again before releasing the guard. Do this for every outcome: the
   reload shows a new result, the previous compressed document, or the
   full-text fallback. Use a fresh undo history and the loaded row's saved
   scroll position. If that reload fails, show the normal opening error;
   never unfreeze a stale editor that could save over a completed result.
5. In `full_text` mode, or when the selected note was not a target, keep the
   current document and restore its editability. Keep the global mode and
   tree selection unchanged in every case.
6. Release the guard and restore focus when the operation UI closes. Resume
   other operations waiting on the existing guard.

This excludes manual editor saves during generation. No target fingerprint,
document revision, or conflict protocol is needed for this workflow. The modal guards
this app instance, not other processes or devices. Retain existing external
sync conflict semantics and the backend's original-source comparison; this
feature does not introduce a distributed vault lock.

### Detecting the end of a job

Keep `notes.compressStart`, `notes.compressionStatus`, `notes.compressCancel`,
and the `notes.compressionChanged` notice as they are. The worker already
sends this notice to every window when a job starts and when it ends, and it
records the outcome in Events. The backend does not change.

1. Listen for the existing `cha-note-compression-changed` window event before
   the start request, so a fast job cannot end unseen.
2. Call `notes.compressStart`. `{ "accepted": true }` starts the wait.
   `{ "skipped": true }` is the folder operation's existing ineligible-note
   case.
3. After acceptance, and after each notice for that note, read
   `notes.compressionStatus`. The job has ended when `pending` is false. If a
   status read fails, keep waiting; Cancel still ends the wait.
4. The dialog does not know whether the job committed. It reloads the saved
   content as described above and closes. Settings → Events shows success,
   failure, cancellation, or a discarded result, as today.

Cancel calls the existing `notes.compressCancel`. It removes the job under the
lifecycle lock, so a late result cannot commit, and the client already waits
until the job is no longer pending. If the commit wins the race, the reload
shows the committed result. Window close and application shutdown keep their
existing behavior. No new reply type, connection ownership, deadline
exemption, or lost-reply recovery is needed.

### Folder compression

Keep the existing tree traversal and missing-only eligibility checks. Move
ownership of the loop into the app's single blocking compression action, so
one dialog and one note-action guard cover the whole selection. Start one
note at a time and wait until its job ends before starting the next. The
dialog's `Note i of N` counts notes examined, including skipped notes; it does
not imply a percentage of provider work.

Cancel cancels the current job and starts no more. Skipped notes do not stop
the loop. The dialog cannot tell a failed job from a successful one, so the
loop continues with the next note after a failed job; Events records each
outcome. A refused start stops the loop and shows the error in the tree.
Previously committed results remain saved. Do not queue all notes as detached
background jobs or add a backend batch subsystem.

## 9. Stored text and model serialization

Load compressed JSON directly into `createNotesEditor()`. Do not reconstruct
it from `compressed_text`, parse it as new Markdown on open, or run compression
again when saving. Reuse the same Tiptap extensions and validation as full text.

The previous storage change deliberately converted generated text into a
literal paragraph, preserving Markdown markers and embedded newlines. It did
not convert those markers into rich headings and lists. Preserve that choice.

The existing editor can normalize literal paragraph newlines to `hardBreak`
nodes during editing. The shared Markdown serializer emits those as Markdown
hard breaks, including a backslash before each newline. This is an existing
representation effect, not a reason to change all note serialization or add
a second compressed-document format. Verify it with an actual editor edit and
reopen test. Opening without editing must not initiate a save.

After edits, `note_get_text(..., prefer_compressed = true)` continues to
serialize the saved compressed JSON through `note_document_markdown()`. Future
chat attachments and missing-copy restoration use that result. Existing
chat-owned snapshots remain unchanged. Full-text editing does not invalidate
a saved compressed document; the user explicitly compresses again when needed.

## 10. Implementation map

| Area | Expected changes |
| --- | --- |
| `webapp/src/state/view.ts` and `webapp/src/components/App.tsx` | Session-only mode, guarded transitions, logical versus document identity, annotation-source navigation, whole-note actions, blocking compression and folder-loop ownership. |
| `webapp/src/components/NotesScreen.tsx` | Mode control, mode-aware loading, loaded document ID, save/scroll destinations, callback identity, audio gating. |
| `webapp/src/components/NotesTree.tsx` | Route Show compressed into the editor, remove the old dialog, delegate compression to the app's modal action. |
| A small compression dialog component | Active note/stage, optional folder count, Cancel, and modal focus behavior. |
| `webapp/src/notes/useNoteSave.ts`, `useNoteLeave.ts` | Reuse current lifecycle with the document ID. Change only what is needed for identity and integration. |
| `webapp/src/notes/useNoteImports.ts` | Capture both IDs and retain the exact document target through background work. |
| `webapp/src/api/client.ts`, `nativeClient.ts`, `resources/dto.yaml`, generated `schema.d.ts` | Optional read mode, required returned document ID, removal of the native text projection, strict validation. |
| `src/bridge/notes_dispatch.cpp`, application/service/store interfaces | Pass the read mode; retain exact document IDs for saves and scroll. |
| `src/storage/note_storage.h` and `.cpp` | Selected-document read, narrow writable-row lookup, parent permissions, compressed saves. Retain the current compression source check. |
| Existing unit, bridge, wire, and native tests | Update the native contract and validate the behaviors below. |

The compression job, its status and cancel requests, its notice, and the
bridge router do not change.

Keep `notes.list` and `NoteItem` unchanged. Existing compressed storage,
migration, sync record representation, and folder-transfer content handling
remain the basis of the feature. In particular, sync uses the original path
and existing `compressed_content` field, never a hidden child's path.

Implement storage/API support, then editor switching, then affected operations
and blocking compression. Remove the old dialog and obsolete tests once its
shortcut opens the editor. Do not modify documents in `docs/` as part of this
feature.

## 11. Validation and acceptance

Use focused regression tests in the existing suites. No live provider request
is needed to test the mode or job-commit races; use existing provider test hooks.

| Area | Required checks |
| --- | --- |
| Selection | All four rows of the selection table; empty compressed document; stale `has_compressed`; folders, annotations, and summaries. |
| Write isolation | Full-text edits leave compressed JSON unchanged and vice versa. Fallback saves still reach full text if a compressed row appears before autosave. A deleted target cannot redirect a save. |
| Permissions | Both versions follow the original read-only flag, including permitted annotation edits. A compressed row's default permission cannot bypass its parent's restriction. |
| Switching | Clean/dirty/in-flight saves; Retry, Stay, Discard; rapid note/mode changes; load failure; same-mode no-op; close and vault switch; separate undo histories and scroll positions. |
| UI state | Exact menu choices, correct check mark, visible fallback, keyboard access, read-only access, empty/loading/error access, toolbar hiding, startup reset, mode retained during navigation and vault changes. Compression modal focus, live status, Cancel/Escape, and blocked shortcuts/native navigation. |
| Commands | Copy All/JSON, formatting, search/replace, prompt insertion, pictures, highlights/comments, shared read-only changes, and deletion of the logical note. |
| Background work | File/page import targets survive a mode change; only the exact document is refreshed; imports fail if their target disappears; full-text audio stops and is unavailable on compressed content. |
| Compression | Flush before starting; source always full text; no editing while pending; an import for the target waits and then inserts into the current document; existing target deliberately replaced; changed source discarded; dialog waits until the job is no longer pending; compressed editor/fallback reloaded before unlocking for every outcome; full-text editor retained. |
| Compression lifecycle | Save failure starts no job; refused start; provider failure keeps the previous content and closes the dialog; a job that ends before the first status read; a failed status read keeps waiting; Cancel versus commit race; late result after cancellation. |
| Folder compression | Sequential execution, one modal/guard, truthful counts, missing-only skips, continue after a failed job, stop on a refused start or Cancel, no later submissions after cancellation, and preservation of completed results. |
| Annotations | Compressed highlights/comments persist without changing folder ANNOTATIONS; original aggregation still works; source links select Full text before locating the target. |
| Content | Actual edit/save/reopen of the literal compressed fixture, Unicode and whitespace, rich formatting, pictures, empty content, size limit, selected-document corruption, and model Markdown after editing. |
| Contracts and transfers | Read-mode validation; strict response keys and IDs; compressed save in sync under the original path as `note.update`; folder transfer preserves edited content; ChaWeb and original-note references retain their contracts. |

Run the webapp's generated-type check, typecheck, and relevant Vitest suites.
Build the affected C++ targets and run note storage, notes service, workspace,
bridge, sync, and ChaWeb adapter tests. Extend the existing native note
compression smoke scenario to verify the blocking dialog, edit a compressed
document, switch to full text, reopen the compressed version, and verify both
documents independently.

Manual acceptance should use a note with a compressed version and one without:
choose Compressed, edit each, switch modes, and reopen each. Verify that the
tree selection remains stable, the fallback is visible, read-only behavior is
shared, and only the displayed document changes. Compress the selected note
again and verify that editing is blocked until the new compressed document is
loaded. Cancel a single-note and folder operation. Restart the application
and verify Full text mode.

The feature is complete when compressed documents can be viewed and edited
through the normal editor without writes reaching the wrong version, while
the existing full-text workflow and the confirmed scope decisions remain intact.
