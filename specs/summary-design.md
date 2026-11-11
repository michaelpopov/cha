# Note summaries: technical design

Date: November 11, 2026

Status: Proposed technical design. It implements
[summary-prd.md](summary-prd.md). It does not implement the feature. The UI
sketches in section 11 are the proposed visual design. They need the user's
approval before implementation.

The user made these decisions on November 11, 2026, for this design:

- Keep the `compressed` column and add a `level` column for L1 and L2 rows.
  Do not rebuild the table.
- Folder export/import and D1 sync keep the link of the folder Summary note.
- CHA creates and replaces the Summary note as a read-only note, as it does
  for session summaries.
- Remove **Show compressed** from the note menu. The editor mode menu keeps
  **Compressed**, so the user can still open the compressed version
  (section 8.3).

## 1. Approach

The feature reuses four existing parts of CHA:

| Existing part | Use in this feature |
| --- | --- |
| Compressed version rows in `note_items` ([compressed-note-json-design.md](compressed-note-json-design.md)) | L1 and L2 are child rows of their note, as the compressed version is. Open, save, read-only, delete, export, and sync work the same way. |
| Editor text modes ([compressed-note-edit-design.md](compressed-note-edit-design.md)) | The mode menu gets two more modes, `l1` and `l2`. |
| Blocking folder compression in `App.tsx` | The summary operation uses the same modal dialog, note-action guard, save-before-start, refresh, and reload steps. The app owns the loop over notes. |
| Session summary generation | The backend makes one model call per request and returns Markdown. The webapp converts the Markdown with the session summary converter and commits the document. |

The webapp owns the loop because the Markdown conversion exists only in the
webapp: `summaryDocument()` uses `marked`, `DOMPurify`, and the Tiptap
parser. The C++ code has no Markdown parser. A backend job would need a
second converter or a callback to the webapp. Both are more machinery than a
loop in the webapp.

### Requirements and mechanisms

Each new mechanism exists for one requirement of the PRD.

| PRD requirement | Mechanism | What breaks without it |
| --- | --- | --- |
| L1 and L2 are versions of their note (5) | `level` column on version rows | No place to store two more versions per note. |
| The Summary note is identified by a link, not by name (5.2) | `folder_summary` flag column | CHA could replace an ordinary note named "Summary". |
| Exclusion flag (4.3) | `summary_excluded` flag column | No place to store the setting. |
| Included notes need 4 KiB of text (4.2) | `notes.summaryPlan` request | The webapp cannot measure note text without reading every note. |
| One model call per level, up to three at the same time (6) | `notes.summaryGenerate` request, one per call | — |
| Formatted documents (5) | Webapp conversion, then `notes.summaryCommit` | — |
| Events records each failure, also conversion failures (6.1) | `notes.summaryAbandon` request | The backend does not know that a conversion in the webapp failed. |
| Cancel stops the calls that run (6.2) | `notes.summaryCancel` and a list of cancel flags | Running provider calls continue after Cancel. |
| Four editable prompts (3) | Four catalog entries | — |
| D1 sync sends L1, L2, and the flag as it sends the compressed version (7) | `note.level` and `note.summary_excluded` records, as `note.compress` and `note.read_only` | A `note.update` record deletes the note's audio on the receiving machine. `note.compress` prevents this for the compressed version today. |

### Choices not in the PRD

The PRD and the user's decisions do not state these rules. The design
author chose them. Each one can change without an effect on the rest of the
design.

| Choice | Section | Reason |
| --- | --- | --- |
| After the first failed L3 section, the loop makes no more L3 calls. | 7.2 | The Summary note cannot be written, so more L3 calls only cost time and money. |
| A result that arrives after Cancel is not saved. | 7.3 | PRD 6.2: Cancel starts no more calls. A save after Cancel would also change data after the user stopped the work. |
| Cancel is disabled while the Summary note commit runs. | 7.3 | PRD 6.2: after Cancel, the Summary note does not change. |
| `is_summary` also covers the Summary note. | 2.5 | The existing menu and compression rules for summary notes then apply with no new code. |
| Summarize is not on chat folders or the vault row. Exclude from summaries is not on summary notes. | 11 | These items can never have an effect there. They are part of the UI approval. |
| No start event. A successful section or overview call writes no event. Each cancelled call writes an info event. | 10 | PRD 6 asks for the result of each job. Compression writes a cancel event in the same way. |
| A table cell with `rowspan` or `colspan` other than 1 fails the conversion. | 6.2 | A merged cell cannot become one plain line of text. Markdown tables have no merged cells, so only raw HTML tables are affected. |
| Markdown that converts to a document with no text fails the conversion. | 6.2 | An answer with only a rule or a picture would replace a good version with an empty one. |

## 2. Storage

### 2.1 New columns

Add three columns to `note_items`:

```sql
level INTEGER NOT NULL DEFAULT 0
    CHECK (level IN (0, 1, 2) AND (level = 0 OR compressed = 1)),
summary_excluded INTEGER NOT NULL DEFAULT 0
    CHECK (summary_excluded IN (0, 1)),
folder_summary INTEGER NOT NULL DEFAULT 0
    CHECK (folder_summary IN (0, 1)
        AND (folder_summary = 0
            OR (kind = 'note' AND compressed = 0 AND parent_id IS NOT NULL))),
```

`compressed = 1` continues to mark a version row: a child document of a
note. `level` tells which version the row holds:

| Row | `compressed` | `level` | `parent_id` |
| --- | --- | --- | --- |
| Folder, original note (L0), or ANNOTATIONS | 0 | 0 | Parent folder, or NULL at the root |
| Compressed version | 1 | 0 | The original note |
| L1 | 1 | 1 | The original note |
| L2 | 1 | 2 | The original note |

`summary_excluded` is 1 when the user excluded the note from summaries.
`folder_summary` is 1 on the Summary note of its parent folder. The flag and
the `parent_id` together are the link that the PRD requires. CHA never reads
the name to find the Summary note.

A version row copies the name of its note when CHA creates it, as the
compressed row does today. CHA does not read that name.

### 2.2 Indexes

Change two indexes and add one:

```sql
DROP INDEX IF EXISTS note_items_compressed_parent;
CREATE UNIQUE INDEX IF NOT EXISTS note_items_version_parent
    ON note_items(parent_id, level) WHERE compressed = 1;

DROP INDEX IF EXISTS note_items_sibling_name;
CREATE UNIQUE INDEX IF NOT EXISTS note_items_original_name
    ON note_items(IFNULL(parent_id, 0), name) WHERE compressed = 0;

CREATE UNIQUE INDEX IF NOT EXISTS note_items_folder_summary
    ON note_items(parent_id) WHERE folder_summary = 1;
```

- `note_items_version_parent` permits one row for each version of a note.
- `note_items_original_name` keeps the unique-name rule for tree items. The
  old index also covered version rows. With three version rows under one
  note, all with the note's name, the old index would reject the second row.
  Version rows are not siblings of tree items, so the rule does not apply to
  them.
- `note_items_folder_summary` permits one Summary note in each folder.

SQLite uses a partial index only when the query contains the index
condition. Two queries used the old name index through
`IFNULL(parent_id, 0)`: `name_taken()` and the source query of
`rebuild_folder_annotations()`. Add `compressed = 0` to both. Their results
do not change, because the parent of a version row is a note, never a
folder. Without the condition, both queries scan the whole table. The
ANNOTATIONS query runs at each save of a full text.

### 2.3 Upgrade of existing vaults

`create_note_items_schema()` runs at each runtime open in the caller's
transaction. Extend it:

1. Add each missing column with `ALTER TABLE note_items ADD COLUMN`, as the
   function does for `forum_id`. Use the same column definitions as
   `create_note_items_table()`. SQLite checks the new CHECK constraints
   against the existing rows. All existing rows have the default values, so
   the checks pass.
2. Run `create_note_items_indexes()` after the columns exist. It drops the
   two old indexes and creates the new ones. A partial unique index on fewer
   rows always accepts the data of the old index.

This changes no rows. It needs no table rebuild, no new database version,
and no sync record. A test of this SQL on a copy of a current local vault
passed `PRAGMA integrity_check`. On that copy, the constraints rejected a
second L1 row, a level on an original row, a second Summary note in a
folder, and a Summary note at the vault root. The compressed-text migration,
`rebuild_compressed_note_items()`, creates the new table with
`create_note_items_table()` and then calls `create_note_items_indexes()`, so
it creates the new columns and indexes too.

### 2.4 Queries that change

The "find the compressed version" queries must add `level = 0`. Otherwise
they also find L1 and L2 rows. The "any version row" queries stay as they
are.

| Place | Change |
| --- | --- |
| `find_item()`, `note_list()` | Replace the `LEFT JOIN ... c.compressed=1` with three `EXISTS` expressions for levels 0, 1, and 2. Return `has_compressed`, `has_l1`, and `has_l2`. A join on all version rows would return one row for each version. |
| `stored_note_fields()` (`note_storage.cpp`), `note_fields()` (`sync_storage.cpp`) | Read the content of each level. See section 9.1. |
| `note_get()` | Select the version row with `c.compressed=1 AND c.level=?` for the requested mode. See section 2.6. |
| `note_get_text()` | Add `c.level=0`. Chats keep the compressed-or-original rule. |
| `write_compressed_note()` | Becomes `write_version_row(database, id, level, blob)`. The `UPDATE` and `INSERT` use the level. Compression calls it with level 0. |
| `delete_compressed_note()` | Remove it. Its `DELETE` of all version rows of a note moves into the new `replace_summary_note()`, which `note_commit_summary()` and `note_commit_folder_summary()` call. Add `delete_version_row(database, id, level)` for incoming sync records with a null field. |
| `document_owner()` | No change. A row with `compressed=1` belongs to its parent note, for every level. |
| `name_taken()`, the source query of `rebuild_folder_annotations()` | Add `compressed = 0`, so SQLite uses `note_items_original_name` (section 2.2). |
| Folder export and import | See section 9.2. |
| Filters with `compressed=0` (tree, paths, sync, folder store upload) | No change. They select original rows, and L1 and L2 rows have `compressed=1`. |

### 2.5 Note items

Add these fields to `NoteItem`, its JSON, `resources/dto.yaml`, and the
note guards in `webapp/src/api/noteGuards.ts`:

| Field | Value |
| --- | --- |
| `has_l1` | An L1 row exists. |
| `has_l2` | An L2 row exists. |
| `summary_excluded` | The note's exclusion flag. |
| `folder_summary` | The note is the Summary note of its folder. |

Change the meaning of `is_summary`. It is true for session summary notes and
for the Summary note: `s.note_id IS NOT NULL OR n.folder_summary = 1`. The
PRD calls both of them summary notes, and they share the menu rules and the
compression rule. `summary_linked` keeps its meaning: a session summary that
is linked to a session.

With this change, these existing checks also apply to the Summary note with
no further change:

- `NotesService::compress_note()` refuses or skips an `is_summary` note.
  Folder compression thus skips the Summary note.
- The tree hides **Compress** for `isSummary` rows.

The guard adds `(!value.folder_summary || value.is_summary)`. ChaWeb uses
the same `NoteItem` JSON and the same guard.

### 2.6 Reading and saving versions

Extend `NoteTextMode` to `full_text`, `compressed`, `l1`, and `l2`, in C++,
in the `notes.get` parser, and in `resources/dto.yaml`. Map the modes to
levels: `compressed` is 0, `l1` is 1, `l2` is 2.

`note_get()` keeps its rule from the editing design:

| Requested mode | Version row exists | Document shown | Save destination |
| --- | --- | --- | --- |
| `full_text` | — | Full text | Original row |
| `compressed`, `l1`, or `l2` | No | Full text | Original row |
| `compressed`, `l1`, or `l2` | Yes | That version | That version row |

The response does not change. `document_id` equals `id` for the full text.
Otherwise the document is the requested version, because the fallback goes
only to the full text.

`note_save()` and `note_save_scroll_position()` do not change.
`document_owner()` resolves an L1 or L2 row to its note. The save then
applies the note's read-only setting, checks the `doc` root, skips the
ANNOTATIONS rebuild, and flags the original note for sync. These are the
rules for the compressed row today.

A replacement of a version row keeps its row ID and `scroll_top`, as
compression does today.

### 2.7 New storage functions

Add to `note_storage.h`:

```cpp
// Caller checks eligibility. Writes the L1 or L2 row, the success event,
// and the sync record in one transaction.
void note_commit_level(storage::SqliteDatabase& database, std::int64_t id,
    int level, const nlohmann::json& document, std::string_view success_message,
    std::string_view sync_machine = {});

// Creates or replaces the Summary note of folder_id in one transaction.
[[nodiscard]] NoteItem note_commit_folder_summary(storage::SqliteDatabase& database,
    std::int64_t folder_id, const nlohmann::json& document,
    std::string_view sync_machine = {});

[[nodiscard]] NoteItem note_set_summary_excluded(storage::SqliteDatabase& database,
    std::int64_t id, bool excluded, std::string_view sync_machine = {});
```

`note_commit_level()`:

1. Require an original row with `kind = 'note'` that is not a summary note.
   Otherwise fail with "That item is not a note.".
2. Require a `doc` root. Encode the document with `compact_document()`,
   which applies the 12 MiB limit.
3. Write the row with `write_version_row()`.
4. Append the success event with `event_append_in_transaction()`.
5. If sync is connected, queue a `note.level` record (section 9.1).

The read-only setting of the note does not block this write. Compression
writes its result in the same way. The read-only rule of section 2.6 applies
only to edits in the editor. EPUB chapters are read-only, and CHA must still
write their L1 and L2.

`note_commit_folder_summary()`:

1. Require a folder row with no `forum_id`. Require a `doc` root and
   readable text, with the existing `summary_text()` check. Apply the
   12 MiB limit.
2. Find the row with `parent_id = folder_id AND folder_summary = 1`.
3. If it exists, replace it as `note_commit_summary()` replaces a session
   summary: abandon and delete its audio, delete all its version rows, and
   set `content_json`, `read_only = 1`, and `scroll_top = 0`.
4. If it does not exist, insert a note with `read_only = 1` and
   `folder_summary = 1`. Its name is "Summary". If that name is taken in
   the folder, use "Summary (2)", "Summary (3)", and so on, as
   `summary_name()` adds copy numbers.
5. Rebuild the folder's ANNOTATIONS with `allow_creation = false`, as a
   session summary commit does. The replacement removed the old highlights.
6. Append the success event.
7. Sync: a new note gets a `note.create` record. A replaced note gets the
   changed flag through `sync_flag_note()`, as a session summary does.

`note_set_summary_excluded()` follows `note_set_read_only()`: it requires a
`kind = 'note'` original row, updates the flag, and queues a
`note.summary_excluded` record when sync is connected. It returns the
updated item.

### 2.8 Unchanged behavior

These rules come from the existing code. They need no change:

- Deleting a note, or a folder, deletes its version rows through the
  `parent_id` cascade. Deleting the Summary note deletes its flag with it.
  The next folder operation then creates a new Summary note.
- Rename of a note does not change its version rows.
- `rebuild_folder_annotations()` reads the notes whose parent is the folder.
  It thus includes the Summary note and never includes L1 or L2 rows.
- The character summary search joins `note_summaries`. The Summary note has
  no `note_summaries` row, so the search never shows it.
- Chats read `note_get_text(id, true)`: the compressed version or the
  original. L1 and L2 are never chat input.
- ChaWeb lists original rows and reads the full text. It shows the Summary
  note as a regular note and never shows L1 or L2.

## 3. Prompts

### 3.1 Catalog

Add four entries to `prompt_catalog` in
[prompt_catalog.cpp](../src/workspace/prompt_catalog.cpp). The array size
becomes 9. Put the entries after `note-compression`:

| ID | Display name | Default source |
| --- | --- | --- |
| `summary-l1` | Summary L1 | `resources/summary-l1-prompt.md` |
| `summary-l2` | Summary L2 | `resources/summary-l2-prompt.md` |
| `summary-l3-section` | Summary L3 section | `resources/summary-l3-section-prompt.md` |
| `summary-l3-overview` | Summary L3 overview | `resources/summary-l3-overview-prompt.md` |

The display names are visible in Settings. They are part of the UI design
in section 11.

`summary-l1-prompt.md` is an exact copy of `compression-prompt.md`. It is a
separate file, so a later change of the compression resource does not change
the L1 default. Section 15 gives the default texts of the other three
prompts.

Embed the four files with `cmake/embed_text.cmake`, as the existing prompt
resources are, with the symbols `embedded_summary_l1_prompt()`,
`embedded_summary_l2_prompt()`, `embedded_summary_l3_section_prompt()`, and
`embedded_summary_l3_overview_prompt()`.

### 3.2 Existing vaults

The prompt design already covers new catalog entries:

- A new vault receives all catalog rows from
  `default_prompt_config_files()`, which reads the catalog.
- An existing vault that has prompt rows does not get the four new rows.
  The workspace resolves each missing row to its bundled default. Settings
  shows that default. A save creates the row.

No migration is necessary.

## 4. Backend operations

Add these methods to `NotesService`. All of them take the lifecycle lock and
call `require_admitted(epoch)`, as the existing note operations do.

```cpp
enum class SummaryPart { l1, l2, l3_section, l3_overview, l3 };

nlohmann::json summary_plan(std::int64_t id, std::uint64_t epoch);
std::shared_ptr<OperationReply> generate_summary_part(std::string connection_id,
    std::int64_t id, SummaryPart part, std::string text, std::uint64_t epoch);
void commit_summary_part(std::int64_t id, SummaryPart part,
    const nlohmann::json& document, std::uint64_t epoch);
void abandon_summary_part(std::int64_t id, SummaryPart part, std::uint64_t epoch);
void cancel_summary_parts(std::string_view connection_id, std::uint64_t epoch);
NoteItem set_note_summary_excluded(std::int64_t id, bool excluded, std::uint64_t epoch);
```

`l3` is the complete Summary note. `l3_section` and `l3_overview` are model
calls only.

### 4.1 Text size

"Text" means the UTF-8 bytes of the Markdown that `note_get_text()` returns.
Compression uses the same measure for its 4 KiB minimum. The limits are:

- minimum: 4 KiB, 4096 bytes, as `note_compression_min_bytes`;
- maximum: 256 KiB, 262144 bytes, as `text_file_byte_limit`.

A note has a valid size when 4096 ≤ bytes ≤ 262144.

### 4.2 Plan

`summary_plan(id)` checks the start conditions and returns the target notes.
It makes no model call and writes no event.

For a note:

1. The item must be an original note that is not a summary note.
   Otherwise: "That item is not a note.".
2. The L0 text size must be valid. Otherwise one of the messages in
   section 11.6.
3. The provider must be ready: call `prepare_note_provider()` with the
   `summary-l1` prompt and discard the result. A missing provider gives the
   compression message "Select a compression provider in Notes settings.".
4. Return the note. The exclusion flag does not apply.

For a folder:

1. The item must be a folder. Otherwise: "That item is not a folder.". A
   managed chat folder gives the existing "This folder is managed by
   CHA.". The UI never offers these cases.
2. Check the provider as for a note.
3. Return the included notes: notes whose parent is the folder, that are
   not summary notes, whose `summary_excluded` is 0, and whose L0 text has
   at least 4096 bytes. A note with more than 256 KiB of text is included.
   Its calls fail in section 4.3, as the PRD requires.

Result:

```json
{
  "notes": [
    { "id": 42, "name": "Chapter 1", "has_l1": false, "has_l2": true }
  ]
}
```

The order is by ID. The webapp sorts the notes in tree order, because the
tree order comes from `compareNoteSiblings()` in the webapp.

### 4.3 Generate

`generate_summary_part(connection_id, id, part, text)` makes one model call
and returns the Markdown through the reply:

```json
{ "markdown": "..." }
```

Input of each part:

| Part | Item | User message | Size rule | Prompt |
| --- | --- | --- | --- | --- |
| `l1` | Note | `note_get_text(id, false)` | At most 256 KiB | `summary-l1` |
| `l2` | Note | `note_get_text(id, false)` | At most 256 KiB | `summary-l2` |
| `l3_section` | Note | `note_get_text(id, true)` | At most 256 KiB | `summary-l3-section` |
| `l3_overview` | Folder | `text` from the request | At most 12 MiB | `summary-l3-overview` |

Steps before the worker starts, under the lifecycle lock:

1. Read the item. A note must be an original note that is not a summary
   note. A folder must be a folder with no `forum_id`. Read the input.
   Check the size rule of the table. The 4 KiB minimum is not checked
   again.
2. Prepare the definition with `prepare_note_provider()` and the part's
   prompt from the current workspace snapshot.
3. Add a cancel flag for this call to the connection's list (section 4.5).
4. Launch the worker with `background_jobs.launch()` and the same cancel
   flag.

If a step fails, append an error event (section 10) and fail the request.
In a folder operation, a note with more than 256 KiB of text fails here,
with its event, as the PRD requires. Other failures here are rare, because
the plan made the same checks.

The worker:

1. Runs the model call with the retry policy of compression: up to three
   retries of a retryable result, after 10, 20, and 40 seconds. Before each
   attempt it checks the cancel flag and `require_admitted(epoch)` under the
   lifecycle lock, and creates the `ProviderClient` there.
2. Checks the answer as `require_summary_answer()` does: cancelled, refused,
   failed with a safe reason, incomplete finish reason, no text, or more than
   12 MiB. Use messages that do not mention sessions, for example "The
   provider returned an incomplete result.".
3. Removes its cancel flag from the list.
4. If the call failed or was cancelled, appends an event and fails the
   reply. If the vault changed, it appends no event, as
   `retire_compression()` does.
5. Completes the reply with the Markdown. A successful call writes no event.
   The commit writes the success event.

The calls do not use `compression_mutex`. Up to three calls of one window
run at the same time. They are independent of compression jobs.

### 4.4 Commit and abandon

`commit_summary_part(id, part, document)`:

- `l1` or `l2`: call `note_commit_level()` with the success message.
- `l3`: `id` is the folder. Call `note_commit_folder_summary()`. Then
  deliver the audio notices as `commit_session_summary()` does, because the
  replacement can abandon audio.
- Other parts: invalid argument.
- On a `NoteError`, append an error event and fail the request.

`abandon_summary_part(id, part)` records a conversion failure from the
webapp. It accepts `l1`, `l2`, and `l3`. It appends an error event with the
reason "the result could not be converted to a note". It changes no note.

A commit does not need a reservation from the generate call. The webapp
already has the document, and `notes.save` already accepts any document from
the webapp. This keeps the backend state to the cancel flags.

### 4.5 Cancel

Keep a list of running calls in `NotesService::Impl`, guarded by the
lifecycle lock:

```cpp
struct SummaryCall {
    std::string connection_id;
    std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
};
std::vector<std::shared_ptr<SummaryCall>> summary_calls;
```

- `cancel_summary_parts(connection_id)` sets the cancel flag of each call of
  that connection. The provider client stops the running request. Each
  cancelled worker appends its own info event and fails its reply with
  `operation_cancelled`.
- The existing `cancel_summaries_locked()` also sets these flags: for one
  connection, or for all calls when it has no argument. The application
  already calls it when a connection closes and during vault maintenance.
- Application shutdown sets the flags through `BackgroundJobs`, because the
  worker was launched with the same flag.

The list holds only flags. It holds no results and no other state.

### 4.6 Exclusion flag

`set_note_summary_excluded(id, excluded)` calls
`note_set_summary_excluded()` and returns the item.

## 5. Native bridge contract

Add six methods. All of them require the current context epoch.

| Method | Parameters | Result |
| --- | --- | --- |
| `notes.summaryPlan` | `{ "id": 7 }` | `SummaryPlan` |
| `notes.summaryGenerate` | `{ "id": 42, "part": "l1" }` | `{ "markdown": "..." }`, background reply |
| `notes.summaryCommit` | `{ "id": 42, "part": "l1", "document": {...} }` | `{}` |
| `notes.summaryAbandon` | `{ "id": 42, "part": "l1" }` | `{}` |
| `notes.summaryCancel` | `{}` | `{}` |
| `notes.setSummaryExcluded` | `{ "id": 42, "excluded": true }` | `NoteItem` |

Parameter rules:

- `notes.summaryGenerate`: `part` is `l1`, `l2`, `l3_section`, or
  `l3_overview`. Only `l3_overview` takes `text`, a nonempty string. It is
  required for that part and refused for the other parts.
- `notes.summaryCommit`: `part` is `l1`, `l2`, or `l3`. `document` is an
  object.
- `notes.summaryAbandon`: `part` is `l1`, `l2`, or `l3`.
- Check exact keys with `require_only_keys()`, as the other note methods do.

Router integration:

- Start `notes.summaryGenerate` with `start_background(connection_id, id,
  reply, true)`. The `true` value calls `release_connection_resources()` if
  the connection closed before the reply was registered, as for session
  summaries.
- Add `Method::notes_summary_generate` to `deadline_exempt()`. A call can
  take many minutes.
- The other methods are ordinary requests.

Add the DTOs `SummaryPlan`, `SummaryTarget`, `SummaryGenerateRequest`,
`SummaryGenerated`, `SummaryCommitRequest`, `SummaryAbandonRequest`, and
`NoteSummaryExcludedRequest` to `resources/dto.yaml`. Add the new values to
`NoteTextMode` and the new fields to `NoteItem`. Regenerate
`webapp/src/api/schema.d.ts` with `npm run api-types`. Add wire fixtures.

## 6. Webapp: client and conversion

### 6.1 Client methods

Add to `NotesClient`, `nativeClient.ts`, and the response guards:

```typescript
summaryPlan(id: number): Promise<SummaryPlan>;
summaryGenerate(id: number, part: SummaryGeneratePart, text?: string): Promise<string>;
summaryCommit(id: number, part: SummaryCommitPart, document: Record<string, unknown>): Promise<void>;
summaryAbandon(id: number, part: SummaryCommitPart): Promise<void>;
summaryCancel(): Promise<void>;
setNoteSummaryExcluded(id: number, excluded: boolean): Promise<NoteItem>;
```

Test client fixtures implement them where the tests need them.

### 6.2 Markdown conversion

Move the shared part of `summaryDocument()` in
[summaryImport.ts](../webapp/src/notes/summaryImport.ts) into two new
functions:

```typescript
function markdownNoteNode(markdown: string): ProseMirrorNode;
export function markdownNoteDocument(markdown: string): Record<string, unknown>;
```

`markdownNoteNode()` does the first part of `summaryDocument()` today:

1. Check the 12 MiB Markdown limit.
2. Render with `notePageHtml(..., { summary: true })`. This permits tables.
3. Reject table cells with `rowspan` or `colspan` other than 1. Flatten each
   table with `flattenClipboardTable()`. Each row becomes one plain line
   with tabs between the cells.
4. Parse with the `notesExtensions()` schema and return the node.

`markdownNoteDocument()` refuses a node with no text, converts the node with
`toJSON()`, and checks the 12 MiB document limit. Both functions throw on
failure.

`summaryDocument()` calls `markdownNoteNode()`, keeps its chapter check on
the node, and then converts and checks the size as today. Its behavior and
its error message do not change. The Notes editor gets no table support.

### 6.3 Summary note document

The webapp assembles the Summary note from separate conversions:

```text
content =
    markdownNoteDocument(overview).content
  + for each section, in tree order:
        { "type": "heading", "attrs": { "level": 1 },
          "content": [{ "type": "text", "text": <note name> }] }
      + markdownNoteDocument(section).content
```

The note names become heading nodes directly. They do not go through
Markdown, so a name with Markdown characters stays literal. If any
conversion throws, or the result is over the 12 MiB document limit, the
whole Summary note fails as a conversion failure.

The overview has no heading. The sections use level-1 headings, so the
editor's Content menu lists the notes of the folder. The section and
overview prompts ask for no level-1 headings (section 15).

## 7. Webapp: the operation

### 7.1 Placement

Add `summarizeAndWait()` in a new file
`webapp/src/notes/summaries.ts`, beside `compressAndWait()` in
`compression.ts`. It runs the plan, the loop, and the Summary note.

Add a `summarizeNotes` action in `App.tsx` beside `compressNote`. It reuses
the parts of the compression action that are not about compression:

- `noteLeave.beginNoteAction()` and `endNoteAction()`;
- the `compressionRun` reference, so the existing navigation, layout, and
  mode-change blocks apply. Only one blocking note operation runs at a
  time;
- `NoteCompressionDialog`. Add an optional `label` property for its
  accessible name. The summary operation passes "Note summaries";
- the vault-context check, as `checkContext()` in `compressNote`. The loop
  calls it before each note and before the overview. A vault change ends the
  operation;
- the editor flush before the start, the tree refresh, the reload of the
  open note, and the final unfreeze.

`NotesTree` gets an `onSummarize(source, epoch, refresh)` property, as it
has `onCompressNote`. A refused start throws, and the tree shows the error
message, as for compression.

### 7.2 Steps

```text
summarizeNotes(source):
  begin the note action; show "Saving note…" with Cancel enabled
  freeze and flush the open editor            -- as compression
  if the save failed or Cancel was requested: end
  plan = summaryPlan(source.id)               -- a refusal ends here
  targets = plan.notes in tree order
  folder = source.kind == 'folder'
  makeL3 = folder and targets is not empty
  sections = []
  for each target, with index i:
      if Cancel was requested: stop the loop
      check the vault context                 -- as compression
      show 'Summarizing “<name>”…', and 'Note i of N' for a folder
      calls = []
      if not folder or not target.has_l1: calls += makeLevel(target, 'l1')
      if not folder or not target.has_l2: calls += makeLevel(target, 'l2')
      if makeL3: calls += makeSection(target)
      wait until all calls settle
  if makeL3 and no Cancel:
      check the vault context
      show "Writing the Summary note…"
      markdown = summaryGenerate(source.id, 'l3_overview', sectionText())
      document = the assembly of section 6.3
          on conversion failure: summaryAbandon(source.id, 'l3'); stop
      if no Cancel: disable Cancel; summaryCommit(source.id, 'l3', document)
  finally: refresh the tree, reload if needed, unfreeze, close the dialog

makeLevel(target, part):
  markdown = summaryGenerate(target.id, part)
  if Cancel was requested: return
  document = markdownNoteDocument(markdown)
      on failure: summaryAbandon(target.id, part); return
  summaryCommit(target.id, part, document)

makeSection(target):
  markdown = summaryGenerate(target.id, 'l3_section')
      on failure: makeL3 = false; return
  sections += { name: target.name, markdown }
```

`sectionText()` joins the sections in tree order:

```markdown
# <note name>

<section Markdown>

# <next note name>

<section Markdown>
```

Rules:

- The notes run one at a time. The calls of one note run at the same time:
  at most three.
- A failed call does not stop the loop. The backend has already appended
  the event.
- After the first failed section, `makeL3` is false. The loop makes no more
  section calls and no overview call, because the Summary note cannot be
  written. L1 and L2 calls continue.
- A section is kept only in the memory of the loop. CHA does not store it.
- The note names come from the plan. The modal dialog blocks rename.
- For a note, the plan returns one target. The loop makes L1 and L2 and
  replaces both, also when they exist.

### 7.3 Cancel

Cancel in the dialog, or Escape:

1. Set `requested` on the run and show "Cancelling…" with Cancel disabled,
   as compression does.
2. Call `client.summaryCancel()`. The backend stops the running calls. The
   existing dialog handler calls `run.cancelJob?.()`, so the summary run
   sets `cancelJob` to a function that calls `client.summaryCancel()`.
3. The loop starts no more calls. A result that arrives after Cancel is not
   committed, also when its call completed. An L1 or L2 commit that was
   already sent completes. Its result stays, as the PRD requires.
4. The loop waits until all its calls settle, then continues with the
   final steps of section 7.4.

Cancel during "Saving note…" prevents the start, as for compression.

The dialog disables Cancel while the Summary note commit runs, as
compression disables it during "Opening note…". The commit is the last step
of the work. Thus a Cancel never leaves a changed Summary note.

### 7.4 Final steps

After the loop, as `compressNote` does:

1. Refresh the tree.
2. Reload the open note if one of these is true:
   - the mode is `l1` or `l2` and the open note was a target;
   - the open note is the folder's Summary note and the `l3` commit
     succeeded;
   - a `notes.changed` notice arrived during the operation (the existing
     `notesChanged` flag).
3. After a successful `l3` commit, call the existing `sourceChanged(folderId)`,
   as `saveSummary` does after a session summary. The commit rebuilt the
   folder's ANNOTATIONS, and this call reopens it when it is the open note.
4. Unfreeze the editor only when it was not reloaded.
5. Close the dialog and end the note action.

The dialog does not report success or failure. Settings → Events records the
outcome, as for compression.

## 8. Webapp: editor and tree

### 8.1 Editor modes

In [NotesScreen.tsx](../webapp/src/components/NotesScreen.tsx):

- `noteTextModes` becomes `['full_text', 'l1', 'l2', 'compressed']`, with
  the labels of section 11.3.
- `loadedTextMode` becomes `fullTextLoaded ? 'full_text' : noteTextMode`.
  The fallback goes only to the full text, so a version row is always the
  requested mode.
- The audio gate and `getNoteLength()` use `fullTextLoaded`. They need no
  change. The read-only setting applies to every version through the
  original note (section 2.6).

`AppState.noteTextMode` takes the four values. It stays a session-only
value that starts as `full_text`.

In [useNoteImports.ts](../webapp/src/notes/useNoteImports.ts), the import
target carries the loaded mode: `{ noteId, documentId, mode }`. The
reopen step reads `getNote(noteId, mode)` and checks `document_id` as
today. Today it derives the mode from `documentId === noteId`, which works
only for two modes.

### 8.2 Tree

In [NotesTree.tsx](../webapp/src/components/NotesTree.tsx) and
[tree.ts](../webapp/src/notes/tree.ts):

- `NoteTreeRow` replaces `hasCompressed` with `hasVersions`
  (`has_compressed || has_l1 || has_l2`). It also gets `summaryExcluded` and
  `folderSummary`. The dot uses `hasVersions`. After the removal in section
  8.3, no tree code reads `hasCompressed`.
- **Rename** is hidden for `summaryLinked || folderSummary` rows.
- **Compress** is already hidden for `isSummary` rows, which now include the
  Summary note.
- **Summarize** is shown on notes that are not summary notes, and on
  folders that are not managed. It is not on the vault row.
- **Exclude from summaries** is shown on notes that are not summary notes.
  It is a `menuitemcheckbox` with `aria-checked` and `CheckIcon`, as the
  Headings menu items are. The existing `menuKeys()` handles it. A click
  calls `setNoteSummaryExcluded()` and refreshes the tree.
- The item key that `NotesTree` builds to find list changes adds the new
  fields.

The exclusion flag does not show in the tree. Section 11 shows the menus.

### 8.3 Removal of Show compressed

The user decided on November 11, 2026, to remove **Show compressed** from
the note menu. The compressed version stays a version of the note. The user
opens it with **Compressed** in the editor mode menu (section 8.1), as PRD
section 5 requires.

This replaces the rule in
[compressed-note-edit-design.md](compressed-note-edit-design.md) section 4
that keeps **Show compressed** as a shortcut.

Changes:

- `NotesTree.tsx`: remove the **Show compressed** menu item and the
  `showCompressed()` function. The item goes away from every menu that
  shows it today: ordinary notes, where it is enabled when a compressed
  version exists, and the ANNOTATIONS item, where it is always disabled.
- `onOpenNote()` in `NotesTree.tsx` and `requestOpenNote()` in `App.tsx`:
  remove the `mode` argument. **Show compressed** was its only caller.
- Keep the `mode` field of the `open-note` action in `view.ts`. ANNOTATIONS
  links use it to open a source note in Full text mode.
- Keep `has_compressed` in `NoteItem`. The dot (`hasVersions`) and the
  backend use it.

The separate window that showed the compressed text as plain text
(`CompressedDialog` with a `<pre>` element) is already gone. The commit
"Improved compressed text visibility" removed it. No code, style, or text of
that window remains, so this feature has nothing more to remove there.

## 9. Transfers

### 9.1 D1 sync

Note fields. `stored_note_fields()` and `note_fields()` add:

| Field | Value |
| --- | --- |
| `l1_content` | Base64 of the L1 row's `content_json`, or null. |
| `l2_content` | Base64 of the L2 row's `content_json`, or null. |
| `summary_excluded` | Boolean. |
| `folder_summary` | Boolean. Only in `note.create`. |

`compressed_content` reads the level-0 row.

New record kinds:

```json
{ "kind": "note.level", "path": ["Book", "Chapter 1"], "level": "l1", "content": "<base64>" }
{ "kind": "note.summary_excluded", "path": ["Book", "Chapter 1"], "summary_excluded": true }
```

- `note_commit_level()` queues `note.level`. It is the L1/L2 counterpart of
  `note.compress`. A `note.update` record would delete the note's audio on
  the receiving machine.
- `note_set_summary_excluded()` queues `note.summary_excluded`, as
  `note_set_read_only()` queues `note.read_only`.
- A manual edit of L1 or L2 flags the original note. The existing
  `note.update` record carries all fields. The receiving machine deletes
  the note's audio, as for an edit of the compressed version today. The
  editing design accepts this effect.
- A new Summary note gets `note.create` with `folder_summary: true`. A
  replaced Summary note gets `note.update` through the changed flag.

Receiver, in `note_apply_record()`:

- `apply_insert_note()` and `apply_note_update()` apply `l1_content` and
  `l2_content` as `apply_compressed_content()` applies
  `compressed_content`: a string is decoded, checked for a `doc` root, and
  written; null or a missing field deletes the row. They set
  `summary_excluded` from the record; a missing field is false.
- `apply_insert_note()` for `note.create` sets `folder_summary` from the
  record. The CHECK and the unique index reject an invalid record. The
  existing error path then skips it with a warning.
- `note.level`: find the note with `apply_find()`, require `kind = 'note'`,
  decode and check the document, and call `write_version_row()`.
- `note.summary_excluded`: find the note, require `kind = 'note'`, and set
  the flag.

### 9.2 Folder export and import

Export. The export file's `note_items` table adds three columns:

```sql
level INTEGER NOT NULL,
summary_excluded INTEGER NOT NULL,
folder_summary INTEGER NOT NULL
```

The export query already walks the tree through `parent_id`. It thus copies
L1 and L2 rows with their note. It copies the three columns of each row.

Import. The importer reads the columns that the file has:

| File | Version rows | Flags |
| --- | --- | --- |
| Has `level` | Copy each version row with its level. | Copy `summary_excluded` and `folder_summary`. |
| Has `compressed`, no `level` | Copy the compressed row at level 0. | 0 |
| Has `compressed_text` | As today. | 0 |

Extend the existing validation query for the new layout. A version row must
have `level` in (0, 1, 2), `kind = 'note'`, content, and an original note as
parent. A note can have one row for each level. A `folder_summary` row must
be an original note whose parent is a
folder. A folder can have one such row. A failure gives "This file is not a
CHA folder export.".

Copy the version rows of a note right after the note, with the note's new
ID as parent and with `write_version_row()`. The `note.create` records of
the import carry `l1_content`, `l2_content`, `summary_excluded`, and
`folder_summary` through `stored_note_fields()`.

The export of a folder thus keeps its Summary note link, as the user
decided. The note is still a regular note in the file. Session summary
links stay out of the export, as today.

### 9.3 R2 sync and ChaWeb

R2 sync copies the database file. It copies the new rows and columns. The
receiving build upgrades the schema at open (section 2.3).

ChaWeb needs no change except the `NoteItem` guard (section 2.5).

## 10. Events

Settings → Events records the result of each job. A job is the L1 of a
note, the L2 of a note, or the L3 of a folder. The L3 of a folder has one
result: the Summary note commit, or the first failed section or overview
call. After that failure the loop makes no more L3 calls. Each cancelled
call writes an info event.

| Outcome | Severity | Text |
| --- | --- | --- |
| L1 or L2 saved | success | `L1 ready for “Chapter 3” (note 42).` |
| Summary note saved | success | `Summary ready for “Book” (folder 7, note 99).` |
| L1 or L2 failed | error | `Could not make L1 for “Chapter 3” (note 42): <reason>.` |
| Section failed | error | `Could not make the L3 section for “Chapter 3” (note 42): <reason>. The Summary note was not changed.` |
| Overview failed | error | `Could not make the L3 overview for “Book” (folder 7): <reason>. The Summary note was not changed.` |
| Summary note save failed | error | `Could not save the Summary note for “Book” (folder 7): <reason>.` |
| Call cancelled | info | `L1 cancelled for “Chapter 3” (note 42).` |

Reasons:

| Case | Reason |
| --- | --- |
| L1 or L2 input over 256 KiB | `the note has more than 256 KiB of text` |
| Section input over 256 KiB, no compressed version | `the note has more than 256 KiB of text. Compress the note, then run Summarize again` |
| Section input over 256 KiB, compressed version exists | `the compressed text has more than 256 KiB` |
| Conversion failure | `the result could not be converted to a note` |
| Provider result | The safe reasons and messages of section 4.3 |
| Storage | The `NoteError` message |

A successful section or overview call writes no event. Its result is not
saved by itself. A result that the webapp drops after Cancel writes no event.

## 11. UI design (for approval)

The square-bracket names for existing controls stand for their current
icons or labels. The sketches add no panel headings, legends, or helper
text.

### 11.1 Note menu

```text
Ordinary note
┌──────────────────────────────┐
│ Rename                       │
│ Make editable                │
│ Generate audio               │
│ Compress                     │
│ Summarize                    │  ← new
│ Exclude from summaries     ✓ │  ← new; ✓ only when set
│ Delete                       │
│ Copy Ref ID                  │
└──────────────────────────────┘

Summary note (folder)
┌──────────────────────────────┐
│ Make editable                │
│ Generate audio               │
│ Delete                       │
│ Copy Ref ID                  │
└──────────────────────────────┘
```

**Show compressed** is removed from the note menu (section 8.3). It was
between **Compress** and **Summarize**.

The Summary note menu follows the session summary rules of the PRD: no
Rename, Compress, or Summarize. It also has no Exclude from summaries,
because a summary note is never included.

### 11.2 Folder menu

```text
┌──────────────────────────────┐
│ New note                     │
│ New folder                   │
│ Import EPUB…                 │
│ Import folder…               │
│ Export folder…               │
│ Rename                       │
│ Generate audio               │
│ Compress                     │
│ Summarize                    │  ← new
│ Delete                       │
│ Upload to store              │
└──────────────────────────────┘
```

Managed chat folders and the vault row do not get Summarize. A chat folder
holds only session summaries, which are never included.

### 11.3 Editor mode menu

```text
┌────────────────────────────────────────────────────────────────────┐
│ [Mode ▾]  [Content]  [History]  [Edit]  [Highlight]  …              │
│ ┌──────────────────┐                                               │
│ │ Full text        │                                               │
│ │ L1             ✓ │                                               │
│ │ L2               │                                               │
│ │ Compressed       │                                               │
│ └──────────────────┘                                               │
│ The L1 document                                                    │
└────────────────────────────────────────────────────────────────────┘
```

The button keeps its icon and its accessible name "Note text mode". Its
title follows today's pattern: "L1 mode; showing L1", or "L1 mode; showing
full text" when the note has no L1. The title does not change "L1" and "L2"
to lowercase.

### 11.4 Progress dialog

```text
Single note                         Folder operation
┌────────────────────────────────┐  ┌────────────────────────────────┐
│ ◌ Summarizing “Research”…       │  │ ◌ Summarizing “Chapter 3”…      │
│                                │  │   Note 3 of 8                  │
│                      [Cancel]  │  │                      [Cancel]  │
└────────────────────────────────┘  └────────────────────────────────┘

Folder overview
┌────────────────────────────────┐
│ ◌ Writing the Summary note…     │
│                      [Cancel]  │
└────────────────────────────────┘
```

The other stages keep the compression texts: "Saving note…",
"Cancelling…", and "Opening note…". The dialog's accessible name is "Note
summaries".

### 11.5 Tree dot and Settings

- The dot's accessible name and title change from "Compressed version
  saved" to "Other versions saved". It shows for a compressed version, L1,
  or L2.
- Settings → Prompts shows four more rows after "Note compression":
  "Summary L1", "Summary L2", "Summary L3 section", and "Summary L3
  overview".

### 11.6 Messages

The tree's error area shows a refused start, as for compression:

- "This note is under 4 KiB and does not need summaries."
- "This note is over 256 KiB. Summaries are limited to 256 KiB of text."
- "Select a compression provider in Notes settings." (existing)

When a start fails without a public message, the tree's error area shows
"Could not start summaries. Try again.", as for compression.

## 12. Known effects

These effects follow from the PRD or from existing behavior. The design
does not add mechanisms for them.

- A manual edit of L1 or L2 sends `note.update`. The receiving machine
  deletes the note's audio, as for an edit of the compressed version.
- Other windows show the new dots after their next tree refresh. The design
  adds no new notice for other windows.
- A result that arrives after Cancel is dropped without an event.
- The `compressed` column marks every version row, also L1 and L2. A row
  with `compressed = 1` and `level = 1` is L1. This is the cost of the
  decision to keep the column. Comments at the schema and in the query
  helpers state this.

## 13. Implementation map

| Area | Changes |
| --- | --- |
| `src/storage/note_storage.h`, `.cpp` | Columns, indexes, schema upgrade, level-aware queries, `compressed = 0` in `name_taken()` and the ANNOTATIONS source query, `NoteTextMode`, `NoteItem` fields, `write_version_row()`, `delete_version_row()`, `note_commit_level()`, `note_commit_folder_summary()`, `note_set_summary_excluded()`, export/import layout, D1 apply for the new fields and records. Update the `note_export_folder()` comment: the Summary note link now goes into the export. |
| `src/storage/sync_storage.cpp` | `note_fields()` for the new fields. |
| `src/workspace/prompt_catalog.cpp`, `CMakeLists.txt`, `resources/` | Four catalog entries, four resource files, embedding. |
| `src/notes/notes_service.h`, `notes_service_internal.h`, `notes_operations.cpp` | Plan, generate, commit, abandon, cancel, exclusion flag, the call list, and the extension of `cancel_summaries_locked()`. |
| `src/app/application.h`, `notes_operations.cpp` | Wrappers for the new service methods. |
| `src/bridge/bridge_protocol.cpp`, `bridge_router.cpp`, `notes_dispatch.cpp` | Six methods, deadline exemption, `notes.get` modes. |
| `resources/dto.yaml`, `webapp/src/api/schema.d.ts` | DTOs and generated types. |
| `webapp/src/api/client.ts`, `nativeClient.ts`, `noteGuards.ts` | Client methods and guards. |
| `webapp/src/notes/summaryImport.ts` | `markdownNoteDocument()`. |
| `webapp/src/notes/summaries.ts` (new) | `summarizeAndWait()`. |
| `webapp/src/components/App.tsx`, `NoteCompressionDialog.tsx` | `summarizeNotes` action, dialog label, no `mode` argument in `requestOpenNote()`. |
| `webapp/src/components/NotesTree.tsx`, `webapp/src/notes/tree.ts` | Menu items, removal of **Show compressed**, dot, row fields. |
| `webapp/src/components/NotesScreen.tsx`, `webapp/src/notes/useNoteImports.ts` | Four modes. `webapp/src/state/view.ts` gets them from the generated `NoteTextMode` type. |

Implement in this order:

1. Storage: columns, indexes, upgrade, level-aware queries, and tests on a
   copy of an existing vault.
2. Prompts.
3. Backend operations and the bridge.
4. Webapp conversion and the operation loop.
5. Editor modes, tree menus, and the dot, after the UI approval.
6. Sync and folder transfer.

Do not change documents in `docs/` as part of this feature.

## 14. Validation

Use the existing suites and the fake provider transport. No live provider
call is necessary.

| Area | Checks |
| --- | --- |
| Schema upgrade | A vault from the current build opens and gets the columns and indexes. Rows and IDs do not change. A vault with `compressed_text` still converts. A second open changes nothing. `EXPLAIN QUERY PLAN` of `name_taken()` and of the ANNOTATIONS source query shows a search with `note_items_original_name`, not a scan. |
| Versions | L1 and L2 rows next to a compressed row under one note. `has_compressed`, `has_l1`, and `has_l2` are correct. `note_list()` returns one row per note. Delete removes all versions. |
| Reading and saving | All four modes, with and without the version row. Save of an L1 row applies the note's read-only rule and does not change the other versions. Scroll positions per row. |
| Plan | Note size limits at 4095, 4096, 262144, and 262145 bytes. Summary notes refused. Folder: excluded, summary, small, and large notes; subfolders not included; managed folder refused; provider message. |
| Generate | Each part uses its prompt and its input. Section uses the compressed text when it exists. Size failures write the events of section 10. Retries. Cancel stops a running call and writes an info event. Connection close cancels. No event after a vault change. |
| Commit | L1/L2 commit writes the row, the event, and `note.level` in one transaction, also for a read-only note. Summary note: create with "Summary" or a copy number, replace by the flag and never by name, read-only, audio removed, ANNOTATIONS rebuilt, ordinary note named "Summary" untouched. |
| Webapp loop | Tree order; three calls at the same time; folder skips existing L1 and L2; note replaces both; a failed call continues; a failed section stops L3 calls and the overview; no included notes makes no calls; Cancel during save, calls, and overview; a late result after Cancel is not committed; a vault change ends the loop; reload rules; an open ANNOTATIONS reopens after the Summary note commit. |
| Conversion | Lists, headings, emphasis, tables as plain lines, a table with `colspan` fails and calls abandon, note names with Markdown characters stay literal, size limit. `summaryDocument()` behavior is unchanged. |
| Exclusion | Menu check mark; folder runs skip the note; note runs ignore the flag. |
| Compression | Folder compression skips the Summary note. Compression of a note keeps its L1 and L2. |
| Chats, search, ChaWeb | Chats read compressed or original text only. The character summary search does not return the Summary note. ChaWeb lists the Summary note and shows no L1 or L2. |
| D1 sync | Create and update records carry the new fields. `note.level` keeps the receiver's audio. `note.summary_excluded`. Summary note link on create. Null fields delete rows. |
| Folder transfer | Round trip of L1, L2, flags, and the Summary link. Import of files with the `compressed` layout and with the `compressed_text` layout. Invalid version rows refused. |
| UI | Menus, check mark, mode menu, dot label, dialog texts and focus, Escape, blocked shortcuts. No note menu has **Show compressed**; the compressed version opens with **Compressed** in the mode menu. |

Run `npm run api-types`, `npm run check`, and the relevant Vitest suites.
Build the C++ targets and run the note storage, notes service, workspace,
bridge, sync, and ChaWeb adapter tests. Add a new native smoke test,
`tests/native/macos/test-note-summaries.py` with
`tests/native/note-summaries.js`: summarize a folder, open L1 and L2, open
the Summary note, and cancel a folder run. Do not add these steps to the
native note compression test. That test checks an exact list of compressed
rows and an exact count of events, and summary steps in the same vault would
break these checks.

## 15. Default prompt texts

`summary-l1-prompt.md` is a copy of `resources/compression-prompt.md`.

### 15.1 summary-l2-prompt.md

```markdown
You make a short, structured version of a text for a human reader. The
reader wants the main ideas and the key facts with less reading. The reader
can open the full text later for details.

Write in the language of the source text.

START

Start with one to three sentences that state the subject of the text and
its main point. Do not put a heading before them.

BODY

Then give the main ideas and the key facts. Select the form from the
content of the text:

- A book of ideas or arguments: the claim, the reasons, and the evidence.
- A textbook or a technical text: definitions, procedures as numbered
  steps, and comparisons.
- History, biography, or journalism: people, events, and causes in time
  order.
- A scientific article: the question, the method, the results, and the
  limits.
- A how-to text: the practices, their steps, and when to use them.
- Philosophy or an essay: the chain of reasoning in short paragraphs.

If the text mixes these kinds, combine the forms.

Use lists first. Use short paragraphs where the reasoning needs connected
sentences. Use a table only to compare several items on the same
properties.

RULES

- Use only information from the text. Do not add facts, opinions, or
  advice.
- Keep names, numbers, dates, definitions, and technical terms exact.
- Keep the author's position separate from positions that the author
  reports or rejects.
- Write Markdown: headings, lists, bold, italic, and tables.
- Return only the result. Do not add an introduction, a closing remark, or
  comments about the task.
```

### 15.2 summary-l3-section-prompt.md

```markdown
You write one section of a summary of a book or of a set of texts. The user
message contains one part, for example one chapter or one article. The
section tells the reader what this part contains, so that the reader can
decide which parts to read.

Write a few short paragraphs with the main ideas and the key facts of this
part. Write in the language of the text.

If the text has no substantive content, for example a table of contents, an
index, a list of references, a copyright page, or acknowledgements, write
one sentence that says what the text is.

RULES

- Use only information from the text.
- Keep names, numbers, and technical terms exact.
- Do not start with a heading or with the title of the part. CHA adds the
  title.
- Do not use headings. Use a list only when it makes the content clearer.
- Return only the section text.
```

### 15.3 summary-l3-overview-prompt.md

```markdown
You write the overview at the start of a summary of a book or of a set of
texts. The user message contains the sections of the summary in reading
order, one section for each part. Each section starts with a level-1
heading that contains the name of the part.

Write the overview:

1. The subject of the whole set.
2. Its main argument or purpose.
3. How the parts connect: how the argument develops from part to part,
   which parts depend on other parts, and which parts are reference
   material.

Use short paragraphs. A short list is permitted for the connections. Write
in the language of the sections.

RULES

- Use only the information in the sections.
- Do not repeat the sections one by one. The sections follow the overview.
- Do not use level-1 headings. Do not add a title.
- Return only the overview text.
```
