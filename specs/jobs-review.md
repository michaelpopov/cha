# Review of the persistent jobs design

Review of [jobs-design.md](jobs-design.md) · November 21, 2026

## Verdict

The design is careful, and its facts about the current code are mostly correct.
But it is much larger than the problem. It had no requirements section. It
reversed requirements that the user recorded earlier. Most of its mechanisms
protect against corner cases that one desktop window does not have.

The user answered the requirement questions on 2026-11-21. With these answers,
a much smaller design is sufficient. See [Recommended design](#recommended-design).

## Requirements confirmed by the user

These requirements come from the user (2026-11-21). They replace the exit and
queue rules in [backsum-design.md](backsum-design.md) (lines 11–12, 20 and 22).

1. When CHA opens, unfinished work continues automatically.
2. A second Summarize waits in a queue. One summarization runs at a time.
3. Pause, Resume, Cancel and Retry come at the next stage. This stage has none
   of them.
4. An unfinished page import continues after a restart. A repeated paid
   formatting call after a restart is acceptable.
5. Events are the only history. CHA keeps no finished jobs.
6. A vault switch works like a restart. The work stops, stays in that vault,
   and continues when that vault opens again.
7. CHA deletes all jobs in the R2 upload copy, where it already deletes audio.
8. CHA deletes all jobs in a local copy of a vault, at the existing cleanup
   point.

## Visible changes approved by the user

The user approved these changes on 2026-11-21:

- A Summarize that must wait closes the dialog and writes the Event
  "Summarization queued for “Name” (folder 8)." The existing
  "Summarization started for …" Event moves to the time when the job starts.
- When CHA quits, it no longer writes the summarization cancellation Event.
- An import failure also writes the Event
  "Could not import <URL> into “Note”: <reason>." An import that continues
  after a restart has no prompt to show its error.

## Findings

Each finding has a status from the user's answers.

### P1: Must fix

#### P1-1. The design has no requirements, and it reverses recorded requirements

**Status:** Resolved by the user's answers. The design must record them.

The design starts with a "Decision" (line 5), not requirements. It does not say
which behavior the user asked for. It changes these recorded requirements:

- Today, when CHA quits, the rest of the batch is cancelled and the next launch
  starts idle. The design continues the work automatically at the next launch
  (line 14).
- Today, a second Summarize is rejected and "never queued". The design puts it
  in a FIFO queue (lines 11 and 67).
- Today, the batch has no Cancel action. The design adds Pause, Resume, Cancel
  and Retry failed (lines 53–58).
- Today, a vault switch cancels the batch. The design keeps the job (line 130).
- The design adds a permanent job history and a later Jobs view.

**Impact:** The implementation would follow the author's choices, not the
user's. Automatic resume spends provider money without a new click.

**Fix:** Add a "Requirements" section with the confirmed requirements above.
Mark everything else as a proposal and link each mechanism to a requirement.

#### P1-2. The first delivery has no way to stop work

**Status:** The user moved all controls to the next stage. The effect below
stays in this stage as a known effect.

The implementation order builds storage and the runner first. The Jobs view
with Pause and Cancel comes later (lines 126 and 134). Today the user can stop
a summarization by quitting CHA or by switching vaults. After this stage, both
stop working: the job continues at the next launch and when the vault opens
again.

**Impact:** If you start Summarize with Overwrite on a large folder by
mistake, you cannot stop it. It continues to spend money across restarts.

**Fix:** None in this stage, by the user's decision. Add Cancel at the next
stage.

#### P1-3. The design is too large for one desktop window

**Status:** Still applies. The queue and the import resume are now real
requirements. Most of the other mechanisms have no source.

The design adds two tables, 6 job states and 8 task states, client UUIDs for
idempotent admission, job revisions, task attempt counters, an executor
attach/claim protocol with observers, coalesced change notices, two partial
unique indexes, a queue order, document reservations, a 12 MiB staged-page
checkpoint, a body token with a native re-merge, savepoints for metadata, and
about nine new bridge operations (lines 101–105).

Many of these mechanisms protect against several executors, lost replies, and
stale callbacks into another vault. The desktop app has one renderer. The
current code already rejects stale work with the vault epoch, and it releases
connection resources when the window reloads
([bridge_router.cpp](../src/bridge/bridge_router.cpp)).

**Impact:** A maintainer must understand a small distributed protocol to
change summarization or page import. This goes against the KISS rules in
AGENTS.md.

**Fix:** Use the [Recommended design](#recommended-design).

### P2: Should fix

Each finding below is a mechanism with no recorded source. For each one, the
review gives the requirement that it serves and what breaks without it.

#### P2-1. Job history and the second table

**Status:** Remove. Events are enough (requirement 5).

Finished jobs and tasks stay forever, with saved names, finish times and paged
history (lines 24–25, 51 and 102). To resume, CHA needs only the unfinished
work. Events already record the start, the failures and the end.

**Fix:** Delete the row when the job ends. Remove `note_job_tasks`,
`finished_at`, the saved names, and the history reads.

#### P2-2. The FIFO summary queue

**Status:** Keep the queue (requirement 2), but make it simple.

The design adds `queue_order`, the `one_running_summary` index, tail rules for
Resume and Retry, and a "mark reused at claim" check (lines 11, 34 and 67–68).

**Fix:** Order the queue by row ID. The summary row with the lowest ID runs.
Remove `queue_order` and the partial index, because no Resume or Retry puts a
job at the tail in this stage. Keep one simple check: with Overwrite off, skip
an item if its version exists when the item's turn comes. An earlier job in
the queue can make that version.

#### P2-3. Pause, Resume, Retry failed, and the `blocked` state

**Status:** Remove from this stage (requirement 3).

Pause needs a reservation rule for imports and tail rules for summaries. Retry
needs reopen rules and reservation checks (lines 53–58). The next stage will
design the controls that it needs.

#### P2-4. The executor protocol

**Status:** Remove.

The design adds attach, claim, task attempts, job revisions and observers
(lines 43–47, 60 and 103). This protocol exists for several executors and for
stale workers. The desktop app has one renderer. The renderer sends each
commit, so a renderer that is gone cannot commit. The existing connection
release cancels native work from an old window. If a row stores only the
remaining work, startup has no "running" state to reset.

#### P2-5. Client UUID for idempotent admission

**Status:** Remove.

This protects against a lost Start reply (lines 41 and 101). In the in-process
desktop bridge, a reply is lost only when the window reloads or crashes. In
that rare case, a second click can create a second job or a second "New note".
This is acceptable. Use an integer ID.

#### P2-6. Three import stages and the `page_data` checkpoint

**Status:** Remove. A repeated formatting call is acceptable (requirement 4).

The design splits the current single reader call into Retrieve and Format,
stores up to 12 MiB in the vault, and adds a staged-page read operation
(lines 79–85). Its only purpose is to avoid a second paid formatting call
after a crash.

**Fix:** Store only the URL and the destination. After a restart, run the
existing import flow again from the start.

#### P2-7. Body token and native re-merge

**Status:** Remove.

This protects against a D1 update or a summary replacement between the read
and the write (lines 91–93). The current code does not do this. D1 work does
not add concurrency checks for corner cases. Without it, the last write wins,
as it does today. Keep the current editor freeze and the note-action guard.

#### P2-8. Metadata savepoints in the append transaction

**Status:** Remove.

Today, the rename and Make read-only steps run after the save, and a failure
shows a warning ([useNoteImports.ts](../webapp/src/notes/useNoteImports.ts)).
The design moves them into the append transaction with savepoints (line 94).
Without the savepoints, a crash between the save and the rename leaves a note
with its old name. This is harmless.

**Fix:** Keep the current separate calls. Make only the note save and the row
delete atomic.

#### P2-9. `notes.jobsChanged` notices

**Status:** Remove.

The renderer runs the jobs, so it already knows their progress (line 105). The
notices are needed only for observers and a separate Jobs view. Add them only
if a confirmed Jobs view needs them.

#### P2-10. Failed imports keep checkpoints with unclear rules and no limit

**Status:** Resolved by P2-6.

The reservation index excludes `failed`, so a new Send to the same note can
create a second job while the failed job keeps its checkpoint (lines 38 and
58). Line 95 says a second Send "can invoke Retry failed", but the design does
not say when a Send retries the old job and when it creates a new one. Before
the Jobs view exists, no control clears a failed import. Each checkpoint (up to
12 MiB) stays in the vault with no limit.

#### P2-11. Visible changes were not shown for approval

**Status:** Resolved. The user approved the texts in
[Visible changes approved by the user](#visible-changes-approved-by-the-user).

The design changed visible behavior but showed only the later Jobs view
(lines 111–126). It added queued, terminal and control Events (lines 41, 51
and 60). It removed the "already running" error from the Summarize dialog
([backctrl-design.md](backctrl-design.md), line 38). It added messages for a
duplicate import and for a retry from the prompt (lines 77 and 95). AGENTS.md
requires approval for each visible change. Any text beyond the approved list
needs a new approval.

### P3: Lower priority

#### P3-1. The stated reason for clone and R2 cleanup is wrong

**Status:** Resolved by requirements 7 and 8.

The design clears jobs at the destination of a local copy or an R2
installation because "local IDs can address different content" (line 130).
That reason is wrong: an installation replaces the old file, and in a copy the
IDs match the content.

**Fix:** Delete all jobs in the R2 upload copy, next to the existing audio
deletes ([r2_database_transfer.cpp](../src/app/r2_database_transfer.cpp)).
Delete all jobs in a local copy at the existing cleanup point
([vault_operations.cpp](../src/app/vault_operations.cpp)). An R2 installation
needs no extra cleanup, because the uploaded copy has no jobs.

#### P3-2. The schema location is wrong

**Status:** Still applies.

Line 20 points to `workspace_config_store.cpp`. Note tables are created in
[note_storage.cpp](../src/storage/note_storage.cpp). Correct the link.

#### P3-3. Reading options with validation can block work

**Status:** Still applies.

Line 29 validates options "on write and read". AGENTS.md says that unused or
obsolete settings must not block an operation. On read, ignore unknown fields
and write a warning log message.

#### P3-4. A vague "stop dispatching" state

**Status:** Still applies.

Line 60 says that after a storage failure, CHA stops dispatching against the
store and reconciles later. This is one more state with no defined exit. Fail
the current item, write an Event if possible, and continue with the next item.

#### P3-5. The acceptance plan is too large

**Status:** Still applies.

Lines 136–145 ask for race tests across eight suites and a real kill and
relaunch for each kind. Most of these tests exist only to check the mechanisms
in P2. With the recommended design, keep a few tests: resume after a restart
for each kind, the queue order, one saved result per item, and an older vault
opens and keeps its data.

## Recommended design

### Storage

Use one table. A row exists only while its work is unfinished.

```sql
CREATE TABLE IF NOT EXISTS note_jobs (
  id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL CHECK (kind IN ('summarize', 'import_page')),
  target_id INTEGER NOT NULL,
  document_id INTEGER UNIQUE,  -- import destination; NULL for summaries
  options TEXT NOT NULL,       -- JSON: provider, effort, levels, overwrite / URL, mode
  remaining TEXT,              -- JSON: summary items not done yet; NULL for imports
  created_at TEXT NOT NULL
) STRICT;
```

Create the table in the existing vault-open schema code. An older vault gets an
empty table.

### Summaries

- Start runs the existing planner
  ([notes_operations.cpp](../src/notes/notes_operations.cpp)) and shows its
  existing errors in the dialog. Then it inserts a row. `remaining` holds the
  planner's note list (each note with its levels) and a folder flag.
- If another summary row exists, the new job waits and CHA writes the approved
  "queued" Event. Otherwise CHA writes the existing "started" Event.
- The summary row with the lowest ID runs, with the existing three note lines,
  retry waits and cost logging.
- Each saved version removes its item from `remaining` in the same
  transaction. So does each failure that writes an Event.
- When a child L3 fails, the same transaction also removes the folder flag and
  writes the existing "Could not build the Summary" Event. After a restart, no
  other state is needed to stop the folder Summary.
- With Overwrite off, CHA checks before the provider call whether the version
  exists. If it exists, CHA removes the item and makes no provider call.
- The last item deletes the row and writes the finish Event in the same
  transaction. Then the renderer starts the next summary row.

### Imports

- The row holds the URL, the note, the document ID and the mode.
- A second import to the same document is rejected. The `UNIQUE` column does
  this check.
- **New note from URL** creates the note and the row in one transaction.
- The existing import flow runs from the start
  ([useNoteImports.ts](../webapp/src/notes/useNoteImports.ts)). Its final note
  save deletes the row in the same transaction. The rename and Make read-only
  steps stay as separate calls after the save.
- A definite failure deletes the row, shows the error as today, and writes the
  approved import failure Event.

### Startup, quit and vault switch

- When the vault and the bridge are ready, the renderer reads the rows. It runs
  the first summary row and all import rows with the existing code. This also
  applies when CHA opens into chat.
- A quit, a window reload or a vault switch signals in-flight work, as today.
  It keeps the rows and writes no cancellation Event.
- Each commit carries the vault epoch, as today, and fails if its row no
  longer exists.

### Bridge

- Add one call that reads the unfinished rows.
- The existing summary start, generate and commit calls, and the page import
  calls, carry the job ID.
- Remove the in-memory summary batch and the import run map as the source of
  unfinished work. Keep in memory only what the running code needs.

### Vault copies

- Delete all rows in the R2 upload copy, next to the audio deletes.
- Delete all rows in a local copy, at the existing cleanup point.

## Fix order

1. Add the confirmed requirements to the design (P1-1).
2. Replace the two-table design with the recommended design (P1-3, P2-1 to
   P2-10).
3. Record P1-2 as a known effect of this stage.
4. Use only the approved visible texts (P2-11).
5. Fix P3-2 to P3-5.
