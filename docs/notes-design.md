# Referenced notes in chat: technical design

Date: October 30, 2026

Status: Technical design. This document does not implement the feature.

## 1. Scope and confirmed behavior

Use note references already registered in a Forum's and a Character's
`file_references` settings as context for model conversations. Folder references
and cloud file-store access are outside this design.

The user confirmed these rules:

1. Copy a referenced note when the chat first uses it. Do not read its current
   content again on every request.
2. Select compressed text if it exists when making that copy. Otherwise use
   the original note converted to Markdown.
3. Keep existing chat-added note behavior: a saved copy changes only when the
   user explicitly adds the source note again.
4. Once copied, a reference stays active in that chat even if it is removed
   from the Forum or Character settings.
5. Forum and Character references to the same note share one saved copy per
   chat.
6. Include a note once per target's system prompt. Explicit chat context takes
   priority over Character context, which takes priority over Forum context.
   This priority must not determine or change a note's position in the prompt.
7. Continue discovering newly registered references on each request. Changes
   in source category alone must not reorder existing note context.

Welcome has one exception to saved-copy retention: when the active vault path
changes, remove all Welcome note copies and their scopes, including explicit
chat additions. Numeric note IDs are local to a vault and cannot identify the
same source across a vault switch. Ordinary chats remain in their own vault.

A Character reference applies only to requests addressed to that Character.
A Forum reference applies to every Character in the chat. An explicitly added
chat note applies to every Character, as it does today. The design aims to keep
note context stable for prompt caching while keeping implementation simple.

## 2. Existing code to extend

| Code | Current responsibility |
| --- | --- |
| `src/workspace/file_reference.h` | `FileReference { kind, ref_id, name }`, with note and folder kinds. |
| `src/workspace/workspace.{h,cpp}` | Loads references into `WorkspaceForum` and `WorkspaceCharacter`; builds the base `CharacterDefinition::system_prompt` without reading note storage. |
| `src/util/item_reference.{h,cpp}` | Parses a CHA Ref ID into a vault-local numeric item ID. |
| `src/session/session_open.cpp` | Supplies the controller's `NoteReader`, using `get_note_text(id, true)`. |
| `src/storage/note_storage.cpp` | `note_get_text` selects compressed text when requested and available; otherwise it converts the original document to Markdown. It supports ordinary, summary, and annotation notes. |
| `src/session/session_controller.cpp` | `add_note` reads and saves a chat copy. `start_generation` appends saved chat context to a request-owned Character definition. |
| `src/storage/session_database.{h,cpp}` | `SessionJournal::add_note` upserts `session_notes`; `notes_prompt` formats saved notes and text files. |

Keep note lookup in the session layer. Do not make `Workspace` depend on a
database or expand references into workspace prompt templates. All providers
already receive the resolved system prompt through `CharacterDefinition`.

## 3. Saved copies and scope

Reuse `session_notes` as the single saved text store. Add one flag to
distinguish explicit chat additions from automatic reference copies:

```sql
explicitly_added INTEGER NOT NULL DEFAULT 1
    CHECK (explicitly_added IN (0, 1))
```

Existing rows are explicit chat additions, so the default preserves their
meaning. Automatic copies must insert `explicitly_added = 0`. Explicit
`add_note` operations must set it to `1`, including when updating an existing
automatic copy.

Add one table for the scopes that have used an automatic reference:

```sql
CREATE TABLE IF NOT EXISTS session_note_scopes (
    session_key INTEGER NOT NULL,
    note_id INTEGER NOT NULL,
    character_id TEXT NOT NULL,
    PRIMARY KEY (session_key, note_id, character_id),
    FOREIGN KEY (session_key, note_id)
        REFERENCES session_notes(session_key, note_id) ON DELETE CASCADE
) STRICT;
```

An empty `character_id` means Forum scope. A nonempty value is the stable ID
of the Character that used the reference. The session already identifies the
Forum, so no additional Forum ID is needed.

The table records durable inclusion, not the current settings list. Do not
delete scope rows when a settings reference or source note is deleted. Do not
give these rows foreign keys to source notes or current Character settings.
This preserves copies and their scope after source and settings changes.

This is the minimum extra state required by the confirmed behavior. A flag
alone cannot identify which Characters may receive a copied note. A scope
table alone cannot distinguish existing explicit chat notes from automatic
copies. Separate text copies per owner are unnecessary.

## 4. First-use materialization

At the start of `SessionController::start_generation`, use the current
workspace settings to discover new references:

1. Inspect the chat's Forum references and process entries of kind `note`.
2. Inspect each target Character's references and process entries of kind
   `note` for that target only.
3. For each entry, parse `ref_id` with `parse_item_reference`.
4. Call `journal_.add_note_scope(note_id, character_id)`, using an empty
   Character ID for Forum scope. It returns true if a saved copy exists and
   ensures that the requested scope exists. In that case, do not read the source.
5. If it returns false, call the existing `read_note_` callback. Check the
   selected text size, then insert the copy with `explicitly_added = 0` and
   its scope in one journal transaction.

Use `NoteText.name` and `NoteText.text` from the first copy. The name captured
in settings remains file-list metadata; it is neither a lookup key nor a
replacement for the note's name at copy time. A later source rename does not
change the saved prompt heading.

Only targeted Characters acquire Character scope. A multicast processes the
Forum once and each selected Character once. A Character that has never been
targeted does not acquire its references merely because it belongs to the
Forum.

First use means preparing a model conversation request. Saving a reference
does not require a successful model response. Once saved during preparation,
the copy remains even if the generation is subsequently stopped or fails.
Do not add rollback bookkeeping tied to provider outcomes. A failure to save
the copy and scope rolls back that materialization transaction.

There is no background synchronization, refresh timer, content hash, or
compression job. Missing references can be tried again on the next request
because no copy or scope was recorded for them.

### Ref IDs copied between vaults

Resolve each configured Ref ID against the currently active vault. A
configuration-only copy or merge preserves reference metadata but does not
preserve the source note's identity across vaults. If that ID identifies a
different readable note in the destination vault, use that destination note.
If it is missing or identifies a folder, follow the omission behavior in
section 7. Do not search by name or require a captured-name match: a source
rename is valid.

When a source is actually read for a new copy, compare its current name with
the captured settings name. If they differ, log a warning with the owner,
Ref ID, and both names, then continue. This is a diagnostic only: a different
name can mean a rename, and matching names cannot prove source identity. Do
not revalidate names or reread text when reusing a saved copy.

After copying or merging configuration, the user must check its note
references and remove and add entries again to capture the intended
destination-vault references. This does not replace a copy already acquired
by a chat; section 7 describes recovery for existing chats. There is no
cross-vault source registry or identity migration. Replacement cloud stores
apply to folder references and remain outside this design.

## 5. Compression and explicit refresh

Use the existing `NoteReader` contract, which calls
`WorkspaceConfigStore::get_note_text(id, /*prefer_compressed=*/ true)`.
Compression is stored as `note_items.compressed_text`; it is not a second
note with a separate Ref ID.

The presence of compressed text determines selection. Do not add a freshness
test or compare the original and compressed lengths. Do not combine both
versions or automatically compress an uncompressed note.

After copying, later source edits, renames, compression, or deletion do not
change the chat's saved text. This rule also applies when another Character
later references the same note: it shares the already saved copy.

Explicitly adding the note again uses the existing source-read path, selects
the currently available compressed or original text, updates the single saved
row, and sets `explicitly_added = 1`. It becomes chat context for every target
and takes chat priority without changing its sort order. Keep its existing
scope rows; they need no update.

Consequently, using explicit add to refresh a Character-only note also makes
that note visible to every other Character in the chat. This follows the
confirmed explicit-add behavior; there is no special scoped refresh command.

For the existing add-note notice, treat a row as a previous explicit addition
only when `explicitly_added = 1`. Promoting an automatic copy uses the existing
"Added note" notice; refreshing an explicit copy uses "Updated this chat's
copy". No new control or notice type is required.

## 6. Prompt selection, precedence, and order

For each replying Character, select saved rows where any of these is true:

- `explicitly_added = 1`;
- a Forum scope row exists;
- a scope row exists for the replying Character ID.

Query from `session_notes` and use `EXISTS` for scope checks so multiple scope
rows cannot duplicate a note. Compare identity by numeric note ID, never by
name or text. Notes with identical names and different IDs remain separate.

Order the combined eligible note set by `note_id`, regardless of source. This
reuses the existing chat-note order and requires no saved ordering state.
Forum, Character, and chat are sources of eligibility, not separate output
blocks. Continue processing settings references in Forum then Character order,
but do not use discovery order or source priority to sort the resulting prompt.

Explicit chat priority means that a deliberate re-add refreshes the shared
copy and makes it visible to every target. Character and Forum references use
that same saved row. No separate source-specific text or category calculation
is needed for formatting.

Append context to the existing base system prompt in this order:

```text
existing base system prompt
all eligible saved notes, ordered by note_id
existing chat-added text files
existing maintenance context, when applicable
```

Use the existing `# Note: <saved name>` and `# File: <name>` formats and blank
line separators. Do not add new source-section headings. Keep text files
ordered by name. With no new references, this preserves the existing context
and ordering.

For example, suppose note A (ID 10) has Forum and Guide scopes, note B (ID 20)
has only Guide scope, and note C (ID 30) has Forum scope and was explicitly
added to the chat:

| Reply target | Note order |
| --- | --- |
| Guide | A, B, C |
| Other | A, C |

Removing A and B from settings does not change this table. Their saved scopes
remain. A note registered only for Guide never reaches Other's request.

Adding another scope for A, or explicitly adding A to the chat, does not move
A in either target's prompt. If the explicit addition reads the same name and
text, the note context for a target that already received A is unchanged.
Refreshing its content can still change prompt text, as intended.

A newly discovered note is inserted according to its ID and may therefore
appear before existing notes. New context is allowed to change the prompt;
this design does not add an append-only ordering mechanism. It prevents
reordering caused solely by changes in source priority.

## 7. Session and failure behavior

| Event | Result |
| --- | --- |
| Add a reference to settings during an existing chat | Acquire its copy and scope when the relevant next request is prepared. |
| Remove a reference from settings after first use | Keep its saved copy and scope active in the chat. |
| Remove a reference before its first use | Do not acquire it. |
| Rename, edit, compress, move, or delete the source after copying | Keep the saved name and text. No source lookup is needed for an existing copy. |
| Add another scope for an already copied note | Reuse that copy, even if the source has since disappeared. Keep its sort order unchanged. |
| Close and reopen the chat | Restore behavior from the saved rows without rereading sources. |
| Start a different chat | Acquire independent copies on first use. |
| Switch the active vault path while Welcome is open | Delete all Welcome note copies, explicit and automatic, and their scopes before using the new vault. Preserve the transcript and chat-added text-file copies. |
| Reopen the same vault path, including a password change | Keep Welcome note copies, matching the existing path-based folder-reset rule. |
| Clear the conversation | Delete note copies and scope rows along with existing chat context. Still-configured references are acquired again on the next relevant request. |
| Delete the session | Existing session-note deletion cascades remove its scopes. |

Apart from Welcome's vault-change cleanup, there is no automatic removal or
refresh mechanism. Existing chat clear and explicit note-add operations
provide the relevant reset and refresh paths.

There is no aggregate note-size or token limit. Several individually valid
notes can together exceed a provider's context limit. Removing references
from settings does not remove copies already saved in a chat. To recover,
remove or reduce the unwanted settings references, then clear the conversation
or start a new chat. Clearing removes the chat history as well as its copies,
including explicit additions; do not re-add unwanted notes.
To replace incorrectly resolved copied Ref IDs, correct the settings entries
before clearing the affected chat or starting another one.

For a new reference whose source is unavailable, log a warning, omit that
reference, and include it in the existing per-request chat notice. Continue
with other context. This includes a deleted source, a Ref ID that resolves to
a folder, and a source that cannot be read or decoded. Keep the settings entry
intact. Include the session, owner, Ref ID, and reason in the log warning; do
not log note text.

Apply the existing 256 KiB per-note chat limit to the selected text before
creating an automatic copy. A large original is acceptable if its selected
compressed variant fits. If the selected text exceeds the limit, warn, omit
it, and report the omission in the same chat notice. Do not truncate, silently
substitute the original, or add an aggregate token budget. Empty readable notes
remain eligible, as with existing chat additions.

The user approved the following notice design on October 30, 2026. Use the
existing notice area below the conversation, with its current styling:

```text
…conversation…
Note context omitted: Guide — Chapter 1 (##00000A): exceeds 256 KiB.
[ Message…                                             Send ]
```

Use `Note context omitted: ` followed by each affected owner, captured
reference name, Ref ID, and a short actionable reason. Label Character sources
with the Character display name and Forum sources with `Forum <display name>`.
Reasons include `note unavailable`, `could not read note`, and
`exceeds 256 KiB`. A missing note and an ID that resolves to a folder may both
use `note unavailable`; do not add another lookup just to distinguish them.
For several omissions, separate the entries with semicolons in that notice.
Captured reference names identify the settings entries even when their source
cannot be read.

Gather note omissions during materialization and publish them after foreground
activation, which currently clears the notice. If folder access is also
omitted, append its existing notice as another line rather than replacing
either notice. Reuse `ControllerUpdate::notice` and the existing notice
retention behavior through generation completion. Add no new DTO, panel,
control, or persistent transcript entry for automatic note inclusion.

Each request retries unresolved references and reports any repeated omission.
This can repeat source reads, Markdown conversion, and logging until the source
or settings are corrected. Keep this behavior explicit; no failed-reference
cache, retry timer, or suppression state is required.

Catch expected source-read failures around the source read only. Journal
read/write failures must follow the controller's existing request-preparation
failure path, rather than being mistaken for missing source notes. Never send
a new automatic copy as durable context if its persistence failed.

The approved omission text is the only new visible behavior. Existing
explicit-add notices remain as described above; no new controls or warning
panels are needed.

## 8. Implementation boundaries

Use two journal operations for materialization:

- `bool add_note_scope(note_id, character_id)`: use `INSERT OR IGNORE ...
  SELECT` from `session_notes` to add the scope only if a copy exists, then
  check whether the requested scope exists. Return true for an existing scope
  as well as a newly inserted one, and false when no copy exists. A zero
  inserted-row count alone cannot distinguish those cases. Do not load text.
- `save_note_reference(note, character_id)`: insert a new saved copy with
  `explicitly_added = 0` and its scope in one transaction. If a copy already
  exists, keep its name, text, and explicit-add flag and add only the scope.

Keep reference-list iteration and source loading in the controller. Read the
source before opening the journal write transaction; do not hold a journal
write transaction while calling the configuration-store callback.

At `SessionRepository::MaintenanceGuard::retarget`, extend the existing
Welcome folder cleanup when `path_changed` is true. Rename the journal's
`clear_folder_associations()` operation to `clear_vault_context()` and perform
the existing folder cleanup and `DELETE FROM session_notes WHERE session_key
= ?` in one transaction. The foreign key deletes the note scopes. Remove the
old helper once its caller is updated. Keep `session_files` and transcript
rows. This deliberately changes current Welcome behavior for explicit note
copies; keeping them would allow an old-vault copy to hide a new-vault note
with the same ID. No per-note vault identifier is needed.

Add a target-aware overload such as `notes_prompt(character_id)` for generation.
Its query implements the selection and source-independent note ordering above.
Keep `notes_prompt()` for current callers, returning only explicitly added notes
and existing text-file copies. In particular, automatic session naming must
not receive unrelated Characters' saved reference notes.

Build each target's final prompt in a request-owned `CharacterDefinition`, as
today. Materialize all selected scopes before building multicast prompts so
every target uses the same saved note version. Do not append target-specific
notes to a shared definition or mutate a workspace snapshot. Once prepared,
the provider request owns fixed prompt text for its execution and tool
continuations.

Keep auxiliary model operations unchanged, including recipient classification,
note compression, and summary generation. Keep the existing settings system
prompt preview as a base-configuration preview: it has no stored chat from
which to select first-use copies and must not create one. Session-specific
saved notes are added only on the conversation request path.

Folder references must be ignored by this feature. Do not expand folders,
contact cloud stores, or alter existing chat folder attachments, retrieval
tools, summary catalogues, or uploaded text-file handling. No provider-specific
request or transport changes are needed.

## 9. Schema integration and delivery

Extend `create_note_items_schema` to create the new column on fresh databases
and add it to existing databases after checking `PRAGMA table_info`. Create
`session_note_scopes` with the existing schema helper. Follow the existing
additive-schema convention without a database version bump.

The normal vault-open path already invokes `create_note_items_schema`, and
fresh session databases use it through `create_workspace_session_schema`.
`SessionRepository` creates Welcome's temporary database through that same
fresh-schema path. Welcome's database lasts for the application run and can
survive several vault switches. Preserve that lifetime and add the path-change
cleanup in section 8; no separate Welcome migration or startup reset is needed.
Do not execute schema edits on every request.

Implement in these existing files:

| Files | Change |
| --- | --- |
| `src/storage/note_storage.cpp` | Add the flag and scope table through existing schema initialization. |
| `src/storage/session_database.{h,cpp}` | Add copy/scope operations and target-aware prompt query; mark explicit additions; preserve no-target formatting semantics; clear saved context. |
| `src/storage/session_repository.cpp` | Clear Welcome note copies with folder links when the vault path changes. |
| `src/session/session_controller.{h,cpp}` | Materialize Forum and target Character references, collect omission notices without replacing folder notices, and build each target's prompt. |
| `tests/storage/unit_session_repository.cpp` | Extend retarget coverage for explicit and automatic note cleanup, scope cascades, and same-path retention. |
| `tests/session/unit_session_controller.cpp`, `tests/session/unit_session_open.cpp` | Cover reference materialization, provider input, Character isolation, and Welcome requests after switching vaults. |
| `tests/runtime/unit_text_input.cpp` | Preserve explicit note-add, refresh, and clear behavior through chat input. |
| `tests/storage/unit_note_storage.cpp` | Cover the additive schema and existing-row defaults. |

No configuration format, bridge API, DTO, or web client changes are needed.
The existing chat notice area displays the approved omission text.

## 10. Validation

Use real temporary SQLite databases and existing fake-provider request capture.
Extend focused tests rather than adding a separate test framework.

- **Compression:** original-only and compressed notes; compression added after
  first use leaves the copy unchanged; explicit re-add selects current text.
- **First use:** no copy at settings-save time; only targeted Characters gain
  scopes; later requests discover newly registered references. Existing and
  newly inserted scopes both make `add_note_scope` return true; a missing
  copy returns false without inserting a scope or reading saved text.
- **Persistence:** changes and deletion of sources or settings do not remove
  saved context; reopen preserves it; another chat starts independently.
- **Scope and precedence:** Forum notes reach all targets; Character-only notes
  reach only that Character; overlap produces one note per prompt in the
  fixed note-ID order; explicit addition promotes and refreshes the shared row
  without moving it.
- **Order stability:** adding Character scope to a Forum note, or explicitly
  adding an already eligible note with unchanged name and text, leaves that
  target's note context identical. Verify this after reopening the chat too.
  Newly discovered notes still appear in note-ID order.
- **Vault-local IDs:** configuration copied into another vault resolves to
  that vault's note, even if its captured name differs; warn on an observed
  name mismatch without rejecting a valid rename. Missing and wrong-kind
  destination items follow omission behavior.
- **Welcome vault switch:** vaults A and B contain different note text with
  the same numeric ID. Acquire A's automatic copy and scopes in Welcome,
  switch to B, then capture a Welcome provider request. It must contain B's
  text and no retained A note context. Repeat with an explicit A copy and
  with an A note whose ID is absent from B. Verify scope deletion, transcript
  and text-file retention, and copy retention when the path does not change.
- **Explicit promotion:** refresh a Guide-only note by explicitly adding it;
  verify that another Character now receives it and its note-ID order is
  unchanged. Do not introduce a hidden scoped-refresh behavior.
- **Multicast:** shared note text is consistent across targets and Character
  context does not leak between requests. Prepared prompts remain unchanged
  if settings or source notes change while providers run.
- **Input and failures:** missing and unreadable sources, wrong-kind IDs,
  oversized selected text, empty notes, summary and annotation notes, and
  identical names with distinct IDs. Source failures log, skip, and produce
  the approved notice; several note omissions and a simultaneous folder
  omission all remain visible through generation completion. A later retry
  can succeed after the source is corrected. A failed copy/scope transaction
  leaves neither partial state nor a dispatched request.
- **Lifecycle and compatibility:** existing rows migrate as explicit notes;
  schema initialization is repeatable; vault and Welcome databases work;
  clearing and deleting sessions remove scopes; existing explicit note, text
  file, naming, maintenance, and folder behavior remains intact except for
  the specified Welcome note cleanup on a vault path change.

Build with the repository's existing CMake preset and run the affected storage,
session, and runtime tests. In particular, preserve
`TextInput.CopiesAndRefreshesANoteWithoutGeneratingAReply` and
`TextInput.ClearRemovesTheChatsNoteCopies` in
`tests/runtime/unit_text_input.cpp`, and
`SessionController.SendsSavedNoteCopiesToEveryCharacterAndRefreshesOnlyOnRequest`
in `tests/session/unit_session_controller.cpp`. Extend
`SessionRepositoryTest.WelcomeFolderRefsFollowVaultPathButKeepTranscriptAndCopies`
for the new Welcome cleanup rule; its existing copied-text fixture is a text
file and must still survive retargeting. Run the normal C++ suite after those
checks pass. This documentation-only change does not require application tests.
