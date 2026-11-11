# Compressed note JSON storage

Date — November 10, 2026

Status — Proposed technical design.

Store the AI-compressed version of a note in its own `note_items` row. The row
uses the same columns as the original note. Its `content_json` holds a Tiptap
document encoded as compact UTF-8 JSON and compressed with zlib. An integer
column tells the two versions apart. Convert the document to Markdown text
when a model needs it. Convert existing saved compressed notes when the vault
opens, including notes that the user never opens.

This stage changes storage only. It adds no editing of compressed notes and
no change to the note API or the UI. Both versions use the same row format, so
the next stage can add editing with the existing note operations.

## Requirements and decisions

| Requirement | Design |
| --- | --- |
| Store compressed notes as zlib-compressed JSON | Store the compressed version in `content_json` of its own `note_items` row, like the original. |
| Make the original and the compressed version equal | Use the same table and columns for both. The integer column `compressed` is 0 for the original and 1 for the compressed version. The compressed row's `parent_id` points to its original note. |
| No editing of compressed notes at this stage | Keep the note API and the UI unchanged. `notes.get` continues to return `compressed_text`, now made from the compressed document. |
| Send text to models | Serialize the selected document through `note_document_markdown()` before building the model context. |
| Transition existing vaults | Convert the `note_items` table in one transaction at runtime open. Create a compressed row from each existing compressed text value. |
| Continue to accept saved data from older builds | Convert legacy folder exports and legacy D1 note records at their input boundaries. |

The proposed initial conversion preserves text literally, including Markdown
markers and whitespace. It puts the saved text into one paragraph. This
choice lets the same small C++ function handle existing vaults, new model
answers, folder imports, and incoming sync records. It also works when
`cha-daemon` opens a vault without a browser connection.

Interpreting existing Markdown as rich headings, lists, and marks is a separate
conversion choice. The current Markdown import function depends on a browser
DOM and adds summary-specific validation. It cannot run directly inside a
native vault migration. The literal conversion proposed here requires only
the existing JSON and zlib dependencies.

## Current implementation

`note_items.content_json` holds the original document as a zlib BLOB.
`note_items.compressed_text` holds the generated compressed version as SQLite
TEXT in the same row. `note_save_compressed()` and `note_commit_compressed()`
write that text. Only `note_commit_compressed()` has a production caller, the
compression worker. Only tests call `note_save_compressed()`. `note_get()`
returns the text as `NoteContent.compressed_text`.

`note_get_text(database, id, prefer_compressed)` currently selects the saved
compressed text when requested and available. Otherwise it converts the
original document to Markdown. Session setup and the session controller use
this function with `prefer_compressed = true`.

Folder exports copy `compressed_text` into another SQLite database. D1
`note.create`, `note.update`, and `note.compress` records also carry it. The
current compressed-note dialog displays it in a `<pre>` element.

The database identity uses `PRAGMA user_version = 2`. Runtime open,
`open_database()` in `workspace_config_store.cpp`, already upgrades version 2
vaults in place: it adds missing columns and tables. One older transition in
`create_note_items_schema()` drops `note_items` when `content_json` has type
TEXT. That older table has no `compressed_text` column. This design keeps that
transition unchanged.

The files that change are
[note_storage.cpp](../src/storage/note_storage.cpp),
[note_storage.h](../src/storage/note_storage.h),
[workspace_config_store.cpp](../src/workspace/workspace_config_store.cpp),
[sync_storage.cpp](../src/storage/sync_storage.cpp), and
[folder_store_storage.cpp](../src/storage/folder_store_storage.cpp). The webapp
code does not change. Only its editor tests get a fixture.

## Stored representation

Replace `compressed_text TEXT` with an integer column in the `note_items`
schema. The database version stays 2.

```sql
compressed INTEGER NOT NULL DEFAULT 0 CHECK (compressed IN (0, 1)),
```

| Row | `compressed` | `parent_id` | `content_json` |
| --- | --- | --- | --- |
| Folder, original note, or ANNOTATIONS | 0 | Parent folder, or NULL at the root | As today |
| Compressed version | 1 | The original note | AI-compressed Tiptap document, compressed with zlib |

A compressed row has `kind = 'note'`. Its `name` is a copy of the original
note's name when the row is created. This stage does not read it. The other
columns keep their defaults. Add this table constraint and this index:

```sql
CHECK (compressed = 0 OR (kind = 'note' AND parent_id IS NOT NULL)),
```

```sql
CREATE UNIQUE INDEX IF NOT EXISTS note_items_compressed_parent
    ON note_items(parent_id) WHERE compressed = 1;
```

The index allows at most one compressed row for each note, like the existing
index that allows one ANNOTATIONS row for each folder. In the annotation
CHECK, replace `compressed_text IS NULL` with `compressed = 0`.

The existing `parent_id` foreign key has `ON DELETE CASCADE`. Deleting a note,
or a folder that contains it, therefore deletes its compressed row. Renaming
the original does not touch the compressed row. A compressed row is not a
sibling of the folder's notes, so name checks do not change.

`has_compressed` becomes true when a row with `parent_id` equal to the note ID
and `compressed = 1` exists.

### Rows that note operations see

At this stage, note operations see only original rows.

- `note_list()` returns only rows with `compressed = 0`. The tree, ChaWeb,
  and every caller of `list_note_items()` therefore never see a compressed
  row.
- `find_item()`, `note_get()`, and `note_get_text()` select only rows with
  `compressed = 0`. The tree never shows a compressed row's ID, but a chat
  prompt accepts a typed item reference with any ID. For a compressed row's
  ID, these lookups report that the note was not found.
- `rebuild_folder_annotations()` reads the notes whose parent is the folder.
  A compressed row's parent is a note, so it is never a source.
- The folder store upload in `capture_impl()` walks the folder tree through
  `parent_id`. Add `compressed = 0` to its recursive step, so it uploads only
  original notes.

Sync path lookups, flagged-note collection, the summary search, note audio,
read-only changes, scroll positions, and chat note copies work with original
note IDs and need no change.

### Writes

`note_commit_compressed()` and `note_save_compressed()` keep their text
arguments. Both convert the text with `note_document_from_text()` and write
through one helper. The helper requires an original row with
`kind = 'note'`. Otherwise it reports that the note was not found, as the
current `kind = 'note'` update does. It replaces `content_json` of the
existing compressed row and keeps its ID, or inserts a compressed row when
none exists. The source comparison, row write, success Event, and sync record
in the commit operation remain in one transaction.

`note_commit_summary()` currently clears `compressed_text` when it replaces a
summary. It deletes the compressed row instead.

### Literal text conversion

Add `note_document_from_text(text)` beside the existing note document helpers.
For a nonempty input it produces

```json
{
  "type": "doc",
  "content": [
    {
      "type": "paragraph",
      "content": [
        { "type": "text", "text": "First line\n\nSecond line" }
      ]
    }
  ]
}
```

Store the complete input in the text node. Preserve Unicode, tabs, newlines,
leading and trailing whitespace, and punctuation. JSON escaping is handled
by `nlohmann::json`. Empty input becomes a document containing one empty
paragraph, since ProseMirror text nodes must be nonempty.

The document is valid for `notesExtensions()`. The current serializer returns
an unmarked text node's string directly, so the model input of an existing
compressed note does not change:

```text
note_document_markdown(note_document_from_text(text)) == text
```

Do not split the input into `hardBreak` nodes during conversion, because their
Markdown serialization adds a backslash before the newline.

For the next stage: when the editor reads back an edited paragraph,
ProseMirror parses it with `preserveWhitespace: true`. Tiptap's HardBreak sets
`linebreakReplacement`, so each newline in that paragraph becomes a
`hardBreak` node. The converted text is one paragraph, so one typed character
converts all of its newlines. After that edit, the model text has a backslash
before each line break.

New model results use this converter after the existing completion,
nonempty-answer, cancellation, and source-change checks. The model continues
to return compressed text. Application code constructs the JSON document.

### JSON and zlib helpers

Reuse the existing compact JSON, zlib compression, and document decoding
logic for the compressed row. Reuse the current note JSON checks at the
storage, import, and sync boundaries, with a `doc` root check for the
compressed document. A fixture verifies that generated documents load with
`notesExtensions()`.

Fresh generation, folder imports, and incoming sync records use the existing
12 MiB document limit, measured on compact UTF-8 JSON before zlib compression.
Test the serialized size because escaping and JSON structure add bytes to the
input.

Migration converts every legacy value without a size check, so it never
drops saved text. Legacy compressed text is model output and is far below
the limit. Reads use the existing decoder and its limit.

## Reading for models and chat copies

Keep the `note_get_text()` interface and the meaning of `prefer_compressed`.
Change its implementation to select the appropriate row, decode its
`content_json`, and serialize it through `note_document_markdown()`.
`NoteText.name` stays the original note's name.

```text
prefer_compressed and the note has a compressed row
    -> decode content_json of the compressed row
otherwise
    -> decode content_json of the original row

selected document -> note_document_markdown() -> NoteText.text
```

This is the common conversion point for attaching notes and restoring missing
chat note copies. Request size checks apply to the resulting UTF-8 Markdown
text. The existing 256 KiB chat attachment limit therefore measures the same
kind of data as today.

The original document remains the input to a new compression request because
that caller uses `prefer_compressed = false`.

A compressed row whose BLOB fails zlib or document validation produces a
storage error. Fallback to the original applies when the compressed row is
absent. This distinction makes damaged saved data visible. `note_get()` also
decodes the compressed row to make `compressed_text`, so a damaged compressed
row also stops the original note from opening in the editor and in ChaWeb.

`session_notes.text` stores a chat-owned text snapshot. Migration preserves
those snapshots. A later attachment or missing-copy restoration uses the new
document reader. Existing session text size and prompt construction code
continues to consume `NoteText.text`.

## Note API and dialog

The note API does not change. `note_get()` reads the compressed row,
serializes its document with `note_document_markdown()`, and returns the
result in the existing `NoteContent.compressed_text` field. `notes.get`,
`resources/dto.yaml`, the TypeScript types, the response guards, and the
compressed-note dialog stay as they are. ChaWeb's `{ item, document }`
response continues to return the original document.

## Existing vault migration

### Entry point

Keep database version 2. Older CHA builds are out of scope, so the format
change needs no new version number. Convert the table in `open_database()`,
the existing runtime upgrade point, before its schema transaction. Desktop
and daemon startup both use this path, so the conversion runs before sessions
open, note operations are admitted, or sync workers start. An unopened note is
converted as part of its vault.

A `compressed_text` column in `note_items` identifies an unconverted table.
Check for it inside the conversion's immediate transaction. When two
processes open the same vault, SQLite's write lock serializes them, and the
second process finds the converted table and skips the conversion.

Vault copies, password changes, R2 uploads and downloads, and configuration
imports continue to handle the database file as they do today. A copied or
downloaded vault with the old table converts at its first runtime open.

### Transaction

Use SQLite's table rebuild procedure, because the STRICT table loses the
`compressed_text` column and the annotation CHECK constraint refers to it.

1. Disable foreign-key enforcement on the runtime connection. This pragma has
   no effect inside a transaction, so set it before the transaction starts.
   Enforcement stays off during the rebuild so the table drop cannot cascade
   into audio, summary, or other dependent rows.
2. Start an immediate write transaction. If `note_items` does not exist or has
   no `compressed_text` column, commit and continue with the normal open. The
   existing `create_note_items_schema()` then handles a missing table or the
   old TEXT layout as it does today.
3. Record the current `sqlite_sequence` high-water value for `note_items`.
4. Create `note_items_new` with the full current schema, including the
   `compressed` column, its CHECK, and the new annotation CHECK. Keep
   self-references pointed at the final `note_items` table name.
5. Copy all rows with their original IDs and `compressed = 0`. Copy each
   column that both tables have, by name. Some existing tables lack
   `changed` or `forum_id` and have their columns in a different order. The
   missing columns take their defaults. Copy original document BLOBs as
   BLOBs.
6. Set the sequence value of `note_items_new` to at least the recorded value.
   After the copy, SQLite sets it to the highest copied ID, which is lower
   when deleted notes had higher IDs.
7. For each old note row with a non-NULL `compressed_text`, insert a
   compressed row into `note_items_new`. It has `parent_id` set to that row's
   ID, its name, `kind = 'note'`, `compressed = 1`, and `content_json` set to
   the zlib of the compact JSON of `note_document_from_text()`. NULL creates
   no row. An empty string creates a row. The new rows get IDs above every old
   ID. The old schema also allows `compressed_text` on folder rows. The
   conversion ignores such a value, because only a note has a compressed
   version.
8. Drop the old table, rename the new table to `note_items`, and recreate all
   note indexes. The rename keeps the sequence value.
9. Run `PRAGMA foreign_key_check` and commit. Related tables continue to
   refer to the same note IDs.
10. Restore foreign-key enforcement, also after a failure.

Any failure rolls back the new table and the converted rows. Report the
affected note ID and a safe error reason. The next open retries from the
intact old table. Keep format conversion separate from content mutation: it
creates no sync records, the new rows have `changed = 0`, and the existing
`changed` flags and sync queue order stay as they are.

The conversion uses the existing SQLCipher connection and vault password, so
protected vaults follow the same transaction.

## Folder exports and imports

The export file's `note_items` table replaces `compressed_text TEXT` with
`compressed INTEGER`. The export query already walks the tree through
`parent_id` and skips only ANNOTATIONS rows, so compressed rows follow their
notes into the file. New exports keep the existing `CHAF` application ID.

The importer selects the layout by the column names in the export's
`note_items` table. For a new export, it copies each note's compressed row
right after the note, with the note's new ID as `parent_id`, and validates its
document. For an old export, it creates the compressed row from the
`compressed_text` value of a note row with the same helper as vault
migration, and ignores the value on a folder row. Perform this inside the
destination import transaction, preserving the source export file and the
existing parent-before-child ID remapping.

Emit imported-note sync records only for folders and original notes. Do not
add compressed rows to the importer's list of created items: a `note.create`
record for a compressed row would have a path through its note, and the
receiver rejects a path whose parent is not a folder. The `note.create`
record of each original note carries its compressed content. Round-trip tests
cover original content, compressed content, audio, and read-only flags in both
export formats.

## D1 synchronization

The compressed version travels as a field of its original note's records, not
as records of its own. The receiver's `apply_parent()` requires folders above
a note, and the parent of a compressed row is a note.

Use `compressed_content` for the new wire value. It is base64 of the
compressed row's `content_json` BLOB, matching the existing `content` field
for the original document. `note.create` and `note.update` include either that
string or JSON null. `note.compress` includes a document string.

Update both `stored_note_fields()` in `note_storage.cpp` and `note_fields()` in
`sync_storage.cpp` to read the compressed row. When applying a record, decode
and validate the document. Then replace the content of the compressed row,
insert a compressed row, or delete it when the value is null.

The new receiver also accepts legacy `compressed_text` records. When a record
has `compressed_content`, validate and use it. Otherwise convert a legacy
`compressed_text` string using the shared literal-text helper, and treat null
as absence. For create/update records with neither field, retain the existing
missing-field behavior: the note has no compressed row afterwards.

Already queued records keep their existing order and payload. The new
receiver converts legacy records from either the local queue or D1 when they
are delivered. Record splitting and assembly continue to carry complete JSON
records, so no queue migration or new transport is needed.

## Implementation sequence

1. Add the text-to-document helper and reuse the document codec. Verify the
   text preservation and the document's validity for `notesExtensions()`.
2. Add the schema change and the transactional table conversion to
   `open_database()`.
3. Change `note_list()`, `find_item()`, `has_compressed`, `note_get()`,
   `note_get_text()`, both compression write paths, and summary replacement.
   Include every SQL reference to the old column.
4. Update folder export and import, the folder store upload, and both sides of
   D1 serialization, with legacy input support.
5. Run migration and regression validation before enabling writes in a
   delivered build.

## Validation

| Case | Required result |
| --- | --- |
| Fresh compression | A row with `compressed = 1` and `parent_id` equal to the note exists. Its `content_json` decodes to a valid Notes JSON document with the provider's text. The tree list does not show it. `has_compressed` is true. |
| Recompression | The content of the same compressed row is replaced. Its ID does not change. Source changes discard stale results. |
| Literal text fixtures | Unicode, tabs, quotes, Markdown markers, CRLF, and repeated blank lines survive JSON, zlib, and text serialization. The document loads with `notesExtensions()`. |
| Model request | Captured note attachment text contains the serialized document. The size check runs on this text. |
| Deletion | Deleting a note, or a folder that contains it, deletes the compressed row. |
| Rename and read-only | Only the original row changes. |
| Summary replacement | The compressed row is deleted. |
| Operations by ID | `notes.get`, ChaWeb, a typed chat item reference, note save, rename, read-only, and scroll position report a compressed row's ID as not found. |
| Compressed write on a non-note ID | The write helper reports that the note was not found and creates no row. |
| ANNOTATIONS | A compressed row never becomes an annotation source. |
| Unconverted vault | Each non-NULL `compressed_text` becomes a compressed row with an ID above the old high-water value. Original IDs, hierarchy, content, summary links, audio, flags, and chat snapshots are preserved. |
| Old table without `changed` or `forum_id` | The conversion copies the existing columns by name. The missing columns take their defaults. |
| Table without `compressed_text` | The conversion skips it. The existing schema code handles a missing table or the old TEXT layout as today. |
| NULL and empty legacy values | NULL creates no compressed row. An empty string creates a compressed row with one empty paragraph. A value on a folder row creates no row. |
| Protected vault | Conversion succeeds with the existing password and leaves a valid protected database. |
| Failed conversion | Rollback keeps the old table and content. Foreign-key enforcement is on again. The next open retries. |
| Second open and competing opener | The column check inside the immediate transaction skips a converted table. |
| Folder transfer | Both legacy and current exports import correctly. Compressed rows get the new IDs of their notes as parents. With sync connected, the import queues records only for folders and original notes, and each note record carries its compressed content. A failed import leaves the destination transaction intact. |
| Folder store upload | Only original notes are uploaded. |
| D1 queue | Legacy and new create/update/compress records apply correctly, including split records. The receiver replaces, inserts, or deletes the compressed row. |
| Copied and downloaded vaults | A copy or download with the old table converts at its first runtime open. |
| Existing native dialog | The dialog shows the text made from the compressed document. The response guards do not change. |
| ChaWeb reader | Its existing `{ item, document }` response still loads the original document. |
| Damaged compressed row | Chat attachment and `notes.get` of the original report a storage error. |

Extend the existing note storage, workspace database, sync, bridge, session,
and editor suites with focused regression fixtures for these boundaries.
Use temporary vaults and fake provider responses so validation can inspect
the exact saved bytes and model-bound text.
