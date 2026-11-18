# Saved L3 notes and folder Summaries

This design covers desktop CHA Notes. Each eligible note can have a saved L3 version. **Summarize** on a note can create that version independently. **Summarize** on a folder uses the saved L3 of each eligible child to build the folder's **Summary**.

Two product decisions govern the design. Selecting **L3** on a folder remains one action. It fills missing L3s, reuses existing ones, and rebuilds the Summary. Selecting **Overwrite** also regenerates the existing L3s. Each section of the folder Summary copies the entire saved L3, including text edits, formatting, highlights, and comment paragraphs.

## Current behavior and proposed change

Compressed, L1, and L2 are hidden child rows of the original note in SQLite. The editor selects a version by mode and saves through its document ID. L3 currently exists only during a folder batch. The desktop coordinator requests a short section from each original, keeps the Markdown in memory, sends the sections to the overview prompt, and commits one visible Summary note. Selecting L3 again repeats every section request. [Note storage](../src/storage/note_storage.cpp) · [Batch planning and inference](../src/notes/notes_operations.cpp) · [Desktop coordinator](../webapp/src/notes/summaries.ts)

The new L3 is the short, usually title-free text produced by the existing **Summary L3 section** prompt. It reads the saved original at the start of its step and works independently of Compressed, L1, and L2. The existing overview prompt still writes the opening of the folder Summary. Existing vault prompt customizations continue to apply. [Section prompt](../resources/summary-l3-section-prompt.md) · [Overview prompt](../resources/summary-l3-overview-prompt.md)

| Selection | Overwrite off | Overwrite on |
| --- | --- | --- |
| Note, L3 checked | Create a missing L3. Skip an existing L3. | Generate and save a new L3 for this note. |
| Folder, L3 checked | Generate missing child L3s. Build the Summary from every eligible child's saved L3. | Regenerate every eligible child's L3. Build the Summary after those writes succeed. |
| Folder, L3 unchecked | Run only the selected note versions. | Replace only the selected note versions. |

For a folder with three eligible children, two with saved L3s, an L3-only run with Overwrite off makes **one section request and one overview request**. A second run makes **one overview request**. An L3-only run with Overwrite on makes **three section requests and one overview request**. These counts assume successful first attempts. Running L3 on one child never rebuilds the parent Summary. Editing an original or an L3 has no automatic regeneration side effect.

This revises the L3 and Overwrite rules in the [summarization controls design](backctrl-design.md) and the section-failure rule in the [concurrent summary design](summary-ext.md). The [background batch lifecycle](backsum-design.md) still applies.

## Approved desktop UI

On November 18, 2026, the user approved the desktop UI shown below, including the **L3** menu option, the single-note **L3** checkbox, and the **Overwrite** defaults for notes and folders.

Add **L3** after **L2** in the existing **Note text mode** menu. Add the L3 output to the single-note **Summarize** dialog. The sketch shows the open mode menu at left and the default single-note dialog at right.

```text
┌────────────────┐
│ Full text      │      Provider  [ My provider ▾ ]
│ L1             │      Effort    [ Medium      ▾ ]
│ L2             │
│ L3           ✓ │      ☑ Compressed   ☑ L1   ☑ L2   ☑ L3
│ Compressed     │      ☑ Overwrite
└────────────────┘
                                      [ Cancel ] [ Start ]
```

The folder dialog has the same four outputs checked and **Overwrite** unchecked. Each opening uses the provider and effort from Notes settings. Closing setup starts no work.

Use one layer of the existing stacked-layer icon for L3. Preserve the radio-item keyboard behavior, checked state, focus, and session-wide mode selection. The tooltip reports the actual display, for example “L3 mode; showing L3” or “L3 mode; showing full text”. Include L3 in the existing version dot and tree change detection. A new L3 has no separate tree entry. [Mode menu](../webapp/src/components/NotesScreen.tsx) · [Icons](../webapp/src/components/Icons.tsx) · [Dialog](../webapp/src/components/NoteSummaryDialog.tsx) · [Tree index](../webapp/src/notes/tree.ts) · [Notes tree](../webapp/src/components/NotesTree.tsx)

Selecting L3 opens the saved version when present. Otherwise the editor shows the full text, as it does for a missing L2. Save and scroll use the ID of the displayed document. The owner's read-only setting governs manual edits. A read-only original still permits generating an L3 and adding highlights and comments under the existing editor rules. L3 has the same audio eligibility as L2. Folder EPUB export in L3 mode follows L2 mode, including the ` (L3)` title and filename suffix, per-chapter full-text fallback, and the existing empty-level error when no chapter has an L3. [Editor save path](../webapp/src/components/NotesScreen.tsx) · [EPUB export](../webapp/src/notes/epubExport.ts)

## SQLite storage and upgrade

Use one `note_items` row with `kind='note'`, `parent_id=<original note ID>`, `compressed=1`, and `level=3`. Store the zlib-compressed, compact ProseMirror JSON document in `content_json`. The existing unique index on `(parent_id, level) WHERE compressed=1` permits one L3 per owner. The folder Summary continues to be a visible direct child of the folder, identified by `folder_summary=1`. An ordinary note named “Summary” retains its identity.

For fresh vaults and older vaults acquiring their first `level` column, define the allowed values as `0, 1, 2, 3` and keep the rule that original rows have level 0. Existing level-aware vaults already have a column-level CHECK that rejects 3. Detect CHA's old `level IN (0, 1, 2)` expression in the `note_items` table definition in `sqlite_schema`. Checking for the column alone cannot distinguish these vaults.

In `create_note_items_schema`, widen the old constraint inside the caller's existing schema transaction. Use the new `level_column` definition for the ADD, then run the existing index-creation routine before commit. The SQL below spells out the affected index.

```sql
DROP INDEX IF EXISTS note_items_version_parent;
ALTER TABLE note_items RENAME COLUMN level TO old_level;
ALTER TABLE note_items ADD COLUMN level INTEGER NOT NULL DEFAULT 0
    CHECK (level IN (0, 1, 2, 3) AND (level = 0 OR compressed = 1));
UPDATE note_items SET level = old_level WHERE old_level <> 0;
ALTER TABLE note_items DROP COLUMN old_level;
CREATE UNIQUE INDEX note_items_version_parent
    ON note_items(parent_id, level) WHERE compressed = 1;
```

This changes the constraint in place with foreign keys enabled. Rows keep their IDs, blobs, parents, flags, and scroll positions. Links, audio, and the AUTOINCREMENT high-water mark remain valid. A failed transaction restores the old column, index, and data. Reopening the upgraded vault skips this branch. Keep the existing earlier migration for `compressed_text` and the workspace database identity. Existing folder Summaries remain readable, including any user edits. Create the first per-note L3 on demand. Old Summary headings cannot reliably identify source IDs or distinguish generated text from later edits. [Schema and version-row helpers](../src/storage/note_storage.cpp) · [Vault open sequence](../src/workspace/workspace_config_store.cpp)

Extend `NoteTextMode`, `note_text_level`, the indexed existence query, and `NoteItem::has_l3`. Append the computed flag to the positional SELECT and its reader together. Extend `note_commit_level` to accept 3. Use `write_version_row` so replacement preserves the L3 document ID and reading position. Commit the body, success Event, and outgoing level record in one transaction. The existing owner checks reject folders and generated Summary notes. Parent deletion cascades to L3. Version edits use `note_save` and mark the owner changed for sync. L1 audio invalidation stays specific to level 1. [Storage interfaces](../src/storage/note_storage.h) · [Storage implementation](../src/storage/note_storage.cpp)

## Admission and desktop bridge

Apply the existing 4 KiB minimum to saved original Markdown, including when reusing an L3. A folder run admits ordinary direct child notes, including read-only notes. Skip generated summaries, excluded children, nested notes, and ANNOTATIONS. A single-note run ignores that note's exclusion flag. Keep the existing managed-folder restriction, selected provider and effort, Flex rules, retries, one active batch, owner connection, and vault epoch.

Keep the `notes.summaryStart` request and its `{notes: [{id, name, levels}]}` reply. In the reply, `levels` lists only versions due for **generation**. For a folder with L3 checked, include every eligible child exactly once, even a child with `levels: []`. Such a child contributes its saved L3. In every other case omit notes with no work. Pass `source.kind === 'folder' && options.levels.includes('l3')` from the accepted start to the coordinator. This supplies the folder-build decision even when every child already has an L3. The native plan stays authoritative for the child list and due work.

For example, with L3 selected and Overwrite off, this plan reuses A and generates B. Both enter the overview in tree order.

```json
{"notes":[{"id":12,"name":"A","levels":[]},{"id":17,"name":"B","levels":["l3"]}]}
```

Allow an empty **reply** `SummaryTarget.levels` in the DTO and runtime guard. The start **request** must still have at least one distinct valid selection. A folder without eligible children gets the existing empty-folder error. A note or non-L3 folder with all its selected outputs already present gets the existing no-work error. A folder with cached L3s still has overview work and requires a valid provider. Use the admitted child count for its start Event.

| Operation | Contract |
| --- | --- |
| `notes.get(id, "l3")` | Same reply and full-text fallback as L2. A saved version has `document_id != id`. |
| `summaryGenerate(noteId, "l3_section")` | Return Markdown from the saved original using the existing section prompt and provider request path. |
| `summaryCommit(noteId, "l3_section", document)` | New accepted part for commit. Write level 3 and emit the note-level result. Conversion failure uses `summaryAbandon(noteId, "l3_section")`. |
| `summaryGenerate(folderId, "l3_overview", text)` | Existing overview operation. Supply the ordered text derived from the saved L3 bodies. |
| `summaryCommit(folderId, "l3", document)` | Existing final folder Summary write. `summaryAbandon(folderId, "l3")` records a folder assembly failure. For a failed required child, permit optional `source_id` on this folder abandon request to identify it. |

Keeping the existing folder commit name avoids changing its meaning for callers. In the App editor guard, map `l3_section` to note text mode `l3`. The wire part `l3` continues to select the folder Summary and ANNOTATIONS refresh. Update desktop DTOs, generated types, strict response guards, fixtures, and native dispatch together. Add `has_l3` to the shared NoteItem serializer (`to_json`), the `NoteItem` DTO, and the shared strict `isNoteItem` guard, so that list and mutation replies carry it. ChaWeb uses the same serializer and guard. Its replies gain the field, and its NoteItem fixtures and tests add it. ChaWeb does not read the field. On November 18, 2026, the user chose this shared field instead of a separate desktop projection. [DTO](../resources/dto.yaml) · [Desktop dispatch](../src/bridge/notes_dispatch.cpp) · [Background dispatch](../src/bridge/bridge_router.cpp) · [Guards](../webapp/src/api/noteGuards.ts) · [Desktop client](../webapp/src/api/nativeClient.ts) · [ChaWeb adapter](../src/daemon/chaweb_adapter.cpp)

## Execution and folder assembly

1. Flush pending editor changes during admission as today. Sort the admitted targets once with `compareNoteSiblings`. Run up to three note lines. Skip line start/finish and cost logging for targets with no due steps. On each active line process Compressed, L1, L2, then L3, subject to the plan. Convert an L3 Markdown reply with `markdownNoteDocument` and commit it through the same editor guard as L2. Store only its body. CHA supplies the chapter title when building the folder Summary.
2. Let all scheduled versions finish independently. A failed L1 or L2 does not prevent an L3 or overview. A failed L3 generation, conversion, or write makes the folder build incomplete. Continue the other child L3s so the next run can reuse their successes. Track that failure explicitly across the existing `step` wrapper, which currently swallows ordinary errors. A failed requested overwrite leaves the earlier L3 readable and suppresses this folder build even though a row exists.
3. After successful required L3 writes, read each admitted child's saved L3 **once** in tree order with `getNote(child.id, 'l3')`. Use the same read path for newly generated and reused versions. A read error prevents replacement. A full-text fallback, where `document_id` equals the child ID, also prevents replacement, because the child has no L3. The final document limit in step 5 covers size.
4. From each read, retain its saved top-level content nodes. Derive the overview input from that same document using the existing Tiptap `generateText` and `notesExtensions()` pattern. Use paragraph breaks and include comment text. Prefix each body with `# <admitted child name>` and join sections with blank lines. Only the overview is inferred at this stage. The provider receives text. Formatting and embedded note content remain available in the saved nodes.
5. Assemble the final document as a literal H1 containing the folder name, the converted overview blocks, then one literal H1 per admitted child followed by **all of its saved L3 blocks in order**. Copy those JSON nodes directly. A Markdown round trip can lose highlights, comment colors, embedded images, or literal punctuation. Keep the user's own headings and marks. Enforce the existing 12 MiB UTF-8 overview-input, conversion, and compact final-document limits. On success commit once through `note_commit_folder_summary`. It chooses the managed Summary by its flag and handles name collisions.

The overview and copied section for a child refer to the same saved read. An edit saved before that read is included. An edit saved afterward belongs to the next folder build. Names and membership come from admission. No editor or database lock spans provider work. Copying creates independent content in the folder Summary. Editing the copy cannot write back to the source L3. [Coordinator](../webapp/src/notes/summaries.ts) · [Conversion](../webapp/src/notes/summaryImport.ts) · [Editor and text extraction](../webapp/src/components/App.tsx)

### Editor, annotations, and failures

When replacing a displayed L3, freeze only for the local commit, settle in-flight saves, and retire or restore the old buffer according to the outcome. When the editor shows full text as the L3 fallback, save that original before switching the view to the new version. Retain the existing pending-load, navigation, focus, and lost-reply handling. In particular, a queue barrier after a lost commit reply establishes ordering only. Report an unconfirmed required L3 write to the coordinator as incomplete after editor recovery. Finding an older L3 at that ID cannot confirm the write.

A successful folder replacement resets the previous Summary's edits and audio under the existing rules. Its new annotations include the copied L3 highlights and comments. The storage commit currently rebuilds only an existing ANNOTATIONS row. Pass the newly assembled Summary as the creation document to `rebuild_folder_annotations(database, folder_id, true, &document)` so the first annotated Summary can also create that row. The aggregate reads visible originals, including the folder Summary. Its links point to the copy in the Summary. Saving a hidden L3 alone does not add entries to the aggregate.

Use the existing result and batch Events. Label the note-level L3 part “L3”, as for L1 and L2, for example “L3 ready for …” and “Could not make L3 for …”. Note-level L3 Events do not say “The Summary note was not changed.” The folder abandon Event reports the Summary outcome. Write the L3 success Event inside the same transaction as the body. Reuse produces no generation event or line cost. Report a note-level failure against its owner. If a required child fails, abandon the folder build once with the affected child ID, for example “Could not build the Summary for ‘Book’ because the L3 for ‘Chapter 2’ (note 17) could not be used.” The ID is sufficient if the child was deleted. This also covers an unavailable saved section that made no provider call. Overview inference and final-commit errors already have native failure Events.

A failed overview, invalid final document, or failed final write preserves the previous folder Summary, its annotations, and audio. Previously committed child versions survive failure or cancellation. Overwrite off retries missing versions and reuses any earlier L3 preserved by a failed replacement. Select Overwrite to retry that replacement. Finish, owner disconnect, vault switch, and exit follow the existing lifecycle and epoch checks.

## Sync and transfer

- **D1 generation.** Extend `note.level` to encode and apply `level: "l3"` with the same validated base64 compressed-document payload as L2. A generated L3 does not clear original or L1 audio on the receiver.
- **D1 snapshots and editor saves.** Add `l3_content` to both `stored_note_fields` (create/import) and `note_fields` (changed-note updates). Apply it the same way as `l2_content`: a blob writes L3, and a missing or null field clears it. Manual L3 edits use the existing owner changed flag and full update path, with its existing audio rules. [Record construction and apply](../src/storage/note_storage.cpp) · [Changed-note snapshots](../src/storage/sync_storage.cpp)
- **SQLite folder transfer.** The recursive export already includes version bodies, level, and scroll position. Extend the import validator to allow level 3. Preserve its checks for an original note parent, a unique owner/level pair, valid content, and a valid Summary link. Import maps each version to its new owner ID and preserves its reading position. A legacy export imports with the levels it contains. Whole-vault copies carry the rows with the database. [Export and import](../src/storage/note_storage.cpp)

## Implementation and acceptance

Implement storage, migration, sync, and transfer first. Then extend the desktop contract and batch planner, wire generation and assembly through the editor guard, and implement the approved desktop UI. Use the existing fake provider for deterministic replies and call counts.

| Case | Acceptance check |
| --- | --- |
| Upgrade and reopen | Open a pre-L3 vault with all old levels, an ID gap above live rows, audio on an L1, a linked Summary, flags, and scroll positions. Compare IDs and blobs, check foreign keys and the next ID, generate L3, then reopen twice. Cover the earlier layouts and rollback of an interrupted upgrade. An old folder Summary survives until a successful rebuild. |
| Single note | L3 alone requires one section call and zero overview calls. Default-on Overwrite replaces the same document ID. With Overwrite off, an L3-only repeat reports no work. Other versions, source text, and their audio survive. A missing L3 uses editable full-text fallback. Actual L3 saves and scroll changes address the version. |
| Folder planning | Exercise the three-child request counts above, an all-cached folder with no previous Summary, an L3-unchecked folder, and a mix of cached children with other due levels. Distinguish an empty eligible set from no work. Check the exact 4 KiB boundary, exclusions, nested folders, managed folders, session and folder summaries, and an ordinary note named “Summary”. |
| Edited and annotated input | Start with one saved L3 containing edited words, a highlight, a comment paragraph, links, and a list. Verify that the overview prompt includes the edited and comment text. Compare all copied section nodes to the stored L3. Change the L3 while the overview is held and confirm that both parts of this build used the earlier read. Check the visible ANNOTATIONS links and the source following a later edit to the copy. |
| Order and partial failure | Complete three lines out of order and check the three-line maximum, per-line order, literal title nodes, and tree order in the overview input and in the final Summary. Fail one required overwrite when an old L3 exists. Other L3s save, the old Summary survives, and no overview is sent. Separately fail an unrelated L2, a cached L3 read, conversion, overview, combined size, and final commit. Verify result Events and the next run's call counts. |
| Active editor and cancellation | Hold a provider reply while editing an L3 or its fallback original. Cover in-flight save, lost commit reply, navigation, owner disconnect, exit, and vault switch. Check saved text, focus, partial progress, one terminal batch Event, and absence of late writes to another vault. |
| Transfer and desktop integration | Round-trip L3 via D1 generation, a manual edit, explicit null, and a SQLite folder export/import. Reject level 4 and duplicate L3 rows. Verify the version dot, L3 picker and tooltip, single-note checkbox, read-only behavior, EPUB fallback, and the existing audio gates. |

Extend the existing [storage tests](../tests/storage/unit_note_storage.cpp), [sync tests](../tests/storage/unit_sync.cpp), [Notes service tests](../tests/notes/unit_notes_service.cpp), [bridge tests](../tests/bridge/unit_bridge_router.cpp), [coordinator tests](../webapp/src/notes/summaries.test.ts), [desktop App tests](../webapp/src/components/App.summary.test.tsx), and [native Notes smoke test](../tests/native/macos/test-note-summaries.py). Add `has_l3` to the NoteItem [wire fixtures](../tests/fixtures/wire/notes-contract.json) and the [ChaWeb adapter tests](../tests/daemon/unit_chaweb_adapter.cpp). Replace expectations that level 3 is invalid or that a failed section skips the remaining L3s.
