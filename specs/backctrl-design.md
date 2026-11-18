# Controls for background note summarization

## Requirements

- In desktop Notes, **Summarize** opens a dialog for the selected note or folder.
- In the dialog the user chooses the provider, the reasoning effort, the outputs and whether to replace existing outputs.
- **Start** applies those choices to one background batch.
- The saved Notes settings supply the initial provider and effort each time the dialog opens.
- When L3 is checked, the run rebuilds the folder Summary. Overwrite applies only to Compressed, L1 and L2.
- The dialog replaces the **Compress** command. Remove the **Compress** and **Cancel compression** items from the note and folder menu.

The rest of this document is the proposed design. It extends the [background summarization design](backsum-design.md).

## Dialog and defaults

| Target | Checked on opening | Overwrite on opening | Effect of pressing Start with these defaults |
| --- | --- | --- | --- |
| Note | Compressed, L1, L2 | On | Generate all three versions, replacing any that exist. |
| Folder | Compressed, L1, L2, L3 | Off | Fill missing Compressed, L1 and L2 versions on eligible direct children. Build or rebuild the folder Summary. |

Proposed dialogs for a note (left) and a folder (right), with an example provider name. Each output has its own checkbox. Like the note audio setup dialog, the dialog has no visible title. Its accessible name is “Summarize”.

```text
Provider   [ My provider ▾ ]           Provider   [ My provider ▾ ]
Effort     [ Medium      ▾ ]           Effort     [ Medium      ▾ ]

[x] Compressed   [x] L1   [x] L2       [x] Compressed   [x] L1   [x] L2   [x] L3

[x] Overwrite                          [ ] Overwrite

               [ Cancel ] [ Start ]                    [ Cancel ] [ Start ]
```

Open the dialog from the existing **Summarize** menu item. Bind it to the target ID and vault epoch captured by the menu. Use the existing modal dialog and focus conventions, labeled controls and keyboard navigation. Cancel or Escape dismisses setup. Opening, changing or dismissing the form makes zero inference requests and reserves no batch slot.

Load the saved `compression_provider`, `compression_reasoning_effort` and the list of providers using the existing Notes settings and provider-list calls. Display provider names and submit their IDs. Offer the five Notes efforts, **None**, **Low**, **Medium**, **High** and **Extra high**. If the saved provider is absent, show **Select provider** and let the user choose any configured provider. With no configured providers, the list holds only **Select provider**, as on the Notes settings screen, and Start stays disabled. Settings load failures appear in the dialog. The choices live only in the form and the batch.

Enable Start once the form has loaded, a provider is selected and at least one output is checked. Offer L3 only on folders. A single submission runs the existing editor-save and admission sequence. While admission is pending, prevent double submission and dismissal. Every start error appears in the dialog and keeps the choices visible. This includes a batch that is already running, whether this window or another connection started it. A vault switch dismisses the obsolete form and invalidates outstanding replies. On acceptance, close the dialog immediately and return focus to Notes. The user can edit or navigate while generation runs.

## What Overwrite controls

Apply Overwrite **per selected note-level output**: Compressed, L1 and L2. L3 does not use Overwrite. On a note with an existing L1 and a missing L2, selecting just L1 and L2 with Overwrite off generates only L2. With Overwrite on, the same selection regenerates L1 and creates L2. Unchecked outputs receive zero generation requests and zero writes.

| Output | Unit of work | Input |
| --- | --- | --- |
| Compressed | One version per eligible note | Its saved original Markdown |
| L1 | One version per eligible note | Its saved original Markdown |
| L2 | One version per eligible note | Its saved original Markdown |
| L3 | One folder Summary per folder | One section from each eligible original, then an overview of all sections in tree order |

L3 has no dependency on Compressed, L1 or L2. With only L3 checked, a folder with three eligible children needs three section calls and one overview call, whether or not a folder Summary exists and whatever the Overwrite setting. Checking L3 builds the Summary or rebuilds the entire existing Summary. An ordinary note named “Summary” counts as a possible source. The existing `folder_summary` flag on a direct child identifies the folder Summary.

A successful replacement discards edits, highlights and comments in that output. Pressing Start with Overwrite checked authorizes this, including the **default single-note replacement of Compressed**. Pressing Start with L3 checked authorizes the replacement of the folder Summary. Each write is independent. A failed generation, conversion or save preserves the previous version. Existing L1 and folder Summary replacement rules handle associated audio and, for the folder Summary, its contribution to ANNOTATIONS.

## Planning and execution

Today the native start call returns note IDs and existence flags. The renderer then chooses work by target kind. It always regenerates L1 and L2 on a single note and always attempts L3 on a folder. Native generation and line-cost setup also consult Notes settings during the run. The two changes needed here are to plan the checked outputs once and to keep the selected provider ID and effort in the accepted batch. [Native Notes operations](../src/notes/notes_operations.cpp) · [Desktop coordinator](../webapp/src/notes/summaries.ts)

1. **Start.** Extend `notes.summaryStart` with the selected provider ID, effort, levels and Overwrite. Under the existing lifecycle mutex, verify the epoch, single-batch slot and target. Validate the options on the native side. Use the existing eligibility rules. A note needs at least 4 KiB of saved original Markdown. A direct note request ignores its exclusion flag. A folder must be user-managed and includes only direct child notes meeting the minimum, excluding summary notes and children marked excluded.
2. **Plan.** For each eligible note, schedule each checked note-level output if it is missing or Overwrite is on. If L3 is checked, include a section for *every* eligible child, including those with no other work. Omit children with no scheduled step. Keep the current “No eligible notes to summarize.” error for an empty eligible set. If eligible notes exist but every selected output already exists, show “All selected outputs already exist.” in the dialog so the user can change the selection or enable Overwrite. Both cases make zero requests and create no lifecycle Events.
3. **Admit.** Resolve and validate the chosen provider and effort before recording the start Event. Keep the selected provider ID, effort and Overwrite in the existing native batch, next to its owner connection, epoch and target. Do not keep the plan or a copy of the provider configuration. Use the planned note count for the start Event. Later calls keep the existing check that they belong to the batch's connection and epoch. The renderer's check of its own running batch and the native slot both return the busy error to the dialog. The native slot also handles competing starts from another connection.
4. **Generate.** Drive the existing coordinator from the returned plan. In tree order, run up to three note lines concurrently. Each line processes its selected steps in Compressed, L1, L2, L3 section order. A retry holds its line. Read the saved original when each step starts and the configured prompt for that step. The Compressed step uses the existing note compression prompt. Every step, retry, the overview and line-cost attribution use the batch's provider ID and effort instead of the Notes settings. As today, each new step reads that provider's current configuration, and a request or retry already prepared keeps its inputs. A later change to Notes settings has no effect on the batch. Credential access still goes through the normal provider client. A credential failure, or a provider deleted during the run, follows the existing per-step failure path.
5. **Finish.** A failed note-level step allows independent steps to continue. A failed required section suppresses remaining unstarted section work and the overview. Only a complete set of sections, a successful overview and successful conversion can replace the folder Summary. Keep the existing owner-scoped finish, lost-reply cleanup, lifecycle Events and cancellation on exit, vault maintenance or owner disconnect.

Reuse Notes provider preparation with the batch's provider ID and effort in place of the Notes settings. Keep the credential, effort compatibility, Flex, prompt and retry rules in their existing helpers. A selected provider that no longer exists fails with “That provider was not found.” instead of the Notes settings message. Report an incompatible requested network effort in the dialog so the user can select a supported effort. Ignore unused provider settings with the existing warnings. Audio settings and chat **Save summary** do not participate in the dialog.

### Removing Compress

Remove the **Compress** and **Cancel compression** items from the note and folder menu. The dialog with only Compressed checked does the same work for a note: Overwrite on replaces its Compressed version, and Overwrite off creates a missing one. A folder run uses only eligible direct children. Thus a folder run does not compress notes in nested folders or notes marked “Exclude from summaries”. To compress such a note, run the dialog on that note. The batch has no Cancel action, and its Compressed step can no longer be cancelled separately.

Remove the code that only these two commands use:

- The compression progress dialog, the standalone compression path in `App`, and [compression.ts](../webapp/src/notes/compression.ts).
- `NotesService::compress_note`, `compress_summary_note`, `run_compression` and the one-at-a-time compression lock.
- The per-note compression reservation, with its status, cancellation, note deletion and vault maintenance handling.
- The bridge calls `notes.compressStart`, `notes.compressCancel`, `notes.compressionStatus` and `notes.summaryCompress`, and the `notes.compressionChanged` notice.
- The tests of these commands and calls.

ChaWeb has no **Compress** command and does not use this code.

### Desktop bridge

| Operation | Proposed contract |
| --- | --- |
| `notes.summaryStart` | Request `{id, provider_id, reasoning_effort, levels, overwrite}`. Levels is a nonempty list of distinct values from `compressed`, `l1`, `l2`, `l3`. Reject unknown values and single-note L3. Effort uses the Notes wire values `none`, `low`, `medium`, `high`, `xhigh`. Overwrite is a required boolean. |
| Start reply | Return `{notes: [{id, name, levels}]}` with the actual steps due for each note. Here `l3` means “generate this child's section.” Its presence calls for one overview on the source folder when all sections succeed. Native admission builds this plan once. The coordinator follows it. |
| `notes.summaryGenerate` | Add the part `compressed`. Like L1, it reads the saved original and returns `{markdown}`, here the compressed text. Other parts keep their payloads. |
| `notes.summaryLine`, `summaryAbandon`, `summaryFinish` | Keep their existing payloads. The batch supplies provider and effort. |
| `notes.summaryCommit` | L1, L2 and L3 keep their document payload. Add `{id, part: "compressed", text}` to save the generated text as the Compressed version. All parts return an empty reply. |

Change the desktop DTO, generated client types, native dispatch, runtime guards and wire fixtures together. Remove the standalone compression calls from the same places. Batch options require no database migration or new saved setting. [DTO](../resources/dto.yaml) · [Desktop client](../webapp/src/api/nativeClient.ts) · [Response guards](../webapp/src/api/noteGuards.ts)

### Replacing an open document

L1, L2 and the folder Summary already have a short editor guard around their commit. Compressed currently writes inside the provider worker. Its completion callback handles the case where the editor showed the original as a fallback. Reloading after a replacement is insufficient when an autosave of the old Compressed document is already in flight. [Editor commit handling](../webapp/src/components/App.tsx) · [Compression worker](../src/notes/notes_operations.cpp)

Thus the Compressed step works like L1 and L2. `summaryGenerate` runs the provider request in the existing summary worker, with its retries, cancellation and cost accounting. The coordinator then commits the text through the same editor guard as L1 and L2. Immediately before replacing a displayed version, freeze that buffer, then wait for any in-flight save to settle before issuing the commit. On success, retire the old buffer and reload the version if it is still visible. On a definite failure, resume the buffer with its pending edits. If the editor shows the original as a fallback, flush its edits before switching to the new version. Preserve the existing handling for a pending load, a changed view and an unknown commit outcome. Provider waits hold no editor guard.

The Compressed step uses the existing summary step Events with the label “Compressed version”, for example “Compressed version ready for …”. Like L1 and L2, it writes no separate start Event.

Do not check existence again at the final write, and do not compare the original again before a commit. The plan decides what is written. During a run, only incoming D1 changes can create a Compressed, L1 or L2 version, and they get no extra checks, as in the background summarization design. An edit to the original during the run has the same effect for Compressed as for L1 and L2: the new version reflects the earlier text, and the edit is kept. This replaces the discard of a batch compression result when the original changed. Write the Compressed version in its existing storage format and sync record. Reuse the current storage routines for version identity, conversion, annotations and sync. [Note storage](../src/storage/note_storage.cpp)

## Acceptance checks

Use the desktop Notes tests with the fake provider. Assert provider call counts as well as saved documents.

| Case | Expected result |
| --- | --- |
| Open, cancel and reopen | Each target shows the defaults pictured above. Changing the form and cancelling starts no batch. Reopening reads the current saved Notes provider and effort. |
| Provider override | Set Notes to provider A, choose B with a different effort and start a folder run. Change Notes settings during a held request. All selected steps, retries, the overview and cost attribution use B. A valid B works even if the saved provider is absent. Delete B during a held request, and the next step fails with “That provider was not found.” |
| Individual checkboxes | Exercise each output alone. L2-only on a note makes one request. Compressed-only uses the compression prompt and storage format. Clearing every box disables Start. Native admission rejects invalid options. |
| Mixed folder | A has L1 only, B has L2 only, both are eligible, and the folder Summary exists. Select L1, L2 and L3 with Overwrite off. Expect A's L2, B's L1, two sections and one overview that replaces the Summary. With only L3 checked, expect two sections and one overview in tree order, with Overwrite on and with Overwrite off. Include an ordinary note named “Summary” in a separate case. |
| No work and exclusions | Distinguish a fully processed target from a folder with zero eligible notes. Check the 4 KiB boundary, an excluded child, an explicit run on that child and a nested note. No-work and rejected starts leave the slot available and write no start Event. |
| Replacement and failure | On a note with three existing versions, default Start replaces all three. With only L1 checked, verify the original, Compressed and L2 content byte-for-byte. Fail L1 and fail an L3 overview separately. The prior output and its audio survive the failure. A successful L1 replacement invalidates its old audio. |
| Editor and concurrent writes | Hold a provider reply while editing an unrelated note, then while editing the target Compressed version with an autosave in flight. Verify the successful replacement survives the late save, a failed commit preserves the draft and an original shown as a fallback keeps its edits. Edit the original while a Compressed request is held. Verify that the edit is kept and that the new Compressed version is saved. |
| Batch lifetime | Cover competing starts with the busy error in the dialog, admission errors, the three-line cap, owner disconnect and vault switch before commit. The old reply cannot write, and each accepted batch has one terminal Event under normal shutdown. |
| Removed commands | The note and folder menus show no **Compress** or **Cancel compression** item. |

Extend the existing desktop suites for scheduling, editor behavior, service admission and bridge contracts. Update the desktop native smoke test to open the dialog and press Start before observing background results. [Scheduler tests](../webapp/src/notes/summaries.test.ts) · [App tests](../webapp/src/components/App.summary.test.tsx) · [Notes service tests](../tests/notes/unit_notes_service.cpp) · [Desktop smoke test](../tests/native/macos/test-note-summaries.py)

This design extends the existing scheduling and background lifecycle decisions. [Concurrent summarization](summary-ext.md) · [Background summarization](backsum-design.md)
