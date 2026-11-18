# The current note in desktop Chat

Technical design for the macOS and Windows desktop apps, November 17, 2026. [Product requirements](current-prd.md)

A user opens *Trip plan* and sends “Seneca, quote the cancellation clause in my current note.” Opening the note stored its ID as the current note in the application core. The Chat submit handler saves any outstanding edits and submits the message as today. The core adds the current note to the chat command. Jev can select both Seneca and the note in one call. Before starting Seneca’s answer, the session refreshes its copy of *Trip plan*, records the usual attachment notice, and names the note in the outgoing user message of this request only. The human transcript entry contains the original question.

## Current note in the core

`Application` owns the current note. Each desktop window runs its own process with one `Application`, so the value belongs to one window. Keep it in memory only, as one note ID guarded by a mutex. ChaWeb never sets it.

| Event | Core behavior |
| --- | --- |
| `notes.select` | Check the request’s context epoch. Read the item from the active database. Accept only a source note or annotation, never a folder or a compressed, L1, or L2 version row. Store its ID. |
| `switch_vault` or `download_database` succeeds | Clear the ID. Both replace the active database, and note IDs differ between databases. |
| The core reads the current note and the item no longer exists | Treat the selection as empty. `Application::bootstrap()` also clears the ID. This covers deletion in this window, through D1 sync, and by another process. `note_items` uses `AUTOINCREMENT`, so a deleted ID never names a new note in the same database. |
| Process exit | The value is lost because it is in memory only. |

Nothing else changes the value. Navigation, Settings, Events, conversation changes, layout changes, Discard in the save-failure dialog, and other vault maintenance such as an upload, a merge, or a configuration import keep it. Maintenance that publishes a new context epoch without replacing the database does not clear it.

`Application::bootstrap()` returns an optional `current_note_id`. Before it returns the ID, the core checks that the note exists, and clears the ID if it does not. `Application::submit` passes only the current note ID on `RawCommand` and does no database read, because the bridge holds its connection lock while it submits a session command. On the session thread, `SessionController::current_note` reads the note’s name with `read_item_` when Jev is enabled. A missing item or a folder counts as no current note.

## The UI copy of the current note

The UI keeps `AppState.activeNoteId` as a copy of the core value for the editor. It identifies the source item even when the editor displays a compressed, L1, or L2 document with a different `document_id`. Both desktop hosts run the same React workspace and native bridge. The UI changes the copy only in these cases:

| Case | UI behavior |
| --- | --- |
| The user opens a note | Call `notes.select`. Dispatch the existing `open-note` action only after the call succeeds. A text-mode change keeps the source ID and makes no call. |
| Startup and `vault-context-refreshed` | Take `current_note_id` from the bootstrap. `vault-context-reset` empties the copy until the refreshed bootstrap arrives. |
| The user deletes the open note or an ancestor folder in this window | Keep the existing `noteInSubtree` and `close-note` path. It applies the same rule as the core. |

Remove the `activeNoteId` clearing from `releaseClosedNote` in `webapp/src/state/view.ts`. It still clears the one-shot `noteDestination` and `noteImportSource` when the editor becomes hidden. The existing save-on-leave behavior applies when the editor is hidden. Returning to Notes reopens the note from the copy.

In `useNoteLeave`, Discard in the save-failure dialog disposes the editor and its unsaved edits but no longer dispatches `close-note`. When Notes shows the note again, the editor loads the saved text.

## Window title

Change the `document.title` effect in `webapp/src/components/App.tsx`. Remove `bootstrap.vault_name` from the title. Build the title from two parts, on every screen:

- The chat part is `activeConversationLabel` while `sessionOperation` is `idle`, as today, but no longer only in the Chat view.
- The note part is the name of `activeNoteId` in the `noteItems` listing, but no longer only in the Notes view. The `NotesTree` in the sidebar stays mounted while it is hidden and reports the listing again after a rename and after each `notesEpoch` change, so the name follows renames.

Join the parts as `CHA: <chat> — <note>`. With one part, use `CHA: <part>`. With no parts, use `CHA`. Both desktop hosts already copy the WebView title to the window title, so neither host changes.

## Save at Send

1. The Chat composer’s `onSubmit` is `App.submitInput`. `PromptInput.send` calls it after voice finalization. At the start of `submitInput`, before its first await, read the active conversation as today and `client.contextEpoch()`.
2. If `noteSaveRef.current` is open and has unsaved edits or a save in flight, save it with the sequence that `runNoteOperation` and `resolveLeave` use:
   - Call `beginNoteAction()`. If it fails, throw the existing “A notes action is already in progress.” error.
   - Call `const canceled = save.freeze()`, then `const admitted = save.inFlightSerial()`, then `await save.waitInFlight()`.
   - If `admitted !== null && save.saveError?.serial === admitted`, the in-flight save failed. Throw its message and do not start another save.
   - Otherwise call `flushRemaining()`. If it returns `false`, throw `save.saveError?.message ?? 'The note could not be saved.'`.
   - In `finally`, unfreeze the editor under the same conditions as `setNoteReadOnly`. If `canceled` is true and `flushRemaining()` did not run, call `save.submitNow()`, as `stayIntent` does, so the autosave that `freeze()` canceled still happens. Then call `endNoteAction()`.

   A clean editor, or an editor that is not open, needs no write.
3. After the save, check the vault epoch in the same way as `checkContext` in `runNoteOperation`. If the vault changed, throw the existing `vault_changed` error. Otherwise call `noteWelcomeSubmission` and `retainSession`, which today run at the top of `submitInput`, and then call `client.submitInput` with the conversation read in step 1. A failed save therefore leaves no welcome-turn change and does not keep an unused session.
4. Apply the save step to **every Chat composer submission**, including explicit recipients, commands, and messages that Jev will classify as unrelated. A thrown error reaches the existing composer error path. `PromptInput` keeps the question and pending voice usage because it clears them only on `clear_input`. The note keeps its unsaved edits. A later press of Send starts a fresh attempt.

The core reads the current note when it receives `session.submit`, after voice finalization and after the save.

## Native contract

Add the `notes.select` method with parameters `{ "id": 42 }` and the usual context epoch. Define it in `resources/dto.yaml` with the existing `NoteId` type and regenerate the desktop API types. A stale epoch, an invalid ID, a missing item, or an item that is not a source note returns the existing error for that case. The reply carries no data.

Add the optional `current_note_id` field to the `app.bootstrap` reply. `session.submit` does not change.

Carry the current note’s ID on `RawCommand` to `LiveSession`. `LiveSession` resolves it with `SessionController::current_note` and passes the ID and name through `handle_text_input` to `SessionController::submit_prompt` and `SessionController::start_multicast`. Keep the current grammar and precedence for host commands, Ref IDs, file paths, slash commands, and addressed prompts. Commands other than `/mcast` ignore the current note. `submit_prompt` and `start_multicast` resolve `@Name`, `@-`, and `/mcast` recipients as today, and a handle error returns before Jev. With a current note and Jev enabled, every prompt then goes through classification, whatever its recipient. Without a current note, explicit prompts start at once as today. With Jev disabled, dispatch using the existing route and ignore the current note.

## Jev decisions

Extend `make_jev_body` with an optional choice question named `use_current_note`. A request holds the `recipient` question, the note question, or both:

| Prompt | Jev request |
| --- | --- |
| No explicit recipient, no current note | `recipient` only, as today |
| No explicit recipient, current note | `recipient` and `use_current_note` |
| `@Name`, `@-`, or `/mcast`, current note | `use_current_note` only |
| `@Name`, `@-`, or `/mcast`, no current note | No request. The prompt starts at once, as today. |

With a note question, add the note’s name from `RawCommand` to Jev’s state. The recipient question continues to consider only the user’s prompt. The note question treats both the prompt and the name as data. Jev receives no note body, list of notes, or conversation history.

| Note choice | Classification criteria |
| --- | --- |
| `yes` | The user asks to read, quote, summarize, or answer from the open note. “The note I just opened” and a request about the selected note by its title qualify. The request can also name a recipient. |
| `no` | The user names a different note, wants to write a new note, asks to revise a previous answer, or changes the subject. “Summarize this” alone gives insufficient evidence. Use this choice whenever the source is unclear. |

Parse the two answers independently in `parse_jev_result`. An attachment requires an exact `choice` answer of `yes` to a question actually asked. A missing, malformed, or unknown note answer means no attachment and produces no notice. When the request asks the recipient question, keep a valid recipient choice, including `undefined` (use the captured default). If only the recipient answer is invalid, apply the existing recipient fallback and notice. A usable affirmative note answer can accompany that fallback. When the recipients are fixed, no recipient answer exists and no recipient fallback notice appears. The target never affects attachment: a self-note target from `@-`, from Jev’s `self_note` choice, or from the Self-notes default can attach the note.

An unavailable classifier, timeout, or unreadable response supplies no affirmative decision. Use the existing ordinary recipient behavior. Stop, cancellation, and submission expiry retain their existing semantics. Store the note ID in `PendingClassification` until the existing owner-thread handoff in `finish_classification`. A later selection cannot alter it. For an explicit prompt, `PendingClassification` also carries the recipients that the user chose: the target ID for `@Name` or `@-`, or the resolved target list for `/mcast`. `finish_classification` then dispatches through `dispatch_target` or `start_resolved_multicast` and never changes the default target. As on the implicit path today, `start_session_name` runs alongside Jev, and the answer starts after both finish. The naming request reads `notes_prompt()` when it starts, before the note is attached. For the first message of a new chat, the session name therefore comes from the prompt and any older copies only. This is accepted, because naming stays parallel to Jev.

## Refresh the copy before starting the answer

Resolve the effective target first. In the dispatch path shared by single-character, all-characters, and `/mcast` requests, and on the self-note path, validate the author, targets, and pending submission. For an affirmative note decision, perform the following work once, before capturing model context, activating the first run, or recording the self-note.

1. Read the note ID through `read_note_`. Production session opening binds this reader to `WorkspaceConfigStore::get_note_text(id, true)`, including for the Welcome session. It returns the source ID, current saved name, and Markdown. Storage chooses the level-0 compressed version if present and falls back to full saved text. The chat read is independent of the editor’s text mode. L1 and L2 are never selected.
2. Apply the Ref ID attachment limit to those Markdown bytes. Exactly 256 KiB is allowed. Recheck submission expiry after the read and before writing. A missing source returns “That note was not found.” An oversized source returns “This note is too large to add to a chat. The limit is 256 KiB of text.” Propagate other read or journal errors through the existing attachment error path.
3. Share the read, size check, and journal write with `SessionController::add_note`. Keep the standalone Ref ID command’s input-consumed and recent-session behavior in its caller. The automatic path must acknowledge the question only after dispatch accepts it. Use `SessionJournal::add_note` to upsert `session_notes` by `(session_key, note_id)`, insert the notice, and apply the existing sync behavior in one transaction. The journal already distinguishes “Added note …” from “Updated this chat’s copy of note …” by row existence. Give `add_note` a `skip_unchanged` flag and return `std::optional<TranscriptEntry>`. With the flag, inside the same transaction, compare the stored name and text with the read. When both are equal, write nothing, enqueue no sync, record no notice, and return no entry. The automatic path sets the flag. The Ref ID command does not, so it always refreshes and shows its notice. After commit, append a returned notice to the in-memory transcript before the human entry.
4. On a missing, oversized, or unreadable note, return an unconsumed failure through `SubmissionResult` and `LiveSession::settle_submission`. Retain the specific error for the originating composer. Leave the previous recipient default and any existing conversation copy intact. Neither a human turn nor a character request starts. The user decides whether to retry. Only a successful source read and refresh can supply a note for this send.

The journal owns each copy after attachment. Edits, moves, deletion of the original, and reopening the conversation leave it available for follow-ups. All copies accumulate in subsequent model requests until `/clear`, including one added by a mistaken classification. The existing schema, notices, and clear operation already implement that lifetime.

## Name the note in this request

Add an optional `current_note_name` member to `RunSpec`. For an affirmative note decision, dispatch passes the name from the note read to `start_generation`, also when the copy was unchanged,, which sets it on the run of every target, including all-characters and `/mcast` requests. A self-note starts no model request, so it gets no line. `project_model_context` is shared by all four provider encoders. When the member is set, it appends one fixed line to the last user message:

```text
The user refers to the note “Trip plan”.
```

`activate_run` keeps using `prompt_text` for the durable human entry, so the transcript shows the question as typed. Later requests project the stored entry and do not repeat the line. A naming request never sets the member. The system prompt and `notes_prompt()` stay unchanged, so the cached prefix with the system prompt and older history stays valid. Two copies with the same title in one chat are not distinguished. This rare case is accepted.

## Acceptance checks

| Boundary | Verification |
| --- | --- |
| Core selection | `notes.select` stores a source note or annotation and rejects a folder, a version row, a missing item, and a stale epoch. The value survives an upload, a merge, and a configuration import. `switch_vault` and `download_database` clear it. Deletion of the note or an ancestor in this window, and a deletion applied by D1 sync, clear it at the next bootstrap or submit. |
| UI copy | Update the `state/view.test.ts` expectations: Settings, Events, and opening a conversation from Notes in single view keep the ID, and returning to Notes reopens the note. Discard in the save-failure dialog keeps the ID and reloads the saved text. Startup and `vault-context-refreshed` take the ID from the bootstrap. In `Notes.test.tsx`, cover folder/menu focus, rename/move, and deletion of the selected note or its ancestor while a sibling survives. |
| Window title | Verify the four title forms from the PRD in the Chat view, the Notes view, split view, and Settings. Verify that the vault name never appears and that a rename of the current note updates the title. |
| Send and save | With dirty split-view text, verify that save completion precedes submit for both an implicit request and an explicit command. Fail an in-flight save with no newer edits and assert the save error and no second save request. Fail an in-flight save while newer edits wait for autosave and assert that `submitNow()` resubmits them. Fail the final flush separately. In both cases, assert the retained draft, editable note, zero submits, an unused session that is not retained, and successful user retry. Hold the note-action slot and assert the busy error. Change the vault during the save and assert zero submits. |
| Native contract | Exercise `notes.select` with valid and invalid IDs and a stale epoch. Verify the bootstrap field with a current note, with no note, and with a deleted note. Verify that `session.submit` is unchanged and that a Ref ID command still attaches exactly the specified item. |
| Jev | Extend `tests/runtime/unit_jev.cpp` and the Jev evaluation fixtures with the PRD examples. Check recipient-only requests, both questions together, valid recipient with malformed note answer, and recipient fallback. Check `@Name`, `@-`, and `/mcast` with a current note: only the note question, recipients kept, no recipient fallback notice, and an unchanged default target. Check that a self-note with `yes` attaches the note and starts no answer, that explicit prompts without a current note make no Jev request, and that other commands make no Jev request. Check disabled Jev, timeouts, and cancellation. Inspect call counts as well as the resulting routing. |
| Storage and generation | With a deterministic Jev result and captured provider inputs, cover compressed/full fallback while viewing L1/L2, an unchanged note (no write, no sync entry, no notice, but the note line is present), a changed name or text, a Ref ID with identical text (notice shown), rename while pending, and the exact byte limit plus one UTF-8 byte over. Delete or oversize the selected source after classification starts with an older copy already attached. Assert a specific error, no new notice or turn, no answer request, and an unconsumed draft. |
| End to end | Open *Trip plan*, then visit Settings and Events, open another conversation, and send a note question. Verify that the answer uses *Trip plan*. Preload copies of *Trip plan* and *Packing list*. Send about *Trip plan*, open *Packing list* while Jev is pending, then inspect the outgoing request and transcript. Repeat with all characters, `@Seneca`, and `/mcast`. Verify a single refreshed copy and notice, the note line only in the last user message, unchanged system prompt headings, the verbatim question in the transcript, no line on the next message, copies after reopen and source deletion, and their removal on `/clear`. Exercise the shared flow on both desktop hosts. |
