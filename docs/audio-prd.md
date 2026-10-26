# Background audio generation for one note

Status: Proposed PRD

Date: October 26, 2026

## Purpose

A user selects a voice, starts audio generation for one saved note, and continues using CHA. The application submits the note's paragraphs as a service batch, remembers the batch, monitors it, and saves the completed clips. Starting a batch and saving its audio produce events in **Settings → Events**.

A registered batch survives application restart. The user does not need to keep a note or dialog open, check the service, or download results manually.

This PRD replaces the earlier single-note modal-generation requirements in this file. The existing implementation is the baseline for playback, narration rules, service settings, and native conversion. The older [audio technical design](audio-design.md) describes the previous flow; its modal lifetime and lack of recovery do not apply to this change.

## Scope and simple defaults

- Change generation started from **Generate audio** on a single note.
- Keep the voice-selection dialog. Close it when the application accepts the start request locally. Uploading, remote generation, downloading, conversion, and saving then run without a modal dialog.
- Keep the current requirement that the note is saved and read-only.
- Keep one service request per narratable heading or paragraph, one batch per note, and one complete saved audio set per note.
- Keep the existing limit of one active audio-generation run in the open vault. This includes single-note and folder runs. Do not add a waiting queue or concurrent runs in this version.
- Keep folder conversion's current dialog, progress, sequencing, skipping, Stop behavior, and lack of restart recovery. Once admitted, a folder run works as it does today.
- Use the existing native platforms that support note audio. Do not add a platform or change playback controls, voice choices, codecs, or settings.

Here, “background” means work owned by the application while its vault is open. CHA does not install an operating-system service. The remote service may continue generating while CHA is closed; CHA resumes local work when the originating vault is opened again.

## User flow

1. The user chooses **Generate audio** for a saved, read-only note. As today, opening the dialog stops note playback and does not resume it automatically.
2. The dialog offers the existing voices: `boroda`, `grant`, `dzig`, `gerdt`, and `Kore`. Require an explicit selection each time. Keep **Close** and **Generate**.
3. If the note has audio, keep the existing replacement warning. The old audio remains available until a complete replacement is committed.
4. On **Generate**, validate the note, selected voice, required settings, and existing size limits. A validation failure stays in this dialog as an actionable error and submits nothing.
5. The application records the local attempt and accepts responsibility for it. The dialog closes as soon as that local acceptance succeeds, before waiting for any service request. Disable repeated submission while acceptance is pending.
6. The user can navigate, use chat, read other notes, open Settings, or close CHA. No progress, completion, or failure modal opens for this run.
7. The application uploads the requests and creates the batch. Once the returned batch ID is saved locally, it records the start event.
8. When the service finishes, the application downloads and validates every result, converts the clips, and saves the full set. Only then does it record the completion event.
9. Playback becomes available without restarting CHA. Refresh the currently displayed note's audio availability after commit. Do not start playback automatically.

Closing the setup dialog before Generate submits nothing. A definite local rejection leaves the dialog open with the voice selector and Generate available for correction and retry. Failures after acceptance appear in Events. A second start while a single-note attempt is active is rejected before upload with **Audio generation is already running for “Chapter 1” (note 42). Make that note editable to abandon the attempt.** Use the actual target name and ID. The unfinished record reserves the slot, including during startup recovery; both single-note and folder starts use the same admission check. A single-note start is also rejected while a folder run is active, with the existing folder-run rejection behavior.

A lost start reply does not prove rejection. Read native attempt status to resolve it; close the dialog if acceptance is confirmed. If status cannot be confirmed, keep the dialog dismissible, explain the uncertainty, and direct the user to Settings → Events. Do not automatically repeat the start request or report it as a definite failure.

### Proposed visual flow

Use the existing dialog styling and spacing. There is no visible panel title, progress area, elapsed timer, or explanatory legend.

```text
┌───────────────────────────────────────────────┐
│ Voice                                         │
│ [ Select voice                            ▾ ] │
│                                               │
│                       [ Close ] [ Generate ]  │
└───────────────────────────────────────────────┘
                       │ local acceptance
                       ▼
              Dialog closes; work continues.
```

When audio already exists, show **Existing audio will be replaced after successful generation.** between the selector and buttons. Validation errors use the existing inline error treatment. Preserve the dialog's accessible name, **Generate note audio**.

Events use the existing timestamp, severity icon, and message rows. For example, with the newest event first:

```text
✓  Oct 26, 2026, 10:08 AM
   Audio ready for “Chapter 1” (note 42): 20 clips saved.

ⓘ  Oct 26, 2026, 10:02 AM
   Audio batch started for “Chapter 1” (note 42):
   20 clips, voice Kore.
```

These sketches describe the proposed UI for review. UI implementation is outside this documentation task.

## Application-owned lifecycle

### Accept and register

Use the existing block extraction and validation. Headings and paragraphs retain their current ordinal keys and reading order. Do not narrate comments, empty blocks, code blocks, or the note's tree name. Keep the current limits of 5,000 Unicode code points per block, 1,000 requests per batch, and 8 MiB of serialized JSONL. Do not split or truncate oversized input. Ignore unused or obsolete configuration with a log warning; validate only settings needed for this run.

Persist a small attempt record in the originating vault database **before** accepting the local start request or sending service requests. It needs the attempt and note IDs, the note name captured at start, the expected clip count, the service origin, and the API key reference. The record's primary key is the attempt identity; no separate identity system is needed. The selected voice stays in the submission worker and is written to the start event; recovery does not need it. If the worker cannot start, remove the unsubmitted record and return a local rejection.

The remote batch ID is initially absent. Save it immediately after a successful creation response. Saving the batch ID and recording the start event must be one database transaction. This is the point at which the batch is **registered** and restart recovery is guaranteed.

If that transaction fails after the service returned a batch ID, retain the ID in the worker and retry the local registration write with the capped delay below. Keep the unfinished record and do not upload or create another batch. A process exit before the ID is saved still has the documented submission-uncertainty limit.

Resolve the saved API key reference through existing credential storage before each service request so replacing that key's value takes effect without restart. Do not copy API key values into attempt records or events. Keep the original service origin and key reference for this attempt; changes to the default Notes settings apply to future attempts. Creating a new key does not restore a deleted key's ID.

Do not persist a second copy of the note, request JSONL, remote progress counters, or downloaded clips for recovery. Before registration, the prepared request can remain in the worker's memory. After registration, the batch ID and expected result keys are enough to retrieve the results.

### Monitor and recover

The native application owns monitoring independently of the dialog and webapp component lifetime. Reuse the existing background-worker and service integration code.

- Poll a healthy registered batch at the existing 10-second interval. There is no overall batch deadline.
- Download results only after the service reports completion, and check both batch errors and individual results.
- On startup or vault open, load unfinished records and resume each known batch by its saved ID. Do not upload or create another batch for it.
- If the service finished while CHA was closed, download and save the results on reopening the vault.
- If CHA stopped during download or conversion, fetch the same batch's results again and repeat local processing. Byte-range resume and persistent partial downloads are unnecessary.
- On vault switch or shutdown, interrupt local work and retain unfinished records. Resume registered batches when that vault is opened again; handle records without a batch ID as described below. Never write clips or events into the newly selected vault on behalf of the old one.
- Ordinary sleep, disconnection, and UI reload do not make a registered batch fail or create a replacement batch.

A local record without a batch ID on startup or vault open means submission was interrupted before registration. Record a warning that submission did not finish and that a remote batch may exist, then retire that attempt. Do not automatically submit it again. This small marker makes an interrupted start visible without building a resumable upload system.

### Save and finish

Require exactly one valid result for every expected block key. Reject missing, duplicate, unexpected, failed, or invalid results. Keep the current native conversion and compressed audio storage.

The complete audio replacement, removal of the unfinished record, and completion event must commit in **one vault database transaction**. The transaction must verify that this exact attempt still exists and the target is still a read-only note before writing audio. A crash before commit leaves the previous audio and unfinished record; recovery can try again. A crash after commit leaves the new audio and one completion event, with no record to replay.

Do not expose a partial set as ready. If the user is playing this note's old audio when the replacement commits, stop that playback so clips from different generations cannot mix. Refresh availability without starting playback or interrupting another note's playback.

Deliver completion from the native application to the webapp for the affected note, reusing the existing note-audio refresh handling. It must work after the dialog unmounts. Stop and invalidate that note's outstanding playback loads before they can use replacement clips; a completion for another note must not stop the current playback. The technical design will define the notification and clip-response ordering.

Completed and terminally failed attempts need no separate history table. Keep their outcome in the existing Events history. Temporary files retain the existing cleanup behavior; add no cache or expiry policy.

## Events and errors

Events belong to the originating vault and are visible when the user opens **Settings → Events**, including after restart. Keep the existing list layout and retention limit of 200 events. Trimming an event must never remove an unfinished batch record. A live progress feed or new event-view controls are outside this change.

| Outcome | Severity | Message and timing |
| --- | --- | --- |
| Batch registered | Information | `Audio batch started for “Chapter 1” (note 42): 20 clips, voice Kore.` Record after the service accepts the batch and its ID is persisted. |
| Audio committed | Success | `Audio ready for “Chapter 1” (note 42): 20 clips saved.` Record with the complete audio commit, not when remote generation merely finishes. |
| Terminal failure | Error | Identify the note, failed step, and actionable reason, for example: `Could not generate audio for “Chapter 1” (note 42): the service returned an incomplete batch.` |
| Submission uncertain | Warning | Identify the note and say that submission could not be confirmed and starting again may duplicate remote work. |
| Target no longer eligible | Warning | Identify the note and say that generation was abandoned because the note was made editable or deleted. |
| Recovery needs attention | Warning | Identify the note and the connection, credential, or local storage problem. Say that the attempt is retained and give the applicable recovery action below. This is not a final outcome. |

Use the note name captured at the start and its ID so events remain understandable after a rename or deletion. Do not include note text, credentials, secret upload URLs, or raw service responses in events. Detailed diagnostics belong in logs.

Record one start and one final outcome for a registered attempt. Restarting, polling, or retrying a download must not repeat those lifecycle events. A monitoring session may also record one recovery warning; suppress repeated warnings for that attempt until the vault is reopened. This suppression can stay in memory. Do not create events for each paragraph, poll, or intermediate phase.

### Failure rules

- **Validation or local acceptance fails:** show the error in the still-open setup dialog. No service work has started.
- **Upload or batch creation fails:** record the error and retire the local attempt. Do not automatically repeat these writes. If acceptance by the service is uncertain, use the warning above.
- **Polling or downloading encounters a network failure or HTTP 5xx:** keep the registered batch, including for HTTP 503. Reuse the existing read backoff of 10, 20, and 40 seconds, then continue at a capped 40-second interval. Successful reads return to the normal cadence. Do not discard a known batch just because three reads failed. Record the recovery warning if the initial retries are exhausted. This changes retry exhaustion only for single-note background runs; folder behavior stays unchanged.
- **An existing saved key is rejected with HTTP 401:** retain the batch and retry on the same capped cadence. The warning directs the user to replace that key's value in Settings → API keys. Each request resolves its current value.
- **A service request is still needed, but the saved key reference has been deleted:** record a terminal error and retire the attempt. Keep key deletion available. Explain that the remote batch may continue and that a new run requires a configured key. Do not wait for an ID the user cannot restore or silently use a different key. If complete results are already available locally, finish saving them without requiring a credential.
- **The service returns another HTTP 4xx or an invalid response, the batch or result no longer exists, the service reports a terminal batch/item failure, or results cannot be validated or converted:** record a terminal error and retire the attempt. Preserve the previous complete audio. A new generation requires another explicit user action.
- **Local storage is temporarily unavailable:** retain the unfinished record and previous audio, log the failure, and retry with the same capped delay. This includes disk full and temporary file failures during download or conversion; malformed audio and unsupported conversion remain terminal. A retry may reuse complete temporary files still owned by the worker; after interruption, download and convert the same batch again. Record one recovery warning if storage allows it. Do not report success or lose recovery information. If the database cannot accept an event, log that failure and keep the unfinished record.

Retiring an attempt and writing its final error or warning must be atomic where the database is available. Every final event, including success and abandonment, requires finding and removing the exact attempt record in that transaction. If another path has already removed it, discard late work without another final event. This also covers a worker failure racing with Make editable or deletion. No automatic remote cancellation, deletion, or replacement batch is required.

## Note changes while work is pending

Reuse the note's existing read-only rule to protect narrated text. Reading, scrolling, highlighting, comments, renaming, and moving the note remain available and do not invalidate the run.

Making the target note editable abandons its unfinished audio attempt, even if the user later makes it read-only again. Deleting the note, including through folder deletion, also abandons it. Record the warning and remove the unfinished record with the corresponding note change. A late service reply or worker result must not revive it or save audio for a new attempt. Remote work may continue because the service has no cancellation API.

Making the note editable is also the existing action for abandoning a permanently stalled attempt, such as one whose original service address is no longer available. Recovery warnings identify the target note and mention this option. Abandonment releases the local audio slot; it does not cancel the remote batch. No new cancellation control is added.

This permits normal note operations without introducing document snapshots, content fingerprints, revision tracking, or paragraph remapping. It does not add stale-audio handling for edits made after a completed run.

## Implementation boundaries

Use a small vault-backed record and the existing background worker, service client, native converter, audio storage, and Events history. Adapt the existing storage operations to share the lifecycle transactions described above. A general event bus, general job framework, scheduler service, durable event queue, and second audio store are unnecessary; the note-specific completion notification is sufficient.

Keep folder orchestration on its current path. Shared helpers may be reused, but this feature must not make folder runs persistent, close their dialogs early, change their retry rules, or produce a new sequence of folder events.

The service currently has no creation idempotency token or lookup-by-client-key operation. If it accepts a batch and CHA loses the response, or stops before saving its ID, automatic recovery cannot identify that batch. Preserve this honest boundary: recovery is guaranteed for **registered** batches. A local database transaction cannot make a remote POST atomic. No service protocol changes are required for this version.

## Acceptance criteria

1. A valid single-note start closes the dialog after durable local acceptance. Upload, generation, download, conversion, and commit proceed while the user uses the application. No later modal opens.
2. Invalid settings, an empty note, an oversized block, or an oversized batch leave the setup dialog open with an error and send no service requests. Definite rejection permits correction and retry. Double clicks create at most one active attempt. Lose the start reply: the UI checks native status and never automatically submits again.
3. A registered batch produces one start event with the correct note, voice, and clip count. Successful download, conversion, and commit produce one success event. Both are visible in Settings → Events.
4. Restart after registration, during remote generation, during download, and during conversion. Each case resumes the same batch and finishes without a new upload or creation request. A batch completed while CHA was closed is handled on reopening.
5. Restart immediately before and after the final database transaction. The visible result is either the previous complete audio with recoverable work, or the new complete audio with one success event. No partial audio or duplicate completion event appears. Fail the registration write after batch creation: the worker retains the returned ID and retries only that local write.
6. Interrupt submission before a batch ID is saved. Restart records an uncertainty warning and does not resubmit. A lost creation response gives the same warning during normal operation.
7. Keep the service offline beyond the old three-retry limit, then restore it. The registered batch remains and completes. Exercise HTTP 401 and 503 separately. Replacing the same saved key's value resumes monitoring without restart or regeneration. Deleting that key while further service access is needed ends the attempt with an error and releases the local slot.
8. Simulate missing results, item errors, corrupt audio, conversion failure, and temporary local storage failure. No success event is recorded early; existing audio remains intact. Terminal failures produce an error event, while recoverable storage failure retains the batch.
9. Switch vaults while work is running. The old vault's registration survives, no old result reaches the new vault, and reopening the old vault resumes processing. An interrupted submission without a saved batch ID is resolved on reopening. A UI reload does not stop native work.
10. Highlight, comment on, rename, and move the target during generation; completion still succeeds. Make it editable or delete it; the attempt is abandoned with a warning and late results are ignored, including after restart or the start of a new attempt for the same note. Race a worker failure with abandonment: only one final event is written. The slot is released so another audio run can start, and a blocked start identifies the target and explains this exit.
11. Completion refreshes playback availability for an already-open note without autoplay. Replacing audio stops playback of that note's old set and invalidates its pending clip loads. Another note's playback continues. Saved audio remains playable offline.
12. Run existing folder conversion and playback checks. Folder dialogs, Stop, sequencing, skipping, retries, and recovery behavior remain unchanged. A pending or recovered single-note attempt rejects a folder start, and an active folder run rejects a single-note start. Event-history trimming cannot affect unfinished work. Recovery warnings do not repeat on each poll.

## Deferred

Concurrent audio runs, a local waiting queue, asynchronous folder conversion, folder recovery, a jobs screen, per-paragraph progress events, notifications outside Events, cancellation controls, automatic resubmission, partial-result playback, upload/download checkpointing, and new service APIs.
