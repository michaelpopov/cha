# Background note summarization

Technical design for **Summarize** on a note or folder in desktop CHA. It supersedes the modal progress, the editor freeze and the Cancel action (`notes.summaryCancel`) in the [note summary design](../specs/summary-design.md) and the [concurrent summary design](../specs/summary-ext.md). Generation order, prompts and result formats follow the concurrent summary design.

## Requirements

These requirements come from the user:

- Summarization runs in the background. It does not block other functionality.
- An Event records the start of a batch. Another Event records its end, or its cancellation when the user exits CHA.
- One summarization batch runs at a time.
- When CHA exits, the rest of the batch is cancelled.

The rest of this document is the proposed design.

## User experience

Select **Summarize** on a note or folder. CHA saves pending editor changes, checks the request and records a start Event in the active vault. The menu closes. The user can type in the editor, open another note, change text mode, chat, change layouts or open Settings while the provider works. Completion does not take keyboard focus. It changes the screen only to reload the open note when a new version replaces what the note shows, as described in [The open editor](#the-open-editor).

CHA accepts one note-summary batch at a time in a running desktop application. A single note counts as a batch. A folder with three eligible notes also counts as one batch. Selecting **Summarize** again reports which batch is running and lets it continue. The second request is never queued. The batch has no Cancel action. Standalone **Compress**, audio generation and a chat session's **Save summary** are separate commands with their existing admission rules. **Cancel compression** in the note menu keeps its existing behavior: it stops a running compression step of the batch for that note, and the batch continues with the next step.

On application exit, CHA stops starting requests, signals in-flight requests and retry waits, discards unfinished results and records one cancellation Event. Switching vaults is allowed during a batch. CHA cancels the batch and records the cancellation Event in the old vault before switching. Other vault maintenance and a closed or reloaded window cancel the batch in the same way. Each version saved before cancellation stays available. The next launch starts idle.

### Proposed interaction

The following shows the existing surfaces after selecting **Summarize**. The timestamps and names are examples.

```text
Notes                                  Settings → Events, newest first
▾ Book                                 ⓘ  10:14  Summarization finished for “Book” (folder 8).
    Chapter 1                             …      [existing individual result Events]
    Chapter 2                          ⓘ  10:02  Summarization started for “Book” (folder 8), 2 notes.
```

The summary progress dialog no longer opens for **Summarize**. A second start shows its error in the existing Notes tree message area. Example text is “Summarization is already running for ‘Book’.” A folder with no eligible notes shows “No eligible notes to summarize.” in the same area. An invalid target, missing provider, failed initial editor save or empty folder plan starts no batch and creates no lifecycle Events. An accepted start needs no further acknowledgement from the user.

## Where the current operation blocks

| Code today | Consequence for the design |
| --- | --- |
| `runNoteOperation` in `webapp/src/components/App.tsx` shares `compressionRun`, `NoteCompressionDialog` and `noteLeave.beginNoteAction()` between Compress and Summarize. It keeps them through inference, tree refresh and editor reload. | Split the two entry points. Hold a note action only for a short local editor operation. Leaving the summary Promise unawaited inside the current wrapper would still freeze the window. |
| `webapp/src/notes/summaries.ts` sorts targets using the same comparator as the tree, sequences calls, assembles L3 and uses the DOM/ProseMirror converter in `summaryImport.ts`. | Run this existing coordinator from the lifetime of `App`, independent of the Notes tree, current screen and selected note. Keep the converter and its size and formatting checks. |
| `src/notes/notes_operations.cpp` already launches inference and summary compression through `BackgroundJobs`. It tracks individual calls and costs by bridge connection. | The native Notes service owns admission and the lifecycle Events. A frontend busy flag alone cannot reject a start from another connection or record an Event after the page is gone. |
| The desktop hosts negotiate a close with the editor, then detach the bridge before requesting runtime shutdown. `NotesService::cancel_summaries_locked` already runs at connection close (`release_connection_resources`), at vault maintenance (`drain_for_maintenance`) and at exit (`Application::join_shutdown`). **Save summary** uses it to record its cancellation Event while the store is open. | Retire the batch in the same function. The page can disappear before a JavaScript cleanup or bridge reply runs. |

## Batch admission and scheduling

1. `App` holds its batch from the `summaryStart` request until the `summaryFinish` reply. If `App` already holds a batch, report its target immediately. This check also makes sure that the finish call of an old loop goes before a new start. Otherwise use the existing note action and save handle to settle an in-flight save and flush remaining edits. Recheck the editor identity and vault epoch after awaits. Release the freeze and action in a `finally` block. Surface a save failure and preserve the draft. Provider waits, retries and folder assembly never own this guard.
2. Turn the native planning call into `notes.summaryStart({id})`. Under the lifecycle mutex, validate the epoch, check the single slot, apply the existing target and Notes-provider validation, and build the plan. A note needs at least 4 KiB of saved source Markdown. A folder includes eligible direct child notes of that size, excluding summary notes and children marked excluded. Single-note runs ignore the exclusion flag. A folder plan with no notes is an ordinary error. The single slot belongs to the desktop runtime.
3. For an eligible run, append the start Event and fill the slot before releasing the mutex. If the Event cannot be written, the start fails. Return the existing list of targets and version flags. The first provider call happens only after admission. If the connection is gone when `summaryStart` returns, the router calls `release_connection_resources` again, as `start_background` already does for summary calls. Connection cleanup then retires the new batch.
4. After acceptance, `App` starts the async loop and returns control to the menu caller. The loop carries the originating epoch. `App` sends `summaryFinish` after every `summaryStart` request, whatever its result. After acceptance, the loop's `finally` block sends it, also after an unexpected error. After a lost start reply, the same call releases a slot that native code admitted. Fetch current view references when applying results. A tree refresh callback captured at start can belong to an unmounted tree.

| Target | Work |
| --- | --- |
| One note | Regenerate L1, then L2 from the saved original. Commit each independently. |
| One folder | Start up to three note lines in tree order. Each line makes missing Compressed, L1 and L2 versions in that order, then an L3 section from the original. Existing versions are preserved. A line owns one slot through its retries. |
| Folder overview | After all lines settle, assemble successful sections in tree order, generate the overview and commit the complete linked Summary note once. A failed required section suppresses the overview. Other version work continues. |

Keep the existing Flex eligibility, cancellation-aware retry waits, request limits, line cost logging and literal title construction. An individual provider or conversion failure leaves that output intact and permits independent work to continue. A lost connection or a changed vault stops the loop, as today. The folder's compression step skips a note already claimed by standalone compression. Read the configured provider and prompt for a new step as the current code does. A request already sent keeps the inputs it was given.

## Native ownership and bridge contract

Keep one optional active batch in `NotesService::Impl`, next to the session-summary runs and guarded by the existing lifecycle mutex. It holds the owner connection, the originating epoch and the target label (name, kind and ID). The summary calls stay registered by connection as today, so connection cleanup, vault maintenance and exit cancel them through the existing paths.

| Desktop bridge operation | Contract |
| --- | --- |
| `notes.summaryStart({id})` | Replaces `notes.summaryPlan` and returns the existing `{notes}` shape. An occupied slot returns an ordinary busy error naming the active target. An empty folder plan returns an ordinary error. |
| `notes.summaryLine`, `summaryGenerate`, `summaryCompress`, `summaryCommit`, `summaryAbandon` | Keep their current arguments, epoch checks and replies. |
| `notes.summaryFinish({})` | If the calling connection owns the slot, record the finish Event and clear the slot. Otherwise do nothing. |
| `notes.summaryCancel` | Remove it together with the dialog's Cancel action. |
| `cancel_summaries_locked` | Also retire the batch: for its owner at connection close, and for any owner at vault maintenance and at exit. Record the cancellation Event when the vault epoch has not changed, as `retire_summary` does, then clear the slot. A cleared slot is not retired again. |

## Edits while the provider is working

Each step reads the saved original when it starts, as today. CHA does not compare the sources again before a commit. An edit made during the run therefore has the same effect as an edit made just after it: the new version reflects the earlier text, and the edit itself is kept. The user summarizes again when necessary. The existing compression check stays: the batch's compression step discards its result when the saved original changed.

The batch uses its admitted list of targets in tree order. Notes added later are eligible on a later run. A note deleted during the run fails its next step with the existing failure Event. A folder Summary deleted during the run is created again by the final commit. Incoming D1 changes get no extra checks.

### The open editor

Use the current editor, displayed document ID, selected mode, save handle and epoch at the time of each commit. Two cases need the editor:

- **Replaced document.** The commit replaces the displayed document: the L1 or L2 of a single-note run, or the folder's Summary note. Before the commit, briefly acquire the existing note action, freeze the editor and settle its in-flight save, so that an old save cannot land after the commit. After a successful commit, retire the old buffer before it can autosave again and reload the document if it is still visible. Edits typed into that document during the run are replaced, because the user asked for a new version. If the commit fails, unfreeze the buffer and keep its edits.
- **Fallback view.** The open note shows its original as a fallback for the mode of the new version. After the commit, or after the `{compressed: true}` reply of the native compression step, acquire the note action, save the open edits to the original and reload the note if it is still visible. If the save fails, keep the buffer and its existing save error.

Recheck the current view after each await. Release the guard on all paths. An editor showing an unrelated note needs no guard.

After a folder Summary commit, also reload a displayed ANNOTATIONS note of that folder. Preserve the reading position where supported. A hidden Notes pane loads the saved result on its next open. Refresh the mounted tree's version flags and new Summary row through its existing refresh path. The chat session summary handler in `App.tsx` already demonstrates how to handle a pending editor load, in-flight writes, an unknown commit outcome and a changed view. Keep the existing L1 and Summary audio invalidation notices.

## Events and termination

Use the existing per-vault `event_history` table. Keep individual result Events for diagnosis. Each admitted batch adds one start row and one terminal row. They are subject to the existing 1,000-row history limit. The captured target name, kind and ID identify the run to the user. A single note uses the existing note label, for example “Chapter 1” (note 12). Example rows follow.

| Severity | Event message |
| --- | --- |
| Info | Summarization started for “Book” (folder 8), 12 notes. |
| Info | Summarization finished for “Book” (folder 8). |
| Info | Summarization cancelled for “Book” (folder 8). |

The finish row does not judge the outcome. The individual Events show each failure and skip. The cancellation row does not name its cause. If a terminal Event cannot be written, log a warning and still clear the slot, as `retire_summary` does. Note text and raw provider responses do not belong in lifecycle Events.

1. **Normal finish.** The coordinator settles every started line, conversion and write, including the final folder commit or its failure, then calls finish.
2. **Confirmed exit.** Preserve the existing unsaved-note close negotiation. If the user elects to stay, the batch continues. After the host commits to exit, it detaches the bridge and requests runtime shutdown, which signals all workers. Connection cleanup or `Application::join_shutdown`, whichever runs first, retires the batch through `cancel_summaries_locked` while the store is open. Provider requests and retry waits stop on cancellation, so the workers normally end within the existing 10-second shutdown grace. The macOS and Windows hosts need no change.
3. **Switch or maintenance.** `drain_for_maintenance` already calls `cancel_summaries_locked` before the original database can close or the epoch can change, so the Event goes to the old vault. A same-vault no-op or a switch rejected before draining keeps the batch. If maintenance starts and later fails, its cancelled batch stays cancelled. Old replies cannot write into the new vault, because each call checks its epoch.
4. **Window closed or reloaded.** Navigating to another screen keeps the root coordinator and connection alive. A full renderer reload or lost bridge removes the coordinator. Connection cleanup cancels the batch and records the Event. An unrelated connection closing has no effect.

After cancellation the application has no queued work to replay. A remote provider may finish a request it already received. Its response cannot write a result. A hard kill, a power failure or a worker that does not stop within the shutdown grace can leave a start Event without a terminal Event. Recording cancellation on the next launch would require a separate recovery requirement.

## Implementation and acceptance

Implement the native slot and its Events first. Update `resources/dto.yaml`, the generated desktop client types, runtime guards and bridge dispatch together with the coordinator. Then split the Summarize path out of `runNoteOperation`, connect the editor refresh and close paths, and replace the tests that currently require the summary dialog. The visible interaction above is proposed for approval before implementation.

Use the existing fake transport with a request that can be held until the test releases it. Let the test observe that a request actually reached the provider before exercising the foreground. Checking only whether the menu closed would miss a blocked editor or a batch that never started.

| Scenario | Acceptance check |
| --- | --- |
| Foreground use | While a provider reply is held, type and save an unrelated note, change text mode, send a chat message, open Settings and return. Observe the start Event while work is active. Release the provider while viewing Chat and verify focus stays in Chat. |
| Admission | Start a second batch from the same window and from a second connection, including a note against a folder. The second command reports the running target, offers no Cancel action and never runs later. Exactly one start Event is written. Invalid input, a note under 4 KiB, an empty folder plan and a failed initial save make none. A new batch can start after finish and after cancellation. |
| Scheduling and failure | Check the three-line maximum, per-line order, retries, section order, rich Markdown conversion and existing cost logging. Fail L2, a section, overview generation and conversion separately. Verify independent saves, preservation of the previous Summary and exactly one terminal Event for each batch. |
| Edits during the run | While a request is held, type into its source note and let autosave run. Release the reply. Verify the typed text survives, the new version is saved and the next note finishes. |
| Open editor | Hold a single-note L1 reply while that L1 is open with an in-flight save, then release it. Verify that the new L1 is shown and that the old buffer does not overwrite it. Repeat with the folder Summary and its ANNOTATIONS, and with L1 and Compressed modes showing the original as a fallback. Cover a failed commit, a failed fallback save, a pending load and navigation just before commit. |
| Quit and relaunch | Hold a provider request after an earlier version has committed. Exit through the real desktop close path and reopen the vault. Expect one cancellation Event, the earlier version present, the versions not yet replaced unchanged, zero provider calls on launch and a free batch slot. Repeat during retry backoff, between steps and during the final commit. Aborting a close keeps the batch running. |
| Vault and connection lifetime | Switch vaults with a request held, then deliver the old reply. Check the cancellation Event in the original vault and no lifecycle Event in the new vault. Repeat with owner reload, with an unrelated connection close, and with the owner connection closing while `summaryStart` runs. |

Place the native checks in the existing Notes service and bridge suites. Cover foreground behavior in `webapp/src/components/App.summary.test.tsx`, scheduling in `webapp/src/notes/summaries.test.ts`, and real quit/reopen in `tests/native/macos/test-note-summaries.py`. Run the desktop checks and standalone compression and session-summary regressions with fake transport.
