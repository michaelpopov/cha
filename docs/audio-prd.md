# Note audio generation and playback PRD

Status: Revised PRD draft  
Date: October 24, 2026

CHA will generate audio for a saved, read-only note and keep one compressed clip for each title and paragraph in the note's database. Users can start listening at any paragraph, pause and resume with Space, and see each paragraph when its audio starts.

The first version is a small macOS feature intended to gather practical experience. Generation runs through a modal dialog that stays open until remote generation, downloading, native conversion, and database storage finish. Windows support, background job management, and handling edited notes are deferred.

## Confirmed scope

- **Notes settings contains only the service URL and an API key reference for audio.** There is no global audio voice setting.
- **Generate audio opens a modal dialog.** The user selects a voice for this note and explicitly starts generation.
- The dialog shows available service progress, normally retrieved every 10 seconds, the current local phase, and elapsed time for the entire operation. Temporary read failures use the retry rule below.
- When all work finishes, the timer stops and the user chooses **Close**. There is no batch Cancel action.
- Playback is available only for read-only notes. **Right-click → Play from here** starts at the selected title or paragraph and continues through the rest of the note.
- After playback starts, the menu action becomes **Stop playing**, including while paused. Stop before choosing another starting position.
- Space pauses; Space again resumes at the same audio position. There are no new playback toolbar buttons.
- Reveal each paragraph when its clip starts. Do not scroll progressively within a paragraph.
- Native macOS APIs convert the service's 24 kHz WAV files to compressed audio. The intended output is AAC in an M4A container.
- Changes to note content after generation are outside this version. Do not build invalidation, revision tracking, or automatic regeneration.

## Service setup

The user opens **Settings → Notes**. Keep the existing text-compression settings and add these audio settings in the same form:

| Control | Behavior |
| --- | --- |
| Audio service URL | The gem_audio origin, such as `http://127.0.0.1:18080`, without `/v1beta` or another path. Accept HTTP and HTTPS, including a private network host. |
| Audio API key | Select an existing key reference from CHA's API key settings. Reuse existing credential storage. |
| Save | Save settings without contacting the service or generating audio. |

An audio configuration is not required to read notes or play saved clips. Missing configuration is reported when generation is requested. Unused or obsolete settings must not block an otherwise valid operation; log a warning instead.

Do not add model, codec, bitrate, seed, token budget, or voice controls to this settings screen. The implemented service uses the public model `gemini-3.8-flash-tts`.

## Generate audio for one note

### Before starting

The user saves the note, makes it read-only, and chooses **Generate audio** in the existing note menu. Opening the dialog stops note playback, including paused playback or playback between clips. Closing the dialog does not resume it. The modal contains:

- A **Voice** selector: `boroda`, `grant`, `dzig`, `gerdt`, and `Kore`.
- **Generate** and **Close** buttons.

Show only the voice IDs, without descriptions. The user selects one voice for all clips in this note. Different notes can use different voices.

Require an explicit voice selection each time the dialog opens, including when the note already has audio. Do not preselect or recall a previous voice. Selecting a voice does not start work or change other notes.

If audio already exists, show the actionable consequence **Existing audio will be replaced after successful generation.** A successful run replaces that note's audio set. A failed run preserves the previous complete set. There is only one saved audio set per note.

No panel title or explanatory legend is added above the controls. The dialog has an accessible name, **Generate note audio**, without a redundant visible heading.

### While running

Choosing **Generate** captures the saved note and selected voice and starts the elapsed timer at zero. The timer shows wall-clock time for the whole run, from **Generate** through the final database commit, as `mm:ss` or `h:mm:ss`. It does not restart between phases. It is a UI display, not a deadline or cancellation mechanism.

The dialog remains modal. The user cannot edit the voice, start a second run, navigate to another note, or dismiss the dialog with Escape, a background click, or Close while work is active. The application must continue to repaint and show progress; this must not block the UI thread. No Cancel button is provided.

| Phase | What the user sees |
| --- | --- |
| Uploading | **Uploading…** |
| Waiting for the service | **Queued** when `metadata.state` is `BATCH_STATE_PENDING`. |
| Generating | **Generating · 8 of 20 complete** when `metadata.state` is `BATCH_STATE_RUNNING`. Completed means `successfulRequestCount + failedRequestCount`; the total is `requestCount`. |
| Downloading | **Downloading…** |
| Converting and saving | **Saving…**, until the complete audio set is converted and committed to the database. |

Poll the service for batch status and progress every 10 seconds while waiting for completion, except during the retry delays below. Use the batch state, not `pendingRequestCount`, to choose Queued or Generating: the service counts the in-flight item as pending. Completed counts include failed items and do not imply successful audio. Download results only when `done` is `true`. If progress counts are unavailable, show the phase and running timer. Do not invent a percentage, remaining-time estimate, or simulated progress in the application.

For a network or transport failure, or HTTP 500, while polling or downloading, retry the same read up to three times, after 10, 20, and 40 seconds. Keep the dialog active and the elapsed timer running during these retries. A successful read resets the retry count; successful polling returns to the normal 10-second interval. Keep the received batch name and result file name for this run. Do not create a replacement batch or upload as part of a read retry. If all three retries fail, end the run with a terminal error. Other HTTP errors, upload failures, and batch-creation failures are terminal without automatic retries. These client retries do not limit how long a healthy queued or running batch may take.

### Completion and failure

On success, show **Audio ready · 20 clips**, the stopped total time, and **Close**. Keep the dialog open until the user closes it. Completion does not start playback. The clips are available after Close and can be played without the service or network.

On a terminal error, stop the timer, show the failed phase and an actionable error, and enable **Close**. Do not present a partial set as complete audio. For this first version, any failed service item, missing or duplicate result, invalid WAV, conversion failure, or storage failure makes the run unsuccessful. Preserve an existing audio set. The user can close the dialog and start another run after correcting the problem. A client failure does not cancel a remote batch that was already accepted.

Do not automatically repeat batch creation. A batch marked succeeded can contain item errors; inspect every result.

## Reading order and limits

Use the saved structured note document. Screen wrapping does not create paragraphs. Generate from the original note, not the compressed text used for chat context.

| Content | First-version rule |
| --- | --- |
| In-document titles and headings | One clip per nonempty heading. |
| Paragraphs | One clip per nonempty paragraph, in document order. |
| Lists and quotations | Include their paragraph and heading blocks once, in reading order. Do not add spoken bullet symbols. |
| Inline formatting and links | Read visible text without markup or appended link targets. |
| Line breaks inside a paragraph | Keep them in that paragraph's request. |
| Empty blocks, images, horizontal rules, code blocks, and CHA comments | Skip. |
| Annotation-only views | Not a narration source. |
| Note name in the tree | Not an extra spoken title. A title is read when it appears in the document. |

For the quickest first version, use **one service request per heading or paragraph and one batch per note**. Validate the service limits before upload: at most 5,000 Unicode code points per text block, 1,000 requests, and 8 MiB of serialized JSONL. If a note exceeds a limit, identify the offending block or note limit in the dialog without submitting work. Never truncate or omit text to make it fit.

At least one narratable block is required. If filtering leaves no blocks, show **No text to narrate**, stop the timer, and enable **Close** without uploading or creating a batch. Preserve any existing audio set.

Automatic long-paragraph splitting, joining several WAVs into one clip, and dividing a large note into several batches are deferred. Long clips can have variable narration quality even within the accepted service limits; practical listening will guide the next version.

Build the block list in one place: the webapp, from the same document it renders. **Generate** sends these block texts, and playback uses the same list. Each clip's key is its block's position in this list. Repeated paragraphs are separate blocks. Do not depend on result arrival order.

## Playback

### Start and stop

Playback works only for read-only notes. When playback is stopped, the user right-clicks a title or paragraph with saved audio and selects **Play from here**. The clicked block is the starting point even if another passage is selected. Keep existing Copy, Highlight, and comment actions. Opening or dismissing the menu does not change playback.

Start at the beginning of that block, then play all following clips in note order, without an added pause between clips. Stop at the end of the note; do not continue into another note.

After playback starts, replace **Play from here** with **Stop playing** in the note's context menu. Keep **Stop playing** while playback is paused or between clips. Choosing it ends playback and returns the action to **Play from here**. To start at another position, the user must stop first, then right-click the desired block and choose **Play from here**. Reaching the end or encountering a playback error also returns the action to **Play from here**.

Temporarily highlight the current spoken block. Use a visual treatment distinct from saved annotations and selection. Pause retains the highlight; stopping or completion clears it. Playback never writes a highlight into the note.

Do not add Play, Pause, progress, or audio settings controls to the note toolbar.

### Pause and resume

After playback has started, pressing **Space** pauses at the current position. Pressing Space again resumes there, then continues through the remaining clips.

Handle Space when focus is in the read-only note's reading area. While playback is active or paused, prevent Space from scrolling the page and ignore held-key repeats. Leave Space unchanged in inputs, dialogs, and menus. An idle note does not start speaking on Space. Pausing between clips prevents the next clip from starting until the user resumes; stopping prevents it from starting at all.

Clicks keep their normal behavior and do not pause or resume playback. Scrolling alone does not pause audio.

If a saved clip cannot be read or played, stop and show the error. Do not silently skip text or start generation.

### Show the current paragraph

When a clip starts and the beginning of its block is not visible, scroll the reading area to show it. Do not scroll during a clip.

Leaving the note, making it editable, switching vaults, or closing CHA stops playback. Reopening does not autoplay or restore a persistent listening bookmark. Starting note playback stops chat speech and starting chat speech stops note playback, including when the other source is paused or between clips. The stopped source cannot resume from its old position; it requires a new explicit start. Thus, Space in the note cannot resume stopped note playback over chat speech.

## Storage and native conversion

Store compressed clip bytes, their format, and reading order in the existing vault database. Audio is persistent note content, not an expiring cache. Existing database backup and restore must carry it with the note. Deleting a note deletes its local clips.

Use native macOS APIs to read the returned mono PCM16 24 kHz WAV and encode AAC in an M4A container, with MIME type `audio/mp4`. Do not require FFmpeg, an encoder command-line tool, or a remote conversion service. Keep the 24 kHz sample rate if the native encoder accepts it. Release temporary WAV and conversion data after completion or failure; do not retain WAV copies in the database.

Use one internal encoding profile, without a user-facing codec or bitrate setting. Verify very short headings, normal paragraphs, and long paragraphs. Clips must decode completely, retain pitch and pace, and play with clean boundaries. Exact sample-continuous playback is not required, but loading and encoder padding must not clip words or create distracting extra pauses.

Do not implement stale-audio detection, document fingerprints, remapping after edits, clip version history, or automatic regeneration. This version assumes a generated note remains unchanged. Behavior after changing its narrated content is deferred. The modal prevents changes through normal note navigation during a run; ordinary storage checks must still avoid writing to a deleted note or another vault.

## Service integration boundaries

Use the current gem_audio file workflow: upload JSONL, create a batch, poll, download results, and decode WAV. Authentication uses `x-goog-api-key`. The selected note voice is repeated in each item's `generationConfig.speechConfig.voiceConfig.voice`. Use the implemented [client manual](../../gem_audio/manual.md) and [protocol parser](../../gem_audio/src/gem_audio/protocol.py) as the contract.

Keep credentials and secret upload URLs out of logs and user-visible errors.

## Acceptance scenarios

1. Notes settings saves a URL and existing key reference, with no voice control. Notes remain readable without service configuration.
2. Generate two notes with different voices. Each batch uses its selected voice ID for every block. Reopening either dialog requires a fresh selection from the plain list of voice IDs.
3. Generate opens the modal but sends no request until a voice is selected and Generate is pressed. Opening it stops active or paused note playback, including between clips; closing it does not resume playback. Starting twice cannot create duplicate work.
4. During a run, show service progress when available and the correct local phase. A running batch with pending items shows Generating, not Queued. Count successful and failed items as complete, but do not treat completion counts as success. Download only when `done` is `true`. The UI stays responsive. Escape, background clicks, and Close cannot dismiss active work.
5. Keep the timer running through upload, queueing, generation, download, conversion, and storage. Stop it only on success or terminal failure. Success shows Close and does not autoplay.
6. Convert 24 kHz PCM16 WAVs with native macOS APIs. Decode the stored compressed clips and verify format, duration, short-clip integrity, and playback in CHA.
7. In a read-only note, start at a middle paragraph, play the following headings and paragraphs once, and stop at the note's end. The menu shows **Stop playing** while active or paused. Stop before selecting a different starting position. Playback is unavailable in editable notes and stops when a note becomes editable.
8. Press Space to pause and resume at the same position, including preventing advancement while paused between clips. Reveal each block when its clip starts; for a tall paragraph, reveal its beginning without progressive scrolling. Clicks, text selection, link clicks, and Space in inputs retain their normal behavior.
9. Simulate a failed item, a corrupt download, a native conversion error, and a database error. Show the error and stopped timer, enable Close, and preserve a previous complete audio set.
10. Reject oversized blocks or batches before upload, without truncating text. A note with no narratable blocks shows No text to narrate, stops the timer, enables Close, sends no upload or batch request, and preserves existing audio.
11. Reopen CHA with the service offline and play previously saved clips. No remote service is required for playback.
12. Simulate temporary network failures and HTTP 500 responses during polling and downloading. Retry the same read after 10, 20, and 40 seconds, with the timer running and no replacement batch. A successful read resets the retry count. Exhausted retries or a non-retryable error stop the timer, enable Close, and preserve existing audio. Upload and batch-creation failures are not automatically retried.
13. Start chat speech while note playback is active, paused, or between clips; note playback stops and Space cannot resume it. Starting note playback likewise stops active or paused chat speech. Neither stopped source resumes automatically.

## Notes

- The service has no cancellation endpoint. It can remain queued or retry inference for a long time. The user is expected to leave CHA running until completion. Quitting or crashing can leave a remote batch running. This version does not persist a resumable job or recover it after restart, and it does not claim that quitting cancels the service. Saved complete audio remains durable; unfinished work is not shown as ready.
- Remote uploaded text and results remain on the service until its operator clears them. Local replacement or note deletion does not remove remote resources.
- Apple exposes native audio-file reading and writing through [AVAudioFile](https://developer.apple.com/documentation/avfaudio/avaudiofile). A local feasibility check on October 24 converted mono PCM16 WAVs of 0.1 and 2 seconds to AAC/M4A at 24 kHz and 64 kbit/s. Decoding returned exactly 2,400 and 48,000 frames respectively. No resampling was needed. This establishes a working native conversion path on the tested Mac; narration quality and in-app playback still need validation. Short M4A clips can exceed the size of their WAV input because of container overhead, so assess compression over a representative whole note. Windows and its sample-rate constraints do not affect this version.

## Deferred work

Windows and ChaWeb support; background generation; cancellation; restart recovery; per-clip retries; partial-result playback; paragraph splitting and multi-batch notes; edited-note handling; playback toolbar controls; an added pause between clips; speed adjustment; persistent listening bookmarks; scrolling within a paragraph; exact word highlighting; audio export; multiple stored voice versions; and folder generation.
