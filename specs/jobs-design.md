# Persistent jobs for summarization and page imports

Proposed technical design for desktop Notes · November 21, 2026

## Requirements for this stage

1. When the desktop window and vault are ready, continue unfinished work automatically, including when CHA opens into chat. Quit, renderer reload, and vault switch interrupt work. Reopening the same vault continues it.
2. Accept a second **Summarize** into a FIFO queue. Run one summary job per vault, with up to three note lines inside it. Page and YouTube imports can run alongside summarization and alongside imports for other documents. Allow one unfinished import per destination document.
3. Preserve each committed summary output. An interrupted import may retrieve the page, format it, and load pictures again. Repeating a paid formatter call before the note was saved is acceptable. A saved article must appear in the note exactly once for that submission.
4. Keep only unfinished jobs in SQLite. Events provide history. Completed and definitely failed imports release their destination immediately.
5. Clear unfinished jobs from the R2 upload copy and from a newly cloned local vault. The source vault keeps its work.
6. Pause, Resume, Cancel, Retry, and a Jobs view belong to a later stage. In this delivery quitting or switching vaults interrupts provider work only while the vault is closed. An accidental Overwrite job resumes and can incur further charges when the vault reopens.

These decisions supersede the earlier busy rejection and cancel-on-exit behavior. The scope and visible Event changes are recorded in the [jobs review](jobs-review.md). The existing output and folder assembly rules come from the [background summarization design](backsum-design.md) and [per-note L3 design](L3-design.md).

## One row per unfinished command

Use one vault-local `note_jobs` table. A summary row carries the existing planner's list with the remaining levels for each note. An import row carries its source and destination. The renderer runs the existing operation for each kind. Chat **Save summary** and local file imports keep their existing lifecycles.

```sql
CREATE TABLE IF NOT EXISTS note_jobs (
    id          INTEGER PRIMARY KEY AUTOINCREMENT
                CHECK (id > 0 AND id <= 9007199254740991),
    kind        TEXT NOT NULL CHECK (kind IN ('summarize', 'import_page')),
    target_id   INTEGER NOT NULL CHECK (target_id > 0),
    document_id INTEGER UNIQUE CHECK (document_id > 0),
    options     TEXT NOT NULL,
    remaining   TEXT,
    CHECK ((kind = 'summarize' AND document_id IS NULL AND remaining IS NOT NULL)
        OR (kind = 'import_page' AND document_id IS NOT NULL AND remaining IS NULL))
) STRICT;
```

The lowest summary ID owns the summary slot. The integer ID also distinguishes an old callback from a later submission after the table has emptied. `UNIQUE(document_id)` reserves an import's physical destination while permitting any number of summary rows with a null document ID. Keep IDs independent of note deletion so the normal failure path can report a missing target. Define the schema in `create_note_items_schema`, called inside the vault-open transaction. Older vaults acquire an empty table. [Note schema and existing ID convention](../src/storage/note_storage.cpp) · [Vault-open transaction](../src/workspace/workspace_config_store.cpp)

Validate new submissions as today. On recovery read the fields required for this operation. Ignore obsolete or unused settings with a warning log. Invalid data required for execution follows the operation's failure path. Only the active plan carries names needed for headings or Event labels. Events keep their existing retention policy. [Events storage](../src/storage/event_storage.cpp)

## Summarization

**Admission.** Flush the active editor and reuse the existing native planner, option validation, provider checks, and no-work errors. Preserve the 4 KiB minimum, exclusion and direct-child rules, and read-only note eligibility. Store the selected provider ID, effort, Overwrite choice, and admitted target label. The plan records admitted note IDs and names, each note's due levels, and whether a folder Summary remains due. For a folder build, keep every contributor in the plan until the overview settles, including children with all levels cached or already completed. This preserves its headings, membership, and tree order across a restart. [Native planner](../src/notes/notes_operations.cpp) · [Renderer scheduler](../webapp/src/notes/summaries.ts)

For example, a folder plan can contain `{"notes":[{"id":12,"name":"A","levels":[]},{"id":17,"name":"B","levels":["l3"]}],"folder_summary":true,"start_event_due":false}`. After B's L3 commits, both lists of due levels are empty. The folder flag still requires one overview using A and B.

**Queue and Events.** Under the store lock, insert the job and its admission Event in one transaction. An immediately runnable summary writes the existing “Summarization started for …” Event with the admitted note count. A waiting summary closes the dialog on acceptance and writes, for example, `Summarization queued for “Name” (folder 8).` Mark that waiting plan's start Event as due. When the renderer begins the head job, native code checks that it is the lowest summary ID, emits any due start Event and clears that marker together. A resumed head can therefore keep its place without writing a second start Event. Use this path for an overview-only job as well. [Dialog](../webapp/src/components/NoteSummaryDialog.tsx) · [Existing Event formatting](../src/notes/notes_operations.cpp)

**Execution and checkpoints.**

1. Run the admitted lines in existing tree order. Within each line process Compressed, L1, L2, and L3 in order. Preserve provider preparation, per-step reads of the saved original and prompt, Flex retries, line cost logging, and the three-line limit. Immediately before generating a version with Overwrite off, check whether it now exists. An earlier FIFO job may have supplied it. If so, remove that due level without a provider call. Overwrite on generates the requested replacement even if an older version exists.
2. Settle each step in a short native transaction. Read the **current** row, verify the job and pending step, apply the existing output write, and remove just that step. Updating the latest plan under the store lock allows three lines to finish out of order without restoring a sibling's completed work. Reuse the transaction bodies of `note_save_compressed`, `note_commit_level`, and `note_commit_folder_summary`, including existing Events, sync changes, and audio/annotation effects. Preserve existing transaction-owning entry points and the renderer's short editor replacement guard. Network calls and editor waits happen outside the transaction. [Output writes](../src/storage/note_storage.cpp) · [Editor guard](../webapp/src/components/App.tsx)
3. On a definite step failure, write its existing failure Event and remove the step together. If a required child L3 fails while the folder build is due, also clear the folder flag and record the existing “Could not build the Summary” Event once. Other children continue. A failed L2 alone permits the overview. When child work settles and the folder flag is still set, read every admitted child's actual saved L3 and build the overview from those same rich sections in tree order. An unreadable or missing section fails the folder build and preserves the previous Summary. [Folder behavior](L3-design.md) · [Assembly and conversion](../webapp/src/notes/summaries.ts)
4. When the final level or folder build settles, including a skip or failure, delete the row and append the existing finish Event in the same transaction. The renderer then starts the next summary. A crash before an output transaction commits leaves that step due. A crash after commit advances past it, including for Overwrite. A stale completion for a missing job or settled step has no effect.

## Web page and YouTube imports

**Admission and scheduling.** Store the submitted URL and loaded text mode in `options`, the logical note in `target_id`, and the actual displayed row in `document_id`. Keep the target label for a failure Event if the note is later deleted. Preserve both URL entry points. For **New note from URL**, create the normally named empty note and its job in one transaction. Submitting against an existing note requires an editable target. A duplicate import to the same document follows the existing busy behavior. Keep the same-window guard for a local file import to that document. Other document imports can progress immediately, subject to existing bridge and picture capacity. [New-note entry point](../webapp/src/components/NotesTree.tsx) · [Loaded mode and URL submission](../webapp/src/components/NotesScreen.tsx)

| Step | Execution and recovery |
| --- | --- |
| Retrieve and optionally format | Run the existing `notes.readPage` path. It recognizes a YouTube URL before or after a redirect and uses transcript formatting. An ordinary page uses article formatting only when the long-paragraph check requires it. Preserve current provider selection and the 12 MiB limit. Before the final save, restart runs this request again from the submitted URL, so the source can change and the formatter can be billed again. |
| Prepare and append | Reuse the existing Markdown sanitization, picture loading, and document size handling. Wait for the short note-action guard, settle any opening editor, and verify the captured destination is still editable. A fallback originally displayed as full text continues to target the original. A removed version fails the import. Merge into the latest available document using the existing editor helper. |
| Save and finish | Save the merged document through the transaction body of `note_save` and delete the matching import row in **one transaction**. Verify the job, logical note, and document ID before writing. A crash leaves either the pending URL or the saved append. A restart cannot append a committed import twice. |

Use the existing freeze and in-flight-save handling for an open target. Flush pre-import user edits first and prepare the merged document in a detached editor while the guard is held. Send only the job-aware final save for the merged content. An ordinary autosave of that content could otherwise commit it while leaving the job resumable. Refresh the live buffer after confirmation, or restore its pre-import draft on definite failure. An offscreen document uses the same final save. Concurrent incoming D1 changes retain today's last-write behavior. [Current import merge and metadata](../webapp/src/notes/useNoteImports.ts) · [Editor save tracker](../webapp/src/notes/useNoteSave.ts) · [Native note save](../src/storage/note_storage.cpp)

After the save, apply today's conditional rename and Make read-only calls separately, with their existing warnings. A crash between save and metadata can leave the old name or editable flag. The article is already durable. On a definite import failure, delete the row and write the approved Event `Could not import <URL> into “Note”: <reason>.` together, using a public reason and existing punctuation conventions. Keep today's prompt error and retry behavior if the originating prompt is still present. Sending again after failure starts a fresh import. An interrupted operation leaves its row for recovery. [Reader and formatter](../src/notes/notes_operations.cpp) · [Import coordinator](../webapp/src/notes/useNoteImports.ts)

## Desktop lifecycle and storage errors

The root App reads unfinished jobs when the bridge and vault become ready, on return from maintenance, and after admissions and completions that can expose more work. Run the lowest summary ID and eligible imports. Keep live job IDs in memory so overlapping callbacks in the same renderer cannot launch an operation twice. When the bridge is at capacity, leave unstarted work pending until a completion frees capacity. Existing UI progress comes from these live calls. The database lease, connection, and vault epoch delimit execution. [Desktop App](../webapp/src/components/App.tsx) · [Vault-open lease](../src/workspace/workspace_config_store.cpp)

Add one bridge read for unfinished jobs. Extend the typed start, summary step, and import paths to carry the storage-assigned job ID through settlement. Let native code choose the current row and verify pending work on every mutation. The renderer drops stale results using its existing connection and epoch checks. Native provider workers return results or errors to the current coordinator. Only a request from the live connection in the admitted epoch can settle an item. Signal live requests and retry waits on close, vault switch, and shutdown. Include job-owned page reads in that cancellation path, which currently handles summary calls. Interruption leaves the row intact and writes no summarization cancellation Event. [Bridge invalidation](../src/bridge/bridge_router.cpp) · [Connection cleanup](../src/app/media_operations.cpp) · [Vault maintenance](../src/app/vault_operations.cpp)

An uncertain admission reply triggers a read of pending work. Do not automatically resend Start or issue the old unconditional Finish. A fresh user submission creates a new job, even if an earlier submission may have committed before its reply was lost. For an uncertain commit, use the existing ordered bridge read and reload the affected document before releasing the editor guard. Recovery acts only on work still present. If a note write definitely fails and rolls back, use a separate transaction to record the failure and retire that item, then continue independent work. If even that transaction cannot commit, log the storage error and leave the item pending. End its dispatch pass without immediately sending another provider request. The next normal vault reopen can read the durable state. A later summary still waits behind an unsettled head.

## Vault copies

Delete jobs **from the temporary upload database** in `R2DatabaseUpload::upload`, alongside the audio deletes and before `VACUUM` and checkpoint. On local clone, clear jobs in the destination transaction that calls `sync_clear_outgoing`. Handle older copies that lack the table. A byte-for-byte vault copy retains valid note IDs. Clearing the rows prevents a second vault from executing the same paid work. Normal R2 installation therefore receives a clean queue. Ordinary reopen, rekey, and sync disconnect preserve the local queue. [R2 upload cleanup](../src/app/r2_database_transfer.cpp) · [Local clone cleanup](../src/app/vault_operations.cpp)

## Acceptance checks

| Case | Observable result |
| --- | --- |
| Older vault and copies | Opening an older vault creates an empty table and preserves its notes. Seed pending work, make a local clone and an R2 upload copy, and verify only the source retains the rows. |
| FIFO and folder recovery | Queue B behind A. Complete child lines out of order, reopen into chat after a saved L3, and verify A finishes before B, including an Overwrite-off skip in B. A's cached and completed contributors all enter the overview. A failed required L3 suppresses it across a restart. Check provider counts, one queued Event for B alone, and one start and finish Event for each job. |
| Import recovery | Run a normal page and a YouTube import to different documents alongside A. Interrupt a formatted import before its final save and reopen. Retrieval and formatting may repeat. Interrupt after the save but before its reply or rename and verify one append and no remaining row. A definite failure releases its destination and writes the approved Event. |
| Transaction and lifecycle boundaries | Roll back an output write with its progress update, then commit it and drop the reply. Verify that note content and remaining work agree in both cases. Switch vaults with a held callback and overlapping note IDs. Only reopening the source continues its pending work. |

Exercise these cases with the existing fake providers and reopened vault databases. Keep the existing scheduler and editor behavior tests. [Notes service tests](../tests/notes/unit_notes_service.cpp) · [Storage tests](../tests/storage/unit_note_storage.cpp) · [Summary scheduler tests](../webapp/src/notes/summaries.test.ts) · [Import coordinator tests](../webapp/src/notes/useNoteImports.test.ts)
