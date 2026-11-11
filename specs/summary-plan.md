# Note summaries: implementation plan

Date: November 11, 2026

Status: Implementation plan. No implementation was done as part of this
plan.

Source: [Note summaries: technical design](summary-design.md). Section
numbers in this plan refer to that design, unless the text says "plan
section". The requirements are in [summary-prd.md](summary-prd.md).

## 1. Scope and completion criteria

Implement the design for the desktop application on macOS and Windows and
for `cha-daemon` with ChaWeb. The feature adds L1 and L2 versions of a note,
a Summary note for a folder (L3), an exclusion flag, four catalog prompts,
and one blocking operation that makes all levels.

Use the user's decisions of November 11, 2026, as the design records them:

- Keep the `compressed` column. Add a `level` column. Do not rebuild
  `note_items`.
- Folder export/import and D1 sync keep the Summary note link.
- CHA creates and replaces the Summary note as a read-only note.
- Remove **Show compressed** from the note menu. Keep **Compressed** in the
  editor mode menu (design section 8.3, task T7.5).

The design's "Choices not in the PRD" table lists rules that the design
author chose. Implement them as written unless the user changes them.

The feature is complete when:

- all tasks of plan sections 4 to 12 are done;
- the checks of each phase and of plan section 12 pass;
- the design's validation table (section 14) is covered by tests;
- the user approved the UI before Phase 3 started.

### UI approval gate

[AGENTS.md](../AGENTS.md) requires the user's approval of each visible
change before implementation. A request to implement this plan does not
approve the UI. Before plan section 6 (Phase 3), present these parts to the
user and wait for an explicit approval:

- design section 11: menus, editor mode menu, progress dialog, tree dot
  label, Settings prompt names, and the refusal messages;
- design section 10: the Events texts, because they are visible in
  Settings → Events.

Phases 1, 2, and 8 have no visible change. They can start before the
approval.
If the user changes a text or a control, update the design document first,
then implement.

### General rules

- Do not change documents in `docs/`.
- Do not change the compression job, the session summary worker, or other
  existing behavior, except where a task below says so.
- Add no state, no setting, and no machinery that the design does not
  specify.
- Commit only when the user asks.

## 2. Code organization

The feature adds no new C++ unit. It adds four prompt resources and one
webapp module.

| New file | Content |
| --- | --- |
| `resources/summary-l1-prompt.md` | Exact copy of `resources/compression-prompt.md`. |
| `resources/summary-l2-prompt.md` | Design section 15.1. |
| `resources/summary-l3-section-prompt.md` | Design section 15.2. |
| `resources/summary-l3-overview-prompt.md` | Design section 15.3. |
| `webapp/src/notes/summaries.ts` | `summarizeAndWait()`: plan, loop, Summary note assembly. |
| `webapp/src/notes/summaries.test.ts` | Tests of the loop with a fake client. |
| `tests/native/macos/test-note-summaries.py` | Native summary smoke test: vault setup, local provider, SQL checks (T9.1). |
| `tests/native/note-summaries.js` | Steps of the native summary smoke test in the host (T9.1). |

Existing files that change:

| Area | Files |
| --- | --- |
| Note storage | [note_storage.h](../src/storage/note_storage.h), [note_storage.cpp](../src/storage/note_storage.cpp) |
| Outgoing sync fields | [sync_storage.cpp](../src/storage/sync_storage.cpp) |
| Store wrappers | [workspace_config_store.h](../src/workspace/workspace_config_store.h), [workspace_config_store.cpp](../src/workspace/workspace_config_store.cpp) |
| Prompt catalog | [prompt_catalog.h](../src/workspace/prompt_catalog.h), [prompt_catalog.cpp](../src/workspace/prompt_catalog.cpp), [CMakeLists.txt](../CMakeLists.txt) |
| Notes service | [notes_service.h](../src/notes/notes_service.h), [notes_service_internal.h](../src/notes/notes_service_internal.h), [notes_operations.cpp](../src/notes/notes_operations.cpp), [notes_types.h](../src/notes/notes_types.h) |
| Application | [application.h](../src/app/application.h), [app/notes_operations.cpp](../src/app/notes_operations.cpp) |
| Bridge | [bridge_protocol.h](../src/bridge/bridge_protocol.h), [bridge_protocol.cpp](../src/bridge/bridge_protocol.cpp), [bridge_router.cpp](../src/bridge/bridge_router.cpp), [notes_dispatch.cpp](../src/bridge/notes_dispatch.cpp) |
| API contract | [dto.yaml](../resources/dto.yaml), generated [schema.d.ts](../webapp/src/api/schema.d.ts) |
| Webapp API | [client.ts](../webapp/src/api/client.ts), [nativeClient.ts](../webapp/src/api/nativeClient.ts), [noteGuards.ts](../webapp/src/api/noteGuards.ts), [guards.ts](../webapp/src/api/guards.ts), [fixtures.ts](../webapp/src/test/fixtures.ts) |
| Webapp notes | [summaryImport.ts](../webapp/src/notes/summaryImport.ts), [tree.ts](../webapp/src/notes/tree.ts), [useNoteImports.ts](../webapp/src/notes/useNoteImports.ts) |
| Webapp screens | [App.tsx](../webapp/src/components/App.tsx), [NoteCompressionDialog.tsx](../webapp/src/components/NoteCompressionDialog.tsx), [NotesTree.tsx](../webapp/src/components/NotesTree.tsx), [NotesScreen.tsx](../webapp/src/components/NotesScreen.tsx), [view.ts](../webapp/src/state/view.ts) |
| Wire fixtures | [notes-contract.json](../tests/fixtures/wire/notes-contract.json), [native-method-policies.json](../tests/fixtures/wire/native-method-policies.json), [prompt-contract.json](../tests/fixtures/wire/prompt-contract.json) |
| Native compression smoke test | [note-compression.js](../tests/native/note-compression.js): only its **Show compressed** step changes (T7.5) |

## 3. Phases and order

Each phase ends with its own checks. Do the checks before the dependent
phase starts. The complete regression run is in Phase 9.

The phase numbers do not change, so the task IDs stay the same. Run the
phases in the order of the first column:

| Order | Phase | Plan section | Work | Depends on | Visible change |
| --- | --- | --- | --- | --- | --- |
| 1 | 1 | 4 | Storage schema, upgrade, and level-aware queries | — | No |
| 2 | 2 | 5 | Storage operations and outgoing sync fields | 1 | No |
| 3 | 8 | 11 | D1 receiver and folder export/import | 2 | No |
| — | — | 1 | UI approval | — | — |
| 4 | 3 | 6 | Prompt catalog | UI approval | Yes: Settings → Prompts |
| 5 | 4 | 7 | Notes service operations, with T5.1 to T5.3 and the method-policy fixture | 2, 3 | Yes: Events texts, refusal messages |
| 6 | 5 | 8 | API contract: T5.4 and the rest of T5.5 | 4 | No |
| 7 | 6 | 9 | Webapp API, conversion, and operation loop | 5 | No |
| 8 | 7 | 10 | Webapp screens | 6, UI approval | Yes |
| 9 | 9 | 12 | Native smoke test and regression run | all | — |

Two rules give this order:

- Phase 8 runs directly after Phase 2, because it depends only on Phase 2.
  Phase 7 lets the user make L1 and L2 and set the exclusion flag. These
  send `note.level` and `note.summary_excluded` records. When Phase 8 comes
  first, no build can send these records to a machine that cannot apply
  them.
- T5.1 to T5.3 and the `native-method-policies.json` item of T5.5 are done
  in the same step as Phase 4. The existing tests of compression and
  session summaries call the backend through the bridge router
  (`BridgeRouterTest`). The Phase 4 tests use the same fixture, so they
  need the bridge methods. Phase 5 then does T5.4 and the rest of T5.5.

## 4. Phase 1: storage schema and queries

Purpose: add the three columns and the new indexes, upgrade existing vaults,
and make every query that reads a version row choose the correct level.
After this phase, the existing behavior is unchanged.

### T1.1 Column definitions

File: `note_storage.cpp`, `create_note_items_table()`.

- Add the three columns of design section 2.1 with their CHECK constraints:
  `level`, `summary_excluded`, and `folder_summary`.
- Keep one definition string for each new column. Use the same string in
  `CREATE TABLE` and in the `ALTER TABLE` of T1.2, so the two cannot differ.
- Add a comment at the table: `compressed = 1` marks every version row;
  `level` tells which version (0 compressed, 1 L1, 2 L2).

### T1.2 Upgrade of existing vaults

File: `note_storage.cpp`, `create_note_items_schema()`.

- After the existing `forum_id` step, add each missing column with
  `ALTER TABLE note_items ADD COLUMN <definition>`. Use
  `note_items_has_column()` for the test, as for `forum_id`.
- Call `create_note_items_indexes()` after the columns exist.
- Change no rows. Do not change the database version.

### T1.3 Indexes

File: `note_storage.cpp`, `create_note_items_indexes()`.

- Add `DROP INDEX IF EXISTS note_items_compressed_parent` and
  `DROP INDEX IF EXISTS note_items_sibling_name`.
- Create `note_items_version_parent`, `note_items_original_name`, and
  `note_items_folder_summary` as design section 2.2 shows.
- `rebuild_compressed_note_items()` calls this function after its rename.
  Make sure that the legacy `compressed_text` conversion still passes: its
  new table has the new columns from T1.1.

### T1.4 Name and ANNOTATIONS queries

File: `note_storage.cpp`.

- `name_taken()`: add `AND compressed = 0` to its `WHERE` clause.
- `rebuild_folder_annotations()`: add `AND compressed = 0` to the source
  query (`... IFNULL(parent_id, 0) = ?1 AND kind = 'note' ...`).
- The results do not change. The condition lets SQLite use
  `note_items_original_name` (design section 2.2).

### T1.5 Note item fields

Files: `note_storage.h`, `note_storage.cpp`.

- Add `has_l1`, `has_l2`, `summary_excluded`, and `folder_summary` to
  `NoteItem`. Do not add them to `to_json(NoteItem)` yet. T5.4 adds them
  together with the DTO, the wire fixtures, and the webapp guard, so the
  contract tests stay green between the phases.
- In `find_item()` and `note_list()`, replace the `LEFT JOIN note_items c`
  for the compressed row with three `EXISTS` expressions:
  `EXISTS (SELECT 1 FROM note_items v WHERE v.parent_id = n.id AND
  v.compressed = 1 AND v.level = <0|1|2>)`.
- Select `n.summary_excluded` and `n.folder_summary`.
- `is_summary` becomes `s.note_id IS NOT NULL OR n.folder_summary = 1`.
  `summary_linked` does not change.
- Extend `read_item()` for the new column positions.

### T1.6 Text modes and version rows

Files: `note_storage.h`, `note_storage.cpp`.

- Extend `NoteTextMode` with `l1` and `l2`. Add one small function that
  maps a mode to its level: `compressed` 0, `l1` 1, `l2` 2.
- `note_get()`: change the join to `c.parent_id = n.id AND c.compressed = 1
  AND c.level = ?3 AND n.kind = 'note' AND ?2`. Bind the level of the mode.
  The fallback to the full text and the response do not change.
- `note_get_text()`: add `c.level = 0` to the join.
- Rename `write_compressed_note()` to `write_version_row(database, id,
  level, blob)`. Use the level in its `UPDATE` and `INSERT`. Change all
  callers to level 0: `note_save_compressed()`, `note_commit_compressed()`,
  `note_import_folder()`, and `apply_compressed_content()`.
- Add `delete_version_row(database, id, level)`. Use it in
  `apply_compressed_content()` for a null field, with level 0.
- Keep `delete_compressed_note()` for `note_commit_summary()`. It deletes
  all version rows of the note. Add a comment that says this.
- `stored_note_fields()` (`note_storage.cpp`) and `note_fields()`
  (`sync_storage.cpp`): add `c.level = 0` to the compressed join now. Without
  it, a note with L1 or L2 returns more than one row. The new fields follow
  in T2.4.
- `note_save()`, `note_save_scroll_position()`, and `document_owner()` do
  not change.

The store and the service already pass the mode to `note_get()`. The bridge
accepts the new modes in T5.3.

### Checks for Phase 1

Add tests to [unit_note_storage.cpp](../tests/storage/unit_note_storage.cpp)
and [unit_workspace_session_database.cpp](../tests/storage/unit_workspace_session_database.cpp):

- Upgrade: build a temporary database with the current `note_items` layout
  and the old indexes. Add folders, notes, a compressed row, and an
  ANNOTATIONS row. Run `create_note_items_schema()`. The three columns and
  three new indexes exist; the two old indexes do not; all rows, IDs, and
  contents are the same. A second run changes nothing.
- A database with `compressed_text` still converts, and gets the new columns
  and indexes.
- Constraints: an L1 and an L2 row beside the compressed row, all with the
  note's name, are accepted. A second L1 row, a level on an original row, a
  second Summary note in a folder, a Summary note at the vault root, and a
  duplicate ordinary name in a folder are rejected.
- `note_list()` returns one row for a note with all three version rows.
  `has_compressed`, `has_l1`, and `has_l2` are independent.
- `note_get()`: each mode with and without its version row (design section
  2.6 table). `document_id` is correct.
- `note_save()` on an L1 row: the read-only rule of the original applies;
  the `doc` root is required; ANNOTATIONS is not rebuilt; other versions do
  not change; the original note gets the changed flag when sync is connected.
- Scroll positions are separate for each row.
- Delete of a note and of a folder deletes all version rows.
- `note_get_text(id, true)` returns the compressed version, never L1 or L2.
- `is_summary` is true for a row with `folder_summary = 1`.

Manual check: run the upgrade on a read-only copy of a current local vault,
as was done in the design review. Run `PRAGMA integrity_check`. Run
`EXPLAIN QUERY PLAN` for the `name_taken()` statement and the ANNOTATIONS
source statement. Both must show a search with `note_items_original_name`,
not a scan. Do this also with a protected vault fixture.

Exit condition: all existing C++ and webapp tests pass. The note JSON and
the API do not change in this phase.

## 5. Phase 2: storage operations and outgoing sync fields

Purpose: add the three write operations of design section 2.7 and the
outgoing D1 data of design section 9.1.

### T2.1 `note_commit_level()`

Files: `note_storage.h`, `note_storage.cpp`.

1. In one transaction, read the item with `find_item()`. Require
   `kind = note` and `is_summary = false`. Otherwise fail with
   `NoteFailure::invalid` and "That item is not a note.".
2. Require a `doc` root. Encode with `compact_document()` (12 MiB limit) and
   `compress_document()`.
3. Call `write_version_row()` with level 1 or 2. Do not apply the read-only
   rule (design section 2.7).
4. Append the success message with `event_append_in_transaction()`.
5. If `sync_connected()`, queue `note.level` with `sync_record_note()`:
   fields `level` (`"l1"` or `"l2"`) and `content` (base64 of the blob).
6. Map SQL errors with `sql_failure()`, as the other write functions do.

### T2.2 `note_commit_folder_summary()`

Files: `note_storage.h`, `note_storage.cpp`.

1. Validate before the transaction: `doc` root, `content` array,
   `summary_text()` readable text, `compact_document()` limit.
2. In one transaction, require a folder row (`kind = 'folder'`,
   `compressed = 0`) with `forum_id IS NULL`. Otherwise fail with
   "That item is not a folder." or the existing managed-folder message.
3. Find `SELECT id FROM note_items WHERE parent_id = ? AND folder_summary = 1`.
4. Replace an existing note in the same way as `note_commit_summary()`:
   `abandon_note_audio(..., "summary replaced.")`, delete its audio rows,
   `delete_compressed_note()` (all version rows), then `UPDATE` content,
   `read_only = 1`, and `scroll_top = 0`.
5. Otherwise insert a note with `read_only = 1` and `folder_summary = 1`.
   Add a small helper `folder_summary_name(database, folder_id)`: try
   "Summary", then "Summary (2)", "Summary (3)", and so on, with
   `name_taken()` and `require_name()`.
6. Call `rebuild_folder_annotations(database, folder_id, false)`.
7. Append the success event: `Summary ready for “<folder>” (folder <id>,
   note <id>).`
8. Sync: for a new note call `sync_record_note_create()`; for a replaced
   note call `sync_flag_note()`.
9. Return the item from `find_item()`.

### T2.3 `note_set_summary_excluded()`

Files: `note_storage.h`, `note_storage.cpp`.

Copy the structure of `note_set_read_only()`: find the item, require
`kind = note`, update `summary_excluded`, and queue
`note.summary_excluded` with the field `summary_excluded` when sync is
connected. Return the updated item. Do not abandon audio.

### T2.4 Outgoing D1 fields

Files: `note_storage.cpp` (`stored_note_fields()`), `sync_storage.cpp`
(`note_fields()`).

- Add `l1_content` and `l2_content`: base64 of the level 1 and level 2 row
  content, or JSON null. Read them with two more `LEFT JOIN`s that have
  `level = 1` and `level = 2`, or with scalar subqueries.
- Add `summary_excluded` as a boolean to both.
- Add `folder_summary` as a boolean to `stored_note_fields()` only. That
  function feeds `note.create`.
- `sync_record_note_create()` then carries all new fields for new notes,
  folder imports, and new Summary notes without further change.

### T2.5 Store wrappers

Files: `workspace_config_store.h`, `workspace_config_store.cpp`.

Add `commit_note_level()`, `commit_folder_summary()`, and
`set_note_summary_excluded()`. Follow `commit_compressed_note()` and
`set_note_read_only()`: same locking, same database access, same sync
machine argument.

### Checks for Phase 2

Tests in `unit_note_storage.cpp` and [unit_sync.cpp](../tests/storage/unit_sync.cpp):

- `note_commit_level()` writes the row, the event, and the `note.level`
  record in one transaction. A forced failure leaves none of them.
- It writes for a read-only note. It refuses a session summary, the Summary
  note, a folder, and a non-`doc` document. It refuses a document over the
  limit.
- A second commit replaces the content and keeps the row ID and
  `scroll_top`.
- `note_commit_folder_summary()`: create with "Summary"; create with
  "Summary (2)" when an ordinary "Summary" exists; the ordinary note is not
  changed; replace finds the note by the flag after a rename in the
  database; the replacement is read-only, has `scroll_top = 0`, has no audio
  and no version rows; ANNOTATIONS is rebuilt (put a highlight in the old
  Summary note first); a managed folder is refused; a document without
  readable text is refused.
- Sync records: a new Summary note queues `note.create` with
  `folder_summary: true`; a replacement sets the changed flag; the flagged
  `note.update` carries `l1_content`, `l2_content`, and `summary_excluded`.
- `note_set_summary_excluded()` updates the flag and queues
  `note.summary_excluded`; it creates no record when sync is not connected.
- The character summary search (`note_summary_tool()` with `summary_search`
  and `summary_read`) and `note_short_summary_catalogue()` do not return the
  Summary note.

## 6. Phase 3: prompt catalog

Start after the UI approval (plan section 1). The display names are visible
in Settings.

### T3.1 Resource files

- Copy `resources/compression-prompt.md` to
  `resources/summary-l1-prompt.md` byte for byte.
- Create the three other files with the texts of design section 15. Keep
  the texts exactly as approved.

### T3.2 Embedding

File: `CMakeLists.txt`.

- Add four `add_custom_command` blocks with `cmake/embed_text.cmake`, as for
  `compression_prompt.cpp`. Symbols: `embedded_summary_l1_prompt`,
  `embedded_summary_l2_prompt`, `embedded_summary_l3_section_prompt`, and
  `embedded_summary_l3_overview_prompt`.
- Add the four generated `.cpp` files to the `cha_lib` sources next to the
  existing generated prompt files.

### T3.3 Catalog

Files: `prompt_catalog.h`, `prompt_catalog.cpp`.

- Change the array size from 5 to 9 in the declaration and the definition.
- Declare the four embedded functions.
- Insert the four entries after `note-compression`, with the IDs and display
  names of design section 3.1.
- No other workspace code changes. New vaults get nine rows through
  `default_prompt_config_files()`. Existing vaults with prompt rows resolve
  the four new entries to their bundled defaults (workspace.cpp, the
  fallback in the prompt load loop).

### Checks for Phase 3

- [unit_workspace_config_store.cpp](../tests/workspace/unit_workspace_config_store.cpp):
  a new vault has nine prompt rows; an existing vault with five rows shows
  nine prompts, and the four new ones have their bundled texts; a save of a
  new prompt creates its row. Review tests that count prompt rows and use
  `prompt_catalog.size()` where they mean the catalog.
- `summary-l1` resolves to text equal to the compression resource.
- Update the `list` part of [prompt-contract.json](../tests/fixtures/wire/prompt-contract.json)
  and the bridge test that compares it.
- [PromptSettings.test.tsx](../webapp/src/components/PromptSettings.test.tsx):
  update fixtures if they list the catalog.

## 7. Phase 4: notes service operations

Purpose: implement design section 4. Do not change the compression job or
the session summary worker.

In the same step, do T5.1 to T5.3 and the `native-method-policies.json`
item of T5.5 (plan section 8). The Phase 4 tests call the new methods
through the bridge router, so they need these tasks (plan section 3).

### T4.1 Types and constants

Files: `notes_service.h` (or `notes_types.h`), `notes_operations.cpp`.

- Add `enum class SummaryPart { l1, l2, l3_section, l3_overview, l3 };`.
- Use `note_compression_min_bytes` (4096) as the minimum and
  `text_file_byte_limit` (262144) as the maximum.
- Add target label helpers next to `compression_target()`:
  `“<name>” (note <id>)` and `“<name>” (folder <id>)`.

### T4.2 Call list

File: `notes_service_internal.h`.

- Add `struct SummaryCall { std::string connection_id;
  std::shared_ptr<std::atomic_bool> cancel; }` and
  `std::vector<std::shared_ptr<SummaryCall>> summary_calls;`. The lifecycle
  lock guards the list.
- Add a small helper that removes one call from the list.

### T4.3 Answer check

File: `notes_operations.cpp`.

Add `require_note_answer(answer, cancel)` in the anonymous namespace. It has
the checks of `require_summary_answer()` with neutral messages:

- cancelled → `operation_cancelled`;
- refused → "The provider refused this request.";
- not completed → the safe reasons, also "Prompt exceeds the model's context
  window.", or the generic provider message;
- finish reason other than empty, `stop`, or `end_turn` → "The provider
  returned an incomplete result.";
- empty text → "The provider returned no text.";
- more than 12 MiB → "The result exceeds 12 MiB.".

Do not change `require_summary_answer()`.

### T4.4 `summary_plan()`

File: `notes_operations.cpp`.

Under the lifecycle lock, after `require_admitted(epoch)`:

- Read the item with `store->get_note_item(id)`. Missing → `not_found`.
- Note: require `kind = note` and `!is_summary` ("That item is not a
  note."). Read `get_note_text(id)`. Under 4096 bytes or over 262144 bytes →
  `invalid_argument` with the approved messages of design section 11.6.
- Folder: require `kind = folder` ("That item is not a folder.") and no
  `forum_id` (existing managed-folder message).
- Both: call `prepare_note_provider()` with the `summary-l1` prompt from a
  workspace snapshot and discard the result.
- Folder targets: from `store->list_note_items()`, take the notes with
  `parent_id = id`, `kind = note`, `!is_summary`, and `!summary_excluded`.
  Read the text of each and keep those with at least 4096 bytes.
- Return `{ "notes": [{ "id", "name", "has_l1", "has_l2" }] }`, ordered by
  ID. Write no event.

### T4.5 `generate_summary_part()`

File: `notes_operations.cpp`.

Synchronous part, under the lifecycle lock:

1. `require_admitted(epoch)`. Context errors write no event.
2. Read the item and check it (design section 4.3, step 1). For `l3_overview`
   use the request `text`; require it to be nonempty and at most 12 MiB.
3. Read the input: `get_note_text(id, false)` for `l1` and `l2`,
   `get_note_text(id, true)` for `l3_section`. Check at most 262144 bytes.
   For `l3_section`, choose the reason by `has_compressed` (design section
   10).
4. Prepare the definition with `prepare_note_provider()` and the prompt of
   the part.
5. Create a `SummaryCall` with the connection ID and add it to the list.
6. Launch the worker with `background_jobs.launch(work, call->cancel)`.
   If the launch fails, remove the call and fail with `command_queue_full`.
7. If steps 2 to 4 fail, append the error event of design section 10 and
   rethrow.

Worker:

1. Retry loop as in the compression worker: up to three retries of a
   retryable result, with waits of 10, 20, and 40 seconds. Use
   `settings.test_hooks.note_compression_wait` when it is set. Before each
   attempt, under the lifecycle lock: stop if the cancel flag is set, call
   `require_admitted(epoch)`, and create the `ProviderClient`.
2. Call `collect_note_prompt()`. Check with `require_note_answer()`.
3. Under the lifecycle lock: remove the call from the list. If the epoch
   changed, write no event. If cancelled, append the info event; if failed,
   append the error event.
4. Outside the lifecycle lock: complete or fail the reply. Delivery
   callbacks must never run with the lifecycle lock held (see the session
   summary worker).

### T4.6 `commit_summary_part()`

- `l1`, `l2`: build the success message (`L1 ready for “…” (note …).`) and
  call `store->commit_note_level()`.
- `l3`: call `store->commit_folder_summary()`. Then call
  `interrupt_abandoned_audio()` and the audio notice, as
  `commit_session_summary()` does, each in its own `try`.
- Other parts: `invalid_argument`.
- On `NoteError`: append the error event (`Could not save …`) and call
  `rethrow_note()`.

### T4.7 `abandon_summary_part()`

Accept `l1`, `l2`, and `l3`. Read the item name. Append the error event with
the reason "the result could not be converted to a note". For `l3`, add
"The Summary note was not changed.". Change no data.

### T4.8 `cancel_summary_parts()` and `cancel_summaries_locked()`

- `cancel_summary_parts(connection_id, epoch)`: `require_admitted(epoch)`,
  then set the flag of every call of that connection. Write no event here.
- Extend `cancel_summaries_locked(connection_id)`: also set the flags of the
  matching calls, or of all calls when it has no argument. The existing
  calls at connection close, vault maintenance, and shutdown then cover the
  new calls.

### T4.9 `set_note_summary_excluded()`

Call `store->set_note_summary_excluded()` and return the item. Map
`NoteError` with `rethrow_note()`.

### T4.10 Compression of the Summary note

No code change: `compress_note()` checks `is_summary`, which now covers the
Summary note. Add a test.

### Checks for Phase 4

Put the tests in [unit_bridge_router.cpp](../tests/bridge/unit_bridge_router.cpp),
in the fixture `BridgeRouterTest`, next to the session summary tests. Do
not use `unit_notes_service.cpp`: it has no provider test setup. Name each
new test `BridgeRouterTest.NoteSummary<Topic>`, so one filter selects them.

Reuse the existing setup. These tests show it:

- `SummaryUsesSavedSourceAndNotesProviderAndReplacesOneReadOnlyNote`: the
  `call()` helper with a background reply, `MockHttpServer`,
  `compression_response()`, `configure_compression()`, and
  `update_prompt()`;
- `SummaryConnectionCloseCancelsProviderAndRetiresExactlyOnce`: a held
  provider request and its cancellation;
- `CompressionExhaustsThreeRetriesThenLetsTheNextNoteRun`: the retry wait
  hook;
- `FolderCompressionSkipsStoredAndShortNotesBeforeProviderValidation`:
  folder compression with skipped notes.

Check these service rules:

- Plan: note size at 4095, 4096, 262144, and 262145 bytes; summary notes and
  folders refused for a note plan; managed folder refused; missing provider
  gives the compression message; folder targets exclude excluded, summary,
  and small notes, include notes over 256 KiB, and do not include notes of
  subfolders; no event is written.
- Generate: each part sends its own prompt as the system prompt and the
  correct user message; `l3_section` uses the compressed text when it
  exists; a note over 256 KiB fails with the event text of design section
  10 in both section cases; retries use the wait hook; a cancelled call
  stops the provider request and writes one info event; a vault change
  writes no event; the call list is empty after each outcome.
- Up to three calls of one connection run at the same time. A compression
  job can run at the same time.
- Commit and abandon write the expected events. A storage error writes an
  error event and fails the request.
- `cancel_summaries_locked()` with and without a connection ID cancels the
  matching calls only.
- Folder compression skips the Summary note. Compression of a note keeps
  its L1 and L2.

Check these bridge rules of T5.1 to T5.3:

- exact keys of the six methods, each `part` rule, and the `text` rule;
- the epoch policy and the background reply of `notes.summaryGenerate`;
- `notes.summaryCancel` cancels only the calls of its own connection; a
  connection close cancels the calls;
- `notes.get` with `l1` and `l2`;
- [unit_bridge_protocol.cpp](../tests/bridge/unit_bridge_protocol.cpp):
  the names and the policies of the six methods;
- `npm --prefix webapp run check` passes with the new policy entries. Its
  policy tests are generic and need no webapp change.

## 8. Phase 5: application and bridge contract

T5.1 to T5.3 and the `native-method-policies.json` item of T5.5 are done in
the Phase 4 step (plan section 3). They stay in this section, so the whole
bridge contract is in one place. The Phase 5 step does T5.4 and the other
items of T5.5.

### T5.1 Application wrappers

Files: `application.h`, `app/notes_operations.cpp`.

Add one wrapper for each new `NotesService` method, as `compress_note()`
does.

### T5.2 Method names

Files: `bridge_protocol.h`, `bridge_protocol.cpp`.

Add six `Method` values and their names: `notes.summaryPlan`,
`notes.summaryGenerate`, `notes.summaryCommit`, `notes.summaryAbandon`,
`notes.summaryCancel`, and `notes.setSummaryExcluded`. All require the
context epoch. None is a control method.

### T5.3 Dispatch

- `notes_dispatch.cpp` (no connection ID needed):
  - `notes.summaryPlan`: keys `{id}`.
  - `notes.summaryCommit`: keys `{id, part, document}`; `part` in `l1`,
    `l2`, `l3`; `document` an object. Result `{}`.
  - `notes.summaryAbandon`: keys `{id, part}`; same parts. Result `{}`.
  - `notes.setSummaryExcluded`: keys `{id, excluded}`; boolean. Result
    `NoteItem`.
- `bridge_router.cpp` (needs the connection ID):
  - `notes.summaryGenerate`: keys `{id, part}` or `{id, part, text}`;
    `part` in `l1`, `l2`, `l3_section`, `l3_overview`; `text` only and
    always for `l3_overview`, a nonempty string. Call
    `start_background(connection_id, id, reply, true)`.
  - `notes.summaryCancel`: keys `{}`. Result `{}`.
  - Add `Method::notes_summary_generate` to `deadline_exempt()`.
- `notes.get` in `notes_dispatch.cpp`: accept the mode values `l1` and `l2`
  next to `full_text` and `compressed`. Reject other values as today.
- Use `require_only_keys()`, `require_safe_id()`, and `require_string()`.
  An unknown `part` is `invalid_argument`.

### T5.4 DTOs and generated types

Files: `dto.yaml`, `note_storage.cpp`.

- `to_json(NoteItem)`: add `has_l1`, `has_l2`, `summary_excluded`, and
  `folder_summary`.
- `NoteItem`: add the four required boolean fields.
- `NoteTextMode`: add `l1` and `l2`.
- Add `SummaryPlan`, `SummaryTarget`, `SummaryGenerateRequest`,
  `SummaryGenerated`, `SummaryCommitRequest`, `SummaryAbandonRequest`, and
  `NoteSummaryExcludedRequest`, with `additionalProperties: false`.
- Run `npm --prefix webapp run api-types`. Do not edit `schema.d.ts` by
  hand.

### T5.5 Wire fixtures

- `native-method-policies.json`: add the six methods. This item is done in
  the Phase 4 step.
- `notes-contract.json`: add the new `NoteItem` fields to every item, a
  Summary note item, a `SummaryPlan` example, and a `SummaryGenerated`
  example.
- Update [unit_wire_fixtures.cpp](../tests/runtime/unit_wire_fixtures.cpp)
  and [wireFixtures.test.ts](../webapp/src/api/wireFixtures.test.ts) for the
  new examples.
- `isNoteItem()` in `noteGuards.ts`: add the four keys to `hasExactKeys()`,
  check their types, and add `(!value.folder_summary || value.is_summary)`.
  ChaWeb uses the same guard. Update the note items in
  [useChawebNotes.test.ts](../webapp/src/chaweb/useChawebNotes.test.ts) and
  other webapp test fixtures with the new fields.

### Checks for Phase 5

The router and protocol checks of T5.1 to T5.3 are in "Checks for Phase 4".

- [unit_bridge_router.cpp](../tests/bridge/unit_bridge_router.cpp): the
  tests that compare `NoteItem` JSON with `notes-contract.json` pass with
  the new fields.
- [unit_wire_fixtures.cpp](../tests/runtime/unit_wire_fixtures.cpp) and
  [wireFixtures.test.ts](../webapp/src/api/wireFixtures.test.ts): the new
  examples.
- [unit_chaweb_adapter.cpp](../tests/daemon/unit_chaweb_adapter.cpp): the
  notes list JSON has the new fields; a Summary note is listed and opens.
- `npm --prefix webapp run check` passes.

## 9. Phase 6: webapp API, conversion, and operation loop

This phase has no visible change. Its code is not reachable until Phase 7.

### T6.1 Client methods and guards

Files: `client.ts`, `nativeClient.ts`, `guards.ts`,
`webapp/src/test/fixtures.ts`.

- Add the six methods of design section 6.1 to `NotesClient` and the native
  implementation. `summaryGenerate()` returns the `markdown` string of the
  reply.
- Add guards for `SummaryPlan` and `SummaryGenerated`.
- Update the fake clients in `fixtures.ts`, `NotesTree.test.tsx`, and
  `Notes.test.tsx` where the type requires the new methods.
- Tests in [nativeClient.test.ts](../webapp/src/api/nativeClient.test.ts):
  method names, parameters, and response guards of the six methods.

### T6.2 Markdown conversion

File: `summaryImport.ts`.

- Add `markdownNoteNode(markdown)`: the first part of `summaryDocument()`
  (Markdown size check, `notePageHtml(..., { summary: true })`, table span
  check, `flattenClipboardTable()`, parse). It returns the ProseMirror node.
- Add `markdownNoteDocument(markdown)`: `markdownNoteNode()`, `toJSON()`,
  and the document size check. It throws on failure.
- Change `summaryDocument()` to call `markdownNoteNode()`, keep its chapter
  check on the node, and keep its conversion, size check, and error message.
- Tests in [summaryImport.test.ts](../webapp/src/notes/summaryImport.test.ts):
  existing `summaryDocument()` tests pass unchanged; lists, headings, bold,
  italic, and links convert; a table becomes one paragraph per row with
  tabs; `colspan` throws; an empty result throws; the size limits throw.

### T6.3 Operation loop

File: `webapp/src/notes/summaries.ts`.

`summarizeAndWait()` receives the client, the source item, a run object
(`requested`, `cancelJob`), a context check function, and a progress
callback. It implements design section 7.2:

- Call `summaryPlan()`. Sort the targets with `compareNoteSiblings()`.
- For each target: stop if `requested`; call the context check; report
  progress; start the calls (`makeLevel` for `l1` and `l2` as the rules say,
  `makeSection` while `makeL3` is true); wait with `Promise.allSettled()`.
- `makeLevel`: generate; return if `requested`; convert with
  `markdownNoteDocument()`; on a conversion error call `summaryAbandon()`;
  otherwise call `summaryCommit()`.
- `makeSection`: generate and keep `{ name, markdown }`; on failure set
  `makeL3` to false.
- After the loop, if `makeL3` and not `requested`: context check, progress
  "Writing the Summary note…", `summaryGenerate(folderId, 'l3_overview',
  sectionText())`, assembly of design section 6.3, and then, if not
  `requested`, a progress update that disables Cancel and
  `summaryCommit(folderId, 'l3', document)`. A conversion error or an
  assembly over the limit calls `summaryAbandon(folderId, 'l3')`.
- Set `run.cancelJob` to a function that calls `client.summaryCancel()`.
- Return the target IDs and whether the `l3` commit succeeded. The caller
  needs them for the reload rules.
- A rejected call never stops the loop. A context error ends it.

Build the heading nodes for note names as JSON objects, not as Markdown
(design section 6.3).

Tests in `summaries.test.ts` with a fake client that records calls and
controls their results:

- tree order; at most three calls per note; for a folder, existing L1 and
  L2 are skipped; for a note, both are made;
- a failed L1 does not stop the loop; a failed section stops further
  sections and the overview, but not L1 and L2;
- no targets → no calls;
- Cancel during calls: `summaryCancel()` is called; no later call starts; a
  result that arrives after Cancel is not committed; a commit already sent
  completes;
- conversion failure → `summaryAbandon()` with the correct part;
- `sectionText()` has the names as level-1 headings in tree order; the
  Summary document has the overview, then one heading node and the section
  content for each note; a name with `*` and `#` stays literal;
- a context error ends the loop.

## 10. Phase 7: webapp screens

Start only after the UI approval. Use the approved texts.

### T7.1 Dialog label

File: `NoteCompressionDialog.tsx`.

Add an optional `label` property. The default stays "Note compression".

### T7.2 App action

File: `App.tsx`.

Add `summarizeNotes(source, epoch, refresh)` beside `compressNote`:

- Same start as `compressNote`: context check, `beginNoteAction()`, set
  `compressionRun.current` to the run, show "Saving note…", freeze and flush
  the open editor, stop on a save failure or Cancel.
- Call `summarizeAndWait()`. Map its progress to `setCompression()` and pass
  the dialog label "Note summaries".
- Final steps of design section 7.4: refresh; reload the open note when the
  mode is `l1` or `l2` and the note was a target, when it is the folder's
  Summary note and the `l3` commit succeeded, or when `notesChanged` is set;
  after a successful `l3` commit call `sourceChanged(folderId)`; unfreeze
  only when the editor was not replaced; clear the run; close the dialog;
  `endNoteAction()`.
- A refused plan throws to the tree, which shows the message.
- The existing dialog `onCancel` handler does not change. It calls
  `run.cancelJob?.()`.

### T7.3 Tree

Files: `tree.ts`, `NotesTree.tsx`.

- `NoteTreeItem`: add `has_l1`, `has_l2`, `summary_excluded`, and
  `folder_summary`. `NoteTreeRow`: replace `hasCompressed` with
  `hasVersions`, and add `summaryExcluded` and `folderSummary`.
- The dot uses `hasVersions`. Its `aria-label` and `title` use the approved
  text.
- Rename: hide also for `folderSummary`.
- Summarize: show on notes with `!isSummary`, and on folders with no
  `forumId` that are not the vault row. Use the approved position. A click
  calls `onSummarize(source, epoch, refresh)` and shows a thrown error in the
  tree's error area, as `startCompress()` does.
- Exclude from summaries: show on notes with `!isSummary`.
  `role="menuitemcheckbox"`, `aria-checked`, and `CheckIcon` when set. A
  click calls `setNoteSummaryExcluded()` and refreshes the list.
- Add the new fields to the item key that the tree uses to find changes.
- Add the `onSummarize` property and pass it from `App.tsx`.

### T7.4 Editor modes

Files: `NotesScreen.tsx`, `view.ts`, `useNoteImports.ts`.

- `noteTextModes`: `['full_text', 'l1', 'l2', 'compressed']` with the
  approved labels.
- `loadedTextMode`: `fullTextLoaded ? 'full_text' : noteTextMode`.
- The title: do not lowercase the L1 and L2 labels.
- `view.ts`: the mode type comes from the generated schema; check the
  reducer tests for the two new values.
- `useNoteImports.ts`: `ImportTarget` gets `mode`. Capture it at the start.
  The reopen step calls `getNote(noteId, mode)` and keeps the `document_id`
  check.

### T7.5 Removal of Show compressed

Files: `NotesTree.tsx`, `App.tsx`, and the tests below.

The user ordered this removal on November 11, 2026. Design section 8.3
gives the rules and section 11.1 shows the result. It is part of the UI
approval of Phase 7, like the other menu changes.

- `NotesTree.tsx`: remove the **Show compressed** `<button>` from the note
  menu and remove the `showCompressed()` function. The item goes away for
  ordinary notes and for the ANNOTATIONS item.
- `NotesTree.tsx`: remove the `mode` argument from the `onOpenNote`
  property type and from its calls. `App.tsx`: remove the `mode` argument
  from `requestOpenNote()` and stop passing it to the `open-note` action.
- Do not change the `mode` field of the `open-note` action in `view.ts`.
  The ANNOTATIONS link in `NotesScreen.tsx` uses it.
- Do not remove `has_compressed` from `NoteItem`, the DTO, or the guards.
  `hasVersions` reads it.
- Do not change the editor mode menu. **Compressed** stays (T7.4).
- Nothing remains of the old plain-text window for the compressed version.
  The last commit removed it. Do not add a replacement.

Test changes:

- [NotesTree.test.tsx](../webapp/src/components/NotesTree.test.tsx):
  - "disables Show compressed without stored text and skips it with the
    arrow keys": change it to check that **Show compressed** is absent for a
    note with and without a compressed version. Keep the arrow-key part with
    the new neighbors of **Compress**.
  - "opens the logical note in compressed mode": delete it. The function no
    longer exists.
  - Test helpers that check `onOpenNote` calls: expect calls without a
    `mode` argument.
- [Notes.test.tsx](../webapp/src/components/Notes.test.tsx):
  - The three tests that open the compressed version with **Show
    compressed** ("keeps fallback edits on the original row even when
    compression appears before the save", "finishes a file import into the
    compressed row after switching to full text", "requires delete
    confirmation based on the original even with a short compressed document
    open"): select **Compressed** in the editor mode menu instead. Keep what
    each test checks.
  - "opens annotation sources, protects generated content and reports stale
    entries": the ANNOTATIONS menu no longer has **Show compressed**. Check
    that it is absent instead of disabled.
- [note-compression.js](../tests/native/note-compression.js): the step
  "shortcut reopened the compressed editor" uses **Show compressed**.
  Replace it: select **Full text** and then **Compressed** in the mode menu,
  and keep the same checks of the compressed editor.

### Checks for Phase 7

- [NotesTree.test.tsx](../webapp/src/components/NotesTree.test.tsx) and
  [tree.test.tsx](../webapp/src/notes/tree.test.tsx): menu items for an
  ordinary note, a Summary note, a session summary, a folder, a managed
  folder, and the vault row; the check mark; the dot for each version; the
  dot label; a refused plan message.
- [Notes.test.tsx](../webapp/src/components/Notes.test.tsx) and App tests:
  the dialog texts and label; Cancel and Escape; blocked navigation and
  mode change during the operation; the reload rules; ANNOTATIONS reopens
  after the `l3` commit.
- [NotesScreen.test.tsx](../webapp/src/components/NotesScreen.test.tsx):
  four menu items, the check mark, the fallback title, audio disabled on L1
  and L2, read-only on all versions.
- [view.test.ts](../webapp/src/state/view.test.ts) and
  [useNoteImports.test.ts](../webapp/src/notes/useNoteImports.test.ts): the
  new modes; an import into L1 that finishes after a mode change targets L1.
- T7.5: no menu has **Show compressed**; the changed tests of T7.5 pass; the
  compressed version still opens and saves through the mode menu.

## 11. Phase 8: D1 receiver and folder export/import

Run this phase directly after Phase 2, before the UI approval (plan
section 3).

### T8.1 Incoming note fields

File: `note_storage.cpp`.

- Generalize `apply_compressed_content()` with a field name and a level:
  a string is base64-decoded, checked with `parse_compressed_note()`, and
  written with `write_version_row()`; null or a missing field calls
  `delete_version_row()`. Keep the legacy `compressed_text` branch only for
  level 0.
- `apply_insert_note()` and `apply_note_update()`: apply `l1_content` and
  `l2_content`; set `summary_excluded` from the record, false when missing.
- `apply_insert_note()` for `note.create`: set `folder_summary` from the
  record. Do not check for an existing Summary note. The constraints reject
  an invalid record, and the existing error path skips it with a warning.

### T8.2 New record kinds

File: `note_storage.cpp`, `note_apply_record()`.

- `note.level`: `apply_find()`, require `kind = note`, require `level` to be
  `"l1"` or `"l2"`, decode `content`, check it with `parse_compressed_note()`,
  write with `write_version_row()`. Do not touch audio.
- `note.summary_excluded`: `apply_find()`, require `kind = note`, set the
  flag from the boolean field.

### T8.3 Folder export

File: `note_storage.cpp`, `note_export_folder()`.

- Add `level`, `summary_excluded`, and `folder_summary` to
  `folder_export_schema` as `INTEGER NOT NULL`.
- Select and insert the three columns for each row.
- Update the function comment in `note_storage.h`: annotations, session
  summary links, and forum links stay out; the Summary note link goes in.

### T8.4 Folder import

File: `note_storage.cpp`, `note_import_folder()`.

- Detect the layout: the file has `level`, or has `compressed` only, or has
  `compressed_text` (design section 9.2 table).
- New layout validation: extend the existing query. A version row has
  `level` in (0, 1, 2), `kind = 'note'`, content, and an original note as
  parent. A note has at most one row for each level. A `folder_summary` row
  is an original note whose parent is a folder. A folder has at most one.
  A failure gives "This file is not a CHA folder export.".
- `copy_item()`: insert `summary_excluded` and `folder_summary` for the
  new layout; 0 for the older layouts.
- Copy the version rows of each note right after the note: read
  `level, kind, content_json` of its rows, check each with
  `parse_compressed_note()`, write with `write_version_row()`. For the
  `compressed` layout, copy the row at level 0 as today.

### Checks for Phase 8

- `unit_note_storage.cpp`: apply of `note.create` and `note.update` with and
  without the new fields; null deletes the row; `note.level` keeps the
  note's audio and its other versions; `note.summary_excluded`; a
  `note.create` with `folder_summary: true` links the note; a second one in
  the same folder is skipped with a warning.
- Folder transfer: a round trip keeps L1, L2, the compressed version, the
  exclusion flag, and the Summary link; a file with the `compressed` layout
  and a file with the `compressed_text` layout import as before; invalid
  version rows and two Summary notes in one folder are refused; with sync
  connected, the import queues `note.create` records with the new fields
  for original notes only.
- [unit_d1_sync.cpp](../tests/app/unit_d1_sync.cpp): two vaults with the
  existing test transport. L1 commit, exclusion change, Summary note
  creation, and an L1 edit reach the second vault. After `note.level`, the
  note's audio on the second vault is still present.

## 12. Phase 9: native smoke test and regression run

### T9.1 Native smoke scenario

New files: `tests/native/macos/test-note-summaries.py` and
`tests/native/note-summaries.js`.

Design section 14 says to extend the compression smoke scenario. Do not add
the summary steps to `test-note-compression.py`. That script checks an
exact list of compressed rows (`WHERE c.compressed=1`) and an exact count
of Events, and summary steps in the same vault would break these checks.
Write a new pair with the same structure instead. Copy the vault setup,
the local provider, and the helpers of the compression pair. The scenario
steps below are the steps that the design asks for. The compression pair
keeps its checks; only its **Show compressed** step changes (T7.5).

The local provider routes each request by its system prompt
(`body['messages'][0]['content']`):

- the overview prompt text → an overview answer;
- the section prompt text → a short section answer;
- the L2 prompt text → a list answer with one Markdown table;
- any other text → an L1 answer. The default L1 prompt is equal to the
  compression prompt, and this script runs no compression.

One held answer lets the test press Cancel, as the compression test does
with `SLOW_FOLDER`.

Steps in the host (`note-summaries.js`):

1. Summarize one note. Check the dialog text, then open L1 and L2 in the
   editor mode menu.
2. Summarize a folder with four notes: two ordinary notes, one excluded
   note, and one note under 4 KiB. Check "Note 1 of 2" and "Note 2 of 2",
   the Summary note in the tree, its two section headings, and the dot on
   the two targets.
3. Summarize the folder again. Check that L1 and L2 are not requested again
   and that the Summary note keeps its ID.
4. Start a folder run and cancel it during a delayed call. Check that the
   Summary note did not change and that no further provider request
   arrived.
5. Check that a chat attachment of a target note still sends the
   compressed or the original text.

Checks after the host ends (`test-note-summaries.py`, with SQL on the test
vault):

- the L1 and L2 rows have `compressed = 1` and `level` 1 or 2;
- the Summary note has `folder_summary = 1`, and its ID did not change in
  the second folder run;
- the excluded note and the note under 4 KiB have no L1 and no L2;
- the provider request counts match the steps.

Run both native pairs. Build the webapp and the native host first:

```bash
npm --prefix webapp run build
```

```bash
cmake --build --preset ninja --target cha_macos_runtime cha_prepare_test_vault
```

```bash
python3 tests/native/macos/test-note-compression.py
```

```bash
python3 tests/native/macos/test-note-summaries.py
```

### T9.2 Commands

Run, in this order:

```bash
npm --prefix webapp run api-types
```

```bash
npm --prefix webapp run check
```

```bash
make test
```

```bash
make test-ui
```

On Linux, `make test` runs the daemon test set. Run `make itest-daemon` for
the ChaWeb path.

### T9.3 Manual acceptance

Use a copy of a vault with an imported EPUB book and a configured Notes
provider:

- Summarize a chapter and read L1 and L2. Highlight in L2. The highlight
  stays after a restart. ANNOTATIONS of the folder does not show it.
- Summarize the book folder. Read the Summary note; the Content menu lists
  the chapters. Add a comment to it; ANNOTATIONS shows it.
- Make a chapter excluded and summarize the folder again; the chapter is not
  in the new Summary note.
- Delete the Summary note and summarize again; CHA creates a new one.
- Rename an ordinary note to "Summary" in another folder and summarize that
  folder; CHA creates "Summary (2)" and does not change the ordinary note.
- Export the folder, import it elsewhere, and summarize the imported folder;
  CHA replaces the imported Summary note and does not create a second one.
- Check Settings → Events for the success, failure, and cancel texts.
- Open the vault in ChaWeb; the Summary note is listed; L1 and L2 are not.

### T9.4 Delivery check

- `git status` shows only the files of plan section 2 and test files.
- No file in `docs/` changed.
- `schema.d.ts` equals the generated output.
- Report test results with their output. Commit only when the user asks.
