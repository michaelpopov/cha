# Create a note from a local file path

Date: November 1, 2026

Status: Technical design for review. This document does not implement the feature.

## 1. Required behavior

In **New note**, the user pastes an absolute file path into the existing name
field and selects **Create** or presses Enter. CHA creates a note in the
selected folder, opens it, loads the file, and applies the same content-based
naming and read-only behavior as the current note file importer.

Loading starts on submission, not on paste or focus loss. Cancel performs no
creation or file read. Names and URLs retain their existing behavior. New
folder and Rename continue to interpret their input as a literal name. To use
a recognized path as a literal note name, create with another name and rename.

This extends the URL shortcut described in `docs/short.md`. The URL shortcut
is now present in the working tree. Build on that implementation, including
its existing loading messages and navigation behavior; do not reimplement
the earlier plan. Existing unrelated working-tree changes are outside this
feature.

## 2. Proposed UI

Change only the new-note field label from **Name or URL** to
**Name, URL, or file path**. Keep the text input, selected default name,
buttons, keyboard behavior, and dialog layout.

Form area of the existing dialog:

```text
┌────────────────────────────────────────────────────────┐
│ Name, URL, or file path                                 │
│ ┌────────────────────────────────────────────────────┐ │
│ │ /Users/me/Documents/plan.md                         │ │
│ └────────────────────────────────────────────────────┘ │
│                                    [Cancel] [Create]   │
└────────────────────────────────────────────────────────┘
```

After creation and successful list refresh, open the new note and reuse the
current loading indicator below the document and above the prompt. File
loading uses the existing retrieval, picture-loading, and saving messages.
Errors use the existing dismissible import-error area. Add no new dialog,
helper text, file picker, or import Retry button. The shortcut works with
the prompt hidden and preserves its existing draft.

These are proposed UI changes for review, not implemented UI changes.

## 3. Existing code to reuse

| Location | Current behavior |
| --- | --- |
| `webapp/src/components/NotesTree.tsx` | `submitCreate` detects a URL, selects an available default name, creates the note, and passes the URL through `finishMutation` after a successful list refresh. |
| `webapp/src/components/App.tsx` | `requestOpenNote` forwards the opening payload. `beforeCreateNote` handles saving the previous note. `setNoteReadOnly` flushes content before locking the imported note. |
| `webapp/src/state/view.ts` | `noteImportUrl` follows the note-opening lifecycle and clears when the note view becomes hidden, including layout changes retaining the selected note. |
| `webapp/src/components/NotesScreen.tsx` | The automatic URL effect uses `handledImportRequest` and an empty-editor check. `loadNoteFile(File \| string)` already reads, converts, replaces, names, and locks imported files. |
| `webapp/src/notes/fileImport.ts` | `promptFilePath` handles quoted desktop paths. `noteFileHtml` converts Markdown or literal text and obtains the content title. |
| `webapp/src/notes/pageImport.ts` | `promptPageUrl` recognizes URLs; `notePageName` normalizes imported titles. |
| `webapp/src/notes/tree.ts` | `availableNoteName` resolves sibling conflicts and reserved names. |
| `webapp/src/api/nativeClient.ts` | `readNoteTextFile` calls `notes.readTextFile` and returns `{ name, text }` or `null`. |
| `src/bridge/notes_dispatch.cpp` | Validates an absolute path, reads the file, and validates its text. |
| `src/util/text_file.cpp` | Reads regular files and enforces text validity and the existing size limit. |

Reuse the desktop bridge. No new backend method, database field, document
format, provider setting, or model call is required. The path refers to the
filesystem available to the running desktop application, as in the existing
prompt file import. This does not add browser access to arbitrary local files.

## 4. Recognizing input

Classify only new-note submissions, in this order: URL, absolute file path,
ordinary name. Classifying a path does not read it or check that it exists.
The existing reader makes that check after the new note opens.

Run `promptPageUrl` on the original input, as today. Only the file-path branch
then trims outside whitespace and removes one matching pair of surrounding
double quotes. A quoted URL does not acquire new URL-import behavior.
Preserve spaces and characters inside the path. Reject embedded
line breaks or NUL from path recognition. Do not interpret shell escapes,
expand `~` or environment variables, resolve relative paths, or decode a
`file://` URL. File extensions do not determine whether an input is a path.

| Input | Classification |
| --- | --- |
| `/Users/me/Documents/plan.md` | File path. |
| `"/Users/me/Documents/my plan.md"` | File path, with quotes removed. |
| `/plan.md` | File path, including a file directly under the filesystem root. |
| `C:\Users\Me\plan.md` or `C:/Users/Me/plan.md` | Windows absolute path. |
| `"C:\Users\Me\my plan.md"` | Windows absolute path, with quotes removed. |
| `\\server\share\plan.md` | UNC path. |
| `https://example.org/plan.md` | Existing URL import. |
| `plan.md`, `./plan.md`, `../plan.md`, or `~/plan.md` | Ordinary name. |
| `file:///Users/me/plan.md` | Ordinary name; file URLs are outside this feature. |

Path prefixes follow the existing desktop conventions: leading `/`, a drive
letter followed by `:\` or `:/`, or leading `\\`. The backend validates the
path for the current operating system. A directory or a path valid only on
another operating system produces the existing file-read error.

The current `promptFilePath` deliberately leaves a single-component POSIX
path such as `/todo` as prompt text. That ambiguity does not apply to this
explicit creation shortcut: `/todo` requests a file import here.

Factor the existing trimming, unquoting, control-character check, and absolute
prefix recognition into an `absoluteNoteFilePath` helper in `fileImport.ts`.
Use that helper for new-note recognition. Keep `promptFilePath` as a wrapper
that also applies its existing conservative POSIX rule. This supports root
files without changing how ordinary prompt text is interpreted.

## 5. One source payload

Replace the URL-only opening payload with one explicit two-case value:

```ts
type NoteImportSource =
  | { kind: 'page'; url: string }
  | { kind: 'file'; path: string };
```

A small `webapp/src/notes/importSource.ts` module can export this type and
`newNoteImportSource(text)`, which uses `promptPageUrl` and
`absoluteNoteFilePath`. It returns the matching source or `undefined` for a
normal name. This is a pure classification helper, not an import service.

Replace `importUrl` with `importSource` through `NotesTree.onOpenNote`,
`finishMutation`, `App.requestOpenNote`, the `open-note` action, and
`NotesScreen`. Replace `noteImportUrl` with `noteImportSource` in `AppState`.
Keep one optional payload; do not retain the old URL field alongside it or
introduce independent file-import state.

When a source is recognized, create the note once with
`availableNoteName(items, parentId, 'New note')`. Do not send the path as the
note name, so paths longer than the note-name limit remain usable. Keep the
existing leave/save check and captured parent. Pass the source through the
existing `finishMutation`; open and import only after an applied list refresh.

Keep all current payload lifecycle rules: an ordinary open replaces it with
`undefined`; close, vault reset, release, and hiding the note view clear it.
A chat action that keeps the note visible in Split View does not clear it.
The existing note-opening Retry forwards the source alongside `destination`.
Do not store the source in the route, prompt draft, database, or a queue.

Extend the existing automatic-start effect rather than adding another effect
for files. Once the editor and save path are ready, verify the current
editable context and that `importing === null`. Only then mark
`(notesEpoch, noteRequest)` handled and check that the editor is empty. Both
loaders already refuse to start while importing; consuming the request before
that check could lose the shortcut without starting a load. Include `importing`
in the effect dependencies so an unhandled request is reconsidered when idle.

Dispatch to `loadNotePage(source.url)` or `loadNoteFile(source.path)`. Ignore
the page result and file boolean results: they already handle completion and
errors. Handle the file's `null` result as specified in section 7. Capture the
editor, note ID, and epoch before calling the loader, and use that captured
context for the asynchronous result check. Do not reset the handled-request
ref on completion or effect cleanup.
Preserve the existing ref and nonempty-editor guard so re-renders, effect
replay, later edits, or dismissing an error cannot repeat the opening import.

## 6. File conversion, naming, and completion

Call the existing `loadNoteFile` directly. It must retain these behaviors:

1. Read via `client.readNoteTextFile(path)` and recheck editor identity, note
   ID, epoch, and editability after asynchronous work.
2. Convert with `noteFileHtml` and replace the document using `replaceNoteHtml`,
   with its existing picture handling, byte limits, and whitespace behavior.
3. For a default-named note, rename from a nonempty content title using
   `renameImportedNote` and `availableNoteName`. Preserve a custom name chosen
   by the user while the read was pending.
4. Save and make the imported note read-only through `lockImportedNote` and
   the existing flush-before-lock path. Reuse the current loading state and
   error handling, including identity-only cleanup after successful locking.

Markdown extensions `.md`, `.markdown`, `.mdown`, and `.mkd`, ignoring case,
use the first rendered heading (`h1` through `h6`) as the title. Setext headings
also work. Title normalization and sibling suffixes remain unchanged.

Other files load as literal text. Plain text, HTML treated as literal text,
and Markdown without a heading provide no content title; they keep the
available default name. Do not derive a fallback from the filename or first
plain-text line, and do not add title generation.

The existing reader accepts valid UTF-8 text up to 256 KiB, rejects binary or
empty input, and requires a regular file. This feature does not add PDF,
Word, EPUB, or other document conversion. Existing Markdown sanitization,
image handling, and the editor's document limits still apply.

File import replaces content, unlike page import which appends. Keep that
existing distinction explicit: text entered during the file read can be
replaced when it completes. The automatic effect skips a note already
nonempty at startup. Do not freeze the editor for the whole file read or
change the established manual file-import policy in this feature.

## 7. Failures and navigation

`readNoteTextFile` can return `null` if the final filename contains whitespace
and either the file is missing or the filesystem status check fails. Spaces
only in a parent directory do not cause this fallback. The existing prompt
flow then treats the submitted input as text.
For an explicit new-note file source, **never insert the path as content**.
If `loadNoteFile(path)` returns `null` and the same editable context is still
current, show `The file could not be loaded. Check the path and permissions.`
in the existing error area. Perform this check in the automatic file branch;
keep `loadNoteFile`'s existing return type and the prompt's `null` fallback.
A late `null` result after leaving the editor must not show an error in the
new note.

| Event | Result |
| --- | --- |
| Creation fails or the user stays in the previous note | No file read starts. Preserve the dialog input and existing error/leave behavior. |
| List refresh fails after creation | Keep the existing list error. The empty note exists but does not open or import. List Retry refreshes only the list. |
| Missing, unreadable, non-regular, invalid-text, empty, or oversized file | Show the existing read/import error. With no user edits, the new note stays empty with its default name. Do not fall back to creating a path-named note or inserting the path. |
| No usable title | Complete the import and keep the default name. |
| Rename fails | Keep imported content and the existing rename warning; recover with Rename. |
| Save or read-only change fails | Keep content and existing save/read-only recovery. Do not reload the file. |
| User leaves the note, creates another note, hides its view, or changes vault before insertion | Abandon the unfinished import. With no edits, the original note stays empty and default-named; no error is shown in the new view. A late result must not modify the new editor. |

There is no automatic retry or background completion. Recover by returning
to the note and using Upload, or by deleting it and creating it again. The
prompt is also available for paths recognized by `promptFilePath`. For a
root-level path such as `/plan.md`, use Upload or the new-note dialog: the
prompt deliberately inserts that input as text. Do not widen prompt path
recognition just to provide recovery for this shortcut.

Wait for one import to finish before creating the next if both must complete.
Content already inserted follows the normal save/leave path. Already
submitted mutations can finish against their original note.

## 8. Implementation and validation

Change only the source classification, opening payload, automatic dispatch,
new-note field label, and the explicit-file `null` error handling. Reuse the
file reader and importer. Update existing URL callers and tests to the new
payload shape; remove the obsolete URL-only field everywhere it is used.

Extend existing URL-shortcut and file-import cases where they already cover
the behavior. Add focused cases for the new file-source branch rather than
duplicating the existing lifecycle and conversion suites:

- Source classification: quoted paths, spaces, root files, Windows drive and
  UNC paths, URLs (including unchanged quoted-URL behavior), ordinary names,
  unsupported relative syntax, and embedded line breaks/NUL. Keep existing
  prompt recognition tests unchanged in meaning.
- `NotesTree.test.tsx`: create one available default-named note in the chosen
  parent, pass the file source, accept a path over 200 characters, and preserve
  Cancel, leave refusal, normal-name, folder, URL, and failed-refresh behavior.
- `state/view.test.ts`: carry either source and clear it through the same
  lifecycle, including hiding a retained selected note.
- `NotesScreen.test.tsx`: one automatic file import after editor/save setup;
  hidden prompt and preserved draft; heading-based naming and default fallback;
  no duplicate import on replay or after failure; an active import does not
  consume an unstarted opening request; explicit `null` produces an error
  rather than text; existing prompt `null` fallback still works; a late `null`
  or successful result does not affect a different note. Preserve file
  conversion and error tests.
- `Notes.test.tsx`: dialog-to-content flow saves and locks the note, updates
  its title with sibling conflict handling, and requires no prompt submission.
  Retain the URL integration tests and hidden-view/remount checks.

After implementation, run from `webapp/`:

```sh
npm run typecheck
npm test -- src/notes/importSource.test.ts src/notes/fileImport.test.ts src/components/NotesTree.test.tsx src/state/view.test.ts src/components/NotesScreen.test.tsx src/components/Notes.test.tsx
```

Manually check an existing Markdown file, a plain-text file, a quoted path
with spaces, and a missing path on the relevant desktop OS. Verify title,
saved content, read-only state, and prompt preservation. This design document
has not run those implementation checks.
