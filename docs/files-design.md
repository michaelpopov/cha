# File references: technical design

Date: October 30, 2026

Status: Technical design for implementation

Requirements: [files-prd.md](files-prd.md), including its approved UI and review
clarifications. This document specifies the implementation; it does not change
application code. The four product recommendations retained in the PRD are
used here: reject duplicate references per owner, permit identical names,
accept all existing readable note kinds, and use settings writability for
Character reference metadata.

## 1. Approach

Store a `file_references` array in the owner's existing configuration file.
Load it into `WorkspaceCharacter` or `WorkspaceForum`, and include it in the
existing Character and Forum detail responses. Add two native bridge mutations:
`fileReference.create` and `fileReference.delete`. Both support the two owner
types. Opening a reference uses the owner's existing detail read, with no new
reference-get endpoint.

Creation resolves and validates the source once, then captures its name. All
later reads and deletion operate on the saved metadata. There is no source
foreign key, name refresh, provider lookup, content copy, or inference change.

Use the existing lifecycle lock, vault epoch check, configuration-store mutex,
candidate validation, and SQLite configuration commit. Do not add a table,
schema version, index, generated entry ID, background worker, cache, or general
attachment framework. Metadata edits must not invalidate live sessions.

## 2. Implementation locations

| Area | Files | Change |
| --- | --- | --- |
| Reference value | New `src/workspace/file_reference.h` | Small shared owner/type enums and reference value; no service or registry. |
| Ref ID parsing | New `src/util/item_reference.{h,cpp}`; `src/runtime/text_input.cpp` | Move the existing private parser into the utility and reuse it without changing chat grammar. |
| Configuration load | `src/workspace/workspace.{h,cpp}` | Parse and validate the optional array for Character definitions, the Assistant, and Forums. |
| Configuration edit | `src/workspace/workspace_config_editor.{h,cpp}`, `workspace_config_store.{h,cpp}` | Add and remove one metadata entry through the existing edit path. |
| Application | `src/app/application.{h,cpp}` | Two short lifecycle-guarded mutations; no live-session invalidation. |
| DTO and native bridge | `src/runtime/protocol.{h,cpp}`, `src/bridge/bridge_protocol.{h,cpp}`, `workspace_dispatch.cpp`, `resources/dto.yaml` | Detail arrays, reference serialization, and two ordinary methods. |
| Client | `webapp/src/api/{client,nativeClient}.ts`, generated `schema.d.ts` | Reference types, guards, and mutation calls. |
| Screens and navigation | `webapp/src/components/{FileScreens,ForumScreens,App,Icons,Sidebar}.tsx`, `webapp/src/state/view.ts`, `webapp/src/styles/app.css` | Approved source controls, reference rows, shared read-only detail, per-owner selection, sidebar current-page state, and title handling. |
| Summary projection | `webapp/src/state/entityUpdates.ts` | Exclude `file_references` when projecting a Forum detail into a summary. |
| Build and checks | `CMakeLists.txt`; existing workspace, bridge, runtime, and webapp tests | Register the parser source and extend focused behavior checks and wire fixtures. |

Keep ChaWeb's separate client and HTTP API unchanged. Its application does not
expose these desktop file-management screens. Adding metadata does not change
the behavior of chat Ref IDs, `/detach`, folder stores, or summary tools.

## 3. Value and configuration format

### 3.1 Shared value

Define the following in the small `workspace/file_reference.h` header, which
both workspace and runtime DTO code can include without importing each other:

```cpp
enum class FileReferenceOwner { character, forum };
enum class FileReferenceKind { note, folder };

struct FileReference {
    FileReferenceKind kind;
    std::string ref_id;
    std::string name;
};
```

The owner enum is an operation parameter, not a stored field. The owning
configuration supplies owner identity. Add
`std::vector<FileReference> file_references` to `WorkspaceCharacter` and
`WorkspaceForum`, and to their runtime detail DTOs. Keep the existing filename
and Markdown collections unchanged. Reference names never enter those maps or
the prompt expansion path.

The stable entry key is `(owner type, owner ID, kind, ref_id)`. Match strings
case-sensitively. Do not use a display name, array index, or provider store ID
as identity. A note and folder reference with the same ID may coexist in
imported metadata; duplicate rejection applies to the same kind and ID.

### 3.2 TOML

Append array-of-table entries to the existing owner file:

```toml
[[file_references]]
kind = "note"
ref_id = "##00000A"
name = "005. Introduction"

[[file_references]]
kind = "folder"
ref_id = "##00000B"
name = "Research"
```

| Owner | Configuration row path |
| --- | --- |
| User-defined Character | Its existing `characters/<optional-groups>/<id>/character.toml` |
| Built-in Assistant | `system/assistant/character.toml` |
| User-defined Forum | Its existing `forums/<id>/config.toml` |

Use the registered configuration paths rather than reconstructing Character
paths from IDs. The Assistant follows the same special path already used by
`write_character_settings`. Other built-ins have no writable reference owner.

An absent field or empty array means no references. Remove the field when the
last entry is deleted. There are no provider fields, numeric source ID copies,
timestamps, or metadata sidecar files.

### 3.3 Loading and validation

Add one local `load_file_references` helper in `workspace.cpp`. Use it from
`load_character_config` for definitions, including `load_assistant`, and from
`load_forum_config`. Carry the result through `CharacterConfig` and
`LoadedForumConfig` into the published workspace values. Add `file_references`
to the respective allowed-field lists. Do not add it to Forum member overrides,
which remain prompt-variable overrides only.

The helper validates:

- The field is an array and each element is a table.
- `kind`, `ref_id`, and `name` are required strings. Kind is `note` or `folder`.
- Stored Ref IDs are already trimmed and accepted by the shared existing
  parser. Creation trims user input before writing it. Do not normalize case.
- Names pass `validate_display_label` from `storage/session_label.h`: valid
  UTF-8, 1–200 code points, no control characters or leading/trailing whitespace.
  Preserve their exact spelling. Do not call the note-creation name validator,
  which reserves `ANNOTATIONS`; that is a valid captured annotation name.
- No two entries have the same kind and Ref ID. Names need not be unique.

Use the existing file-path-aware configuration error handling. Warn about
unknown entry fields through `warn_unknown_fields`, as for other configuration.
Wrong types or invalid values in known fields are validation errors, not
silently dropped entries. Do not introduce a new maximum reference count.

Loading performs no note or store queries. A missing source, changed source
kind, missing credential, or absent/incomplete store cannot reject otherwise
valid saved metadata. This is also true during candidate validation after an
unrelated settings edit or configuration import.

Sort the published reference vector by `(name, kind spelling, ref_id)` using
case-sensitive `std::string` ordering, matching existing filename ordering.
This is a derived view, not a second persistent list. Keep actual files in their
current order; the UI appends the already sorted reference vector after them.

## 4. Shared Ref ID parser

Move `parse_item_reference(std::string_view)` from the anonymous namespace in
`runtime/text_input.cpp` to `util/item_reference.{h,cpp}`. Keep its existing
behavior: surrounding whitespace is trimmed, the complete input must match
the current `##` plus six base-62 characters, and character case matters. Keep
its `std::optional<std::int64_t>` result. Use it from chat input, reference
creation, deletion, and saved-metadata syntax validation.

The existing parser accepts `##000000` as numeric zero. Creation resolves this
as no item through `note_find_item`, which already rejects nonpositive IDs.
Do not add a synthetic-root code path. Do not change the existing Ref ID
formatter or widen the chat grammar as part of this work.

For saved configuration and delete requests, require the stored/selected
trimmed spelling. For create requests, trim once before parsing and persistence.
There is no further canonicalization operation or second frontend parser.

## 5. Native mutations and commit

### 5.1 Store and editor interfaces

Add these narrow operations to `WorkspaceConfigStore`:

```cpp
FileReference create_file_reference(
    FileReferenceOwner owner, std::string_view owner_id,
    FileReferenceKind kind, std::string_view ref_id);

void delete_file_reference(
    FileReferenceOwner owner, std::string_view owner_id,
    FileReferenceKind kind, std::string_view ref_id);
```

Add matching editor operations that append one captured value or remove one
`(kind, ref_id)` entry. A small private path helper selects the existing owner
configuration. Use `rewrite_toml` to change only `file_references`; preserve the
other owner fields. Preserve untouched reference tables, including ignored
unknown fields, rather than reconstructing the whole array from the parsed
workspace vector. Candidate loading supplies normal validation.

Both store operations run inside `impl_->edit`. In the writer, validate owner
existence and `character_settings_are_writable` or `forum_is_writable`. This
allows the Assistant's metadata without changing its text-file permissions.
No settings API may create references on Guest, Entrance, or an unknown owner.

Return an empty affected-forum list from the writer. The Application mutations
must not call `apply_edit`, `invalidate_affected_sessions`, or a refresh that
restarts a session. This follows the existing picture metadata path.

### 5.2 Create sequence

1. `Application::create_file_reference` holds `lifecycle_mutex` and calls
   `require_admitted(epoch)`, as existing native configuration operations do.
2. The store enters `impl_->edit`, which holds its existing mutex, requires
   main-process ownership, and supplies the current published workspace and
   disposable editor. Keep the existing lock order: lifecycle, then store.
3. Check the owner and its write permission. Trim and parse the submitted Ref
   ID; reject invalid syntax with the PRD's malformed-ID message.
4. Check the owner's saved references for the same kind and trimmed ID. Reject
   duplicates without changing their captured name. This check uses saved
   metadata even if its source has since disappeared or came from another vault.
5. Call `note_find_item` directly on `impl_->database` while the edit holds the
   store mutex. It reads item metadata, not note content. Do not call a public
   store getter from inside the writer: it would acquire the same mutex again.
6. Reject a missing item. Note mode accepts `NoteItemKind::note` and
   `NoteItemKind::annotation`; summaries are normal notes. Folder mode requires
   `NoteItemKind::folder`. Return the PRD's wrong-kind message when appropriate.
7. For folder mode, call `folder_store_folder_is_ready` on that same database
   and exact item ID. It checks the existing local association only. Both no
   association and incomplete upload use the same PRD error. No credential or
   remote-resource access is needed.
8. Construct `{kind, trimmed_ref_id, item.name}` and append it to the owner's
   TOML array. Use the captured value as the create result.
9. Let the existing edit path validate configuration rows, load the candidate,
   commit the changed rows, and publish the workspace. Return success only
   after this completes. If it fails, return no created result.

The lifecycle and store locks already serialize the relevant local operations.
Do not add a source reservation, source transaction spanning configuration,
revision check, or race-specific lock. The existing configuration commit remains
the only write transaction. There is no remote work to coordinate.

### 5.3 Delete sequence

Use the same lifecycle/epoch and store-edit boundary. Validate owner permission
and the selected kind and Ref ID. Locate the entry by its saved key, remove its
table, and commit. Do not resolve the source or check store readiness. If the
entry is already absent, return `not_found` through the ordinary error path.
Deletion remains available for dangling or copied references.

There are no source rename, move, edit, store removal, or source deletion hooks.
Owner settings and name edits already preserve other TOML keys. Owner deletion
already removes its configuration directory, so it removes references without
extra cleanup code.

### 5.4 Failure behavior

Use existing `ApplicationError` codes, not a new error type or numeric status
system. Expected input/type/readiness/duplicate failures use `invalid_argument`;
missing source, owner, or saved entry uses `not_found`. Preserve the specific
PRD source-not-found and validation messages when translating store errors.
Do not use the Markdown helper's generic “That file was not found” translation
for these failures.

Log underlying persistence failures and return `internal_error` with
`The reference could not be added. Try again.` For deletion, use the existing
detail-action error handling with an appropriate deletion failure message.
Preserve `vault_changed`, application-unavailable, and existing maintenance or
secondary-process errors rather than converting them to an input error.

Candidate validation or SQLite commit failure leaves both the committed rows
and the published workspace unchanged. Keep this behavior by using the existing
edit implementation, not by writing rows directly. Do not add automatic retry,
idempotency tokens, or a pending-operation record. If a response is lost after
commit, reopening the list shows the entry; another create hits the ordinary
duplicate check and cannot overwrite its name.

## 6. Wire contract and client

### 6.1 Detail values

Add this schema in `resources/dto.yaml` and corresponding native serialization
in `runtime/protocol.cpp`:

```json
{
  "kind": "note",
  "ref_id": "##00000A",
  "name": "005. Introduction"
}
```

`FileReference` requires all three fields; kind is `note | folder`. Ref ID and
name are strings with the native validation specified above. Add required
`file_references: FileReference[]` to `CharacterDetail` and `ForumDetail`.
Always serialize the array, including `[]` for old configurations and built-ins
with no entries. Populate it in `character_detail` and `forum_detail` in
`app/workspace_operations.cpp` from the loaded workspace value.

In `state/entityUpdates.ts`, extend `forumSummary` to remove `file_references`
along with `forum_markdown`, `markdown_files`, and `writable` before returning
the summary. Both `forum-created` and `applyForumUpdate` use this projection.
Keep the array out of `bootstrap.forums` and `sessionSnapshot.forum`; those
summaries must not become a second, stale copy of reference metadata. The
Character `rosterSummary` already selects its summary fields explicitly and
needs no new reference field.

Do not append reference names to `markdown_files`, modify file endpoints, or
add a `filename` to the reference value. Existing text and picture APIs keep
their contracts.

### 6.2 Mutations

Both methods accept exactly these required keys:

```json
{
  "owner_type": "character",
  "owner_id": "guide",
  "kind": "note",
  "ref_id": "##00000A"
}
```

| Method | Result | Meaning |
| --- | --- | --- |
| `fileReference.create` | `FileReference` | Resolve locally, capture the name, commit one new entry. |
| `fileReference.delete` | `{}` | Remove the saved entry identified by these keys. |

`owner_type` is `character | forum`. The request never accepts a name, readiness
flag, service, or remote store ID. Use `require_only_keys`, `require_identifier`
for the owner ID, and string/enum validation for the other fields in
`workspace_dispatch.cpp`. Ref ID is not a filename or owner identifier and must
not pass through those validators.

Register two enum values and method names in `bridge_protocol.{h,cpp}`. They
use the ordinary queue, require the context epoch, and do not change context.
They need no control-queue entry, subscription, completion event, router
background branch, or bridge protocol-version change. The existing workspace
dispatcher fallback in `bridge_router.cpp` handles them.

Add a request schema for these fields and generate `webapp/src/api/schema.d.ts`
with `npm --prefix webapp run api-types`. Export `FileReference` and the request
type from `api/client.ts`, and add:

```ts
createFileReference(request: FileReferenceRequest): Promise<FileReference>;
deleteFileReference(request: FileReferenceRequest): Promise<void>;
```

Add `isFileReference` and validate each detail-array element in
`isCharacterDetail` and `isForumDetail`. The frontend guard checks DTO shape;
native code owns semantic and source validation. Use existing `call` handling
in `nativeClient.ts`. Update the shared test client, Character/Forum fixtures,
the Character wire fixture, and native method-policy fixture together. Native
and webapp assets ship together; no missing-field fallback is needed for mixed
versions.

## 7. UI and navigation

### 7.1 New file

Extend the existing shared `NewMarkdownFileScreen` in `FileScreens.tsx`; do not
create a second form or reference setup dialog. Keep its upload, text,
filename, picture, reading, saving, and error behavior, and add:

```ts
type Mode = 'upload' | 'text' | 'note-reference' | 'folder-reference';
const [refId, setRefId] = useState('');
```

The shared Ref ID state is necessary because it must survive mode switches
independently of Filename. No resolved-source state is needed. Do not load the
Notes tree or call a lookup API as the user types.

Render the approved two-row action group before the active fields. Use the
existing primary class only for the active mode and set `aria-pressed` on all
four buttons. Hide Write text where it is already unavailable. Use two flex
rows or a small grid scoped to these actions, retaining the approved layout
when Write text is hidden. Reuse control sizes, colors, and spacing from the
existing styles. Reference modes render only Ref ID plus errors and actions.

Upload file continues to open the hidden file input without setting mode.
Only successful file reading selects upload mode for a picture or text mode
for text. Cancellation changes nothing. Keep `refId` on every mode change;
clear only validation errors when selecting another mode or changing the ID.
Do not run `normalizeMarkdownFilename` on a reference or copy Ref ID text into
Filename. Existing upload/text drafts remain in their existing local state.

Add file branches on mode. For a reference, require `refId.trim()` and call
`createFileReference` with the current owner and corresponding kind. Use the
existing busy flag to block repeated submission and conflicting controls.
On success, return to Character files or Forum detail so its ordinary load
shows the committed entry. Do not navigate to the source or open a text editor.
Other submit branches keep their existing behavior.

Keep the form keyed by owner type and owner ID so an owner change cannot reuse
another owner's draft. Use existing unmount and bridge stale-context handling
to ignore obsolete results. Vault-switch protection remains the bridge epoch
and native lifecycle guard, not a new frontend vault-state subsystem.

### 7.2 Lists and detail

Append reference rows to `CharacterFilesScreen` and the Forum `FileList` usage.
Use the native order and keep existing real-file rows unchanged. Show No files
only when both collections are empty. The new rows use the captured name,
chevron, reference-specific note/folder icon, and an accessible type label.
Keep any new icon components local to the existing `Icons.tsx` conventions.
Do not add type badges, group titles, provider status, or Ref ID subtitles.

Add two views to `MainView` in `view.ts`: `character-file-reference` beside
`character-file`, and `forum-file-reference` beside `forum-file`. Keep one
shared screen component. Add a nullable `reference` field beside `file` in
both `inspectedCharacter` and `inspectedForum`, using this selection type:

```ts
type FileReferenceSelection = {
  kind: 'note' | 'folder';
  ref_id: string;
};
```

Owner identity stays in its existing location: `inspectedCharacter.id` for a
Character and `currentForumId` for the inspected Forum. Do not duplicate the
owner type or ID in the selection, or introduce a global reference selection.
The active view supplies the owner type.

Add `inspect-character-file-reference` and `inspect-forum-file-reference`
actions to `AppAction` and `NavigationAction`. Each carries its owner ID and
the selection key, and uses the same owner-ID guard as the corresponding
existing file action. Selecting a reference sets that owner's `reference`,
clears its `file`, and opens the matching reference view. Selecting an actual
file sets `file` and clears `reference`. Keep the filename fields and existing
file routes unchanged. Reference keys also give React rows stable keys
independent of displayed names.

Initialize `reference` to `null` in the initial state and every existing
inspection initializer. Clear it when returning to the owner's file list or
detail, changing or deleting the inspected owner, and on `vault-context-reset`
for both owner inspections. Owner settings/name updates preserve the current
selection, as they preserve the existing inspection. Use the existing
navigation lifecycle for both new actions; no separate navigation guard or
cleanup mechanism is required.

In `Sidebar.tsx`, add `character-file-reference` beside `character-file` in
`settingsViews`, and `forum-file-reference` beside `forum-file` in `forumViews`.
The latter is already included in `settingsViews` through `...forumViews`.
Thus both reference screens keep Settings current, and only a Forum reference
also marks its current Forum row. A Character reference must not highlight
`currentForumId`, which can still name an unrelated conversation's Forum.
Preserve the existing `is-current` and `aria-current="page"` behavior.

Add both views to `navigationTitle` with a `null` result. The captured name is
shown in the detail itself; do not add an application navigation title for
either new view.

Implement one shared `FileReferenceScreen` in `FileScreens.tsx`, selected by
both new `App.tsx` view branches. Each branch supplies its owner type, the
existing owner ID, and that owner's selected reference. Reuse
`RosterDetailScreen` loading/error/refresh handling to read `getCharacter` or
`getForum`, then find the selected key in `file_references`.
Use a composite component key containing owner type, owner ID, kind, and Ref
ID. Loading and refresh read configuration metadata only; they never use a
Notes client or folder-store operation.

Show the captured name and selectable Ref ID with the approved back navigation
and Delete action. Use plain text for the captured name, not a filename input
or rename control. Reuse the existing confirmation-dialog and mutation-error
patterns, with the PRD's reference-specific deletion text. Do not add a panel
heading or legend. Gate deletion on freshly loaded settings writability for a
Character or writability for a Forum, even when the source is gone.

Delete calls `deleteFileReference`, composing the request from the supplied
owner and selected key, then returns to Character files or Forum detail after
success. Back navigation uses the same destinations. A failed delete keeps
the detail with an error.
If the owner or saved entry has disappeared during refresh, show the ordinary
not-found state with back navigation; never substitute a same-name entry or
open a source note as a fallback.

## 8. Lifetime and vault transfer

No code observes source changes on behalf of a saved reference. In the same
vault, source IDs are not reused under the current AUTOINCREMENT schema.
Keeping a dangling entry therefore does not bind it to a newly created source.
There are no changes to note or folder-store deletion operations.

Storing the array inside the owner file gives existing rename, settings,
backup, import, and deletion behavior without another integration layer.
Configuration-only copies keep the metadata but do not carry note identity.
Ref IDs in the destination may be missing or collide with different items.
They remain displayable and removable, and the saved duplicate key still
applies. Removing the copied entry and adding the destination reference is the
explicit way to capture the destination name.

Existing merges replace source-matching configuration paths in full. Because
the array is in that file, the source array replaces the destination array;
an absent source array also removes destination references in that replaced
file. Do not merge arrays or rewrite Ref IDs. A full vault copy retains notes
and reference metadata while its existing store-ownership clearing leaves
saved folder references intact. New folder additions still require readiness.

This design does not make cross-vault resolution safe: it stores no origin
identifier. The later usage design must address copied IDs before using these
entries to retrieve content. This phase never resolves them during use because
reference-driven retrieval and prompting are out of scope.

## 9. Validation plan

Extend existing tests at their current boundaries. Do not build a provider
simulator, a concurrency harness, or source-change subscription tests for this
metadata feature.

| Boundary | Checks | PRD acceptance criteria |
| --- | --- | --- |
| Shared parser and chat | Existing valid/invalid, case-sensitive, whitespace, and zero-ID behavior; chat attachment and detach grammar unchanged after extraction. | 3, 12 |
| Workspace load (`tests/workspace/unit_workspace.cpp`) | Absent/empty arrays; all owner paths including Assistant; invalid table/field/kind/name/ID; duplicate keys; annotation names and Unicode/punctuation; source-independent loading and ordering. | 1, 4, 8, 14 |
| Store (`unit_workspace_config_store.cpp`) | Add/delete round trips; capture name; readable kinds; exact-folder readiness for both services; missing and incomplete store rejection; duplicate and same-name cases; source rename/delete/store removal leave metadata; owner permission and settings preservation. | 1–8, 11, 13 |
| Existing store fault injection | Validation and SQLite commit failures publish no partial metadata; existing main-process admission remains enforced. | 9 |
| Bridge/application (`unit_bridge_protocol.cpp`, `unit_bridge_router.cpp`) | Exact request keys and result shapes, native permissions, error messages, stale epoch rejection; an existing live session stays live across reference create/delete and no provider calls occur. | 3, 10–12 |
| Existing vault copy/merge tests | Configuration-only copy with missing/colliding IDs; per-file replacement including an absent array; full copy with cleared stores; metadata remains inspectable and removable. | 13 |
| Frontend (`nativeClient.test.ts`, `state/view.test.ts`, `components/App.test.tsx`, `components/Sidebar.test.tsx`) | New DTO guards and calls; both owner flows; reference identity and same-name rows; no source API reads; add/list/detail/delete; retained Ref ID; chooser cancellation and text/picture mode selection; active styling; existing upload and text regressions. Check per-owner selection/reset and mutual exclusion with file selection, both null navigation titles, Settings current on both views, and the Forum row current only on a Forum reference. | 1, 7–9, 11, 12 |
| Summary projection (`state/entityUpdates.test.ts`, `state/view.test.ts`) | Create and update a Forum whose detail has nonempty `file_references`; assert `bootstrap.forums` and an affected `sessionSnapshot.forum` contain only summary fields and no `file_references`. | 12, 13 |

Use the existing store failure hooks rather than new failure machinery. Test
source independence by mutating the source through normal operations and then
reading the owner, not by scheduling a race between two local saves. Keep the
vault-switch check focused on existing epoch admission.

After implementation, regenerate API types, run `npm --prefix webapp run check`,
build `cha_tests` and `cha_app_tests` with the `ninja` preset, and run the relevant
CTest cases in `build/ninja`. Verify the approved UI at desktop and narrow
widths, including Assistant mode restrictions, keyboard submission, reference
detail selection, and deletion confirmation. Run existing upload, text-file,
picture, and chat-reference checks as regression coverage. This design-only
task does not claim those implementation checks have run.

## 10. Implementation order

1. Extract the shared parser and add the reference value, TOML loader, and
   workspace/editor/store mutations with focused load and persistence tests.
2. Add Application methods, detail serialization, bridge methods, DTO schemas,
   and wire/client checks. Confirm source validation occurs only on create and
   metadata edits leave live sessions intact.
3. Extend the shared New file form, both owner lists, and the shared reference
   detail/navigation using the already approved UI. Add the frontend behavior
   checks and perform visual verification.
4. Check vault transfer and failure cases, run the required regressions, and
   confirm that no prompts, provider operations, source hooks, or unnecessary
   metadata stores were added.
