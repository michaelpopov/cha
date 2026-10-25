# Note audio technical design

Status: Implementation design draft

Date: October 24, 2026

Requirements: [audio-prd.md](audio-prd.md)

## Scope and approach

Implement the PRD on macOS by completing the audio code already present in the working tree. Use the existing native bridge, application worker facility, vault database, credential store, and media-resource transport. One modal owns one generation attempt. One note has one complete saved audio set.

The PRD is authoritative where the current code differs. This document describes proposed changes; it does not establish that the current implementation meets the requirements. Windows and ChaWeb support, editable-note handling, resumable jobs, background generation UI, and the other deferred PRD features remain out of scope.

The existing `background_jobs` facility runs blocking work off the UI thread. Using it does not introduce the deferred background-generation feature: the modal still remains open until the attempt finishes. Do not add a job queue, job database, separate audio cache, expiry policy, document fingerprint, or version history.

Before implementing visible changes, present the affected screens and states for explicit visual approval as required by [AGENTS.md](../AGENTS.md). This technical design is not UI approval. Follow the PRD's controls and copy, without adding panel headings or explanatory legends.

## Existing code and intended changes

| Area | Existing path | Intended change |
| --- | --- | --- |
| Settings | `webapp/src/components/NotesSettings.tsx`, `src/app/settings_operations.cpp`, `src/workspace/workspace.cpp`, `src/workspace/workspace_config_editor.cpp` | Retain `audio_url` and the `audio_api_key` reference; retain compression settings and warning behavior for unused fields. |
| Dialog and menu entry | `webapp/src/components/NoteAudioDialog.tsx`, `NotesTree.tsx` | Build blocks from the selected note's saved document, stop note playback on opening through the existing audio-change notification, require a fresh voice, and match the PRD's phase and timer behavior. |
| Bridge | `webapp/src/api/client.ts`, `nativeClient.ts`, `src/bridge/notes_dispatch.cpp`, `bridge_router.cpp` | Extend `noteAudio.start` with block texts; adapt status and info responses. Reuse source and release methods. |
| Native operation | `src/app/note_audio_operations.cpp`, `application.h`, `application_internal.h` | Keep one in-memory attempt and the existing worker; enforce lifecycle checks and publish terminal status after commit or failure. |
| Service client | `src/media/note_audio.cpp`, `note_audio.h` | Keep the file workflow; send no API key to the upload URL and remove the current same-origin prefix check; add classified read retries, optional progress counts, and complete result validation. |
| Native conversion | `src/media/note_audio_codec.cpp`, `CMakeLists.txt` | Retain AudioToolbox conversion and the Apple framework linkage; verify complete AAC/M4A output. |
| Storage | `src/storage/note_storage.cpp`, `note_storage.h`, `src/workspace/workspace_config_store.cpp`, its header | Retain transactional replacement; replace the stored `voice` column with `mime_type`; remove the separate C++ document-to-block extractor. |
| Playback | `webapp/src/notes/useNoteAudio.ts`, `speechPlayback.ts`, `textToSpeech.ts`, `NotesScreen.tsx` | Use Space, explicit Stop, block-start scrolling, and speech ownership that survives pauses and clip transitions. Remove click-to-pause and progressive scrolling. |

Use the repository's existing DTO conventions when changing shared shapes. Update `resources/dto.yaml` and regenerate `webapp/src/api/schema.d.ts` when a changed shape is defined there; do not maintain conflicting handwritten and generated definitions.

## One block list

Extend the existing `noteAudioBlocks` helper in the webapp to return ordered entries with `{ position, size, text }`. Generation and playback must call this same helper on a ProseMirror document constructed with the existing note schema. Do not introduce a second JSON traversal in the native layer.

The traversal follows document order:

1. Skip code blocks, images, horizontal rules, and entire CHA comment blocks, using the existing comment predicate.
2. For each heading or paragraph, collect inline text in order. Convert `hardBreak` nodes to `\n`; ignore marks and image content. Link text remains text, without appended targets.
3. Omit a block if its extracted text is whitespace-only. Use trimming only to decide emptiness; preserve the original extracted text in the request.
4. Emit the block once and do not traverse its inline children again as blocks. Traverse list and quotation containers normally.

The array index is the clip ordinal. Its canonical decimal string (`"0"`, `"1"`, ...) is the service request key. ProseMirror positions are used only to locate the clicked or spoken block; they are not service keys or database identities. Repeated text yields separate entries.

When Generate is pressed, capture the voice and load the selected note's saved document through the existing note client (`client.getNote(id)`). Use this one path for the active note and for a note that is not open. Construct a ProseMirror node with the schema from `notesExtensions()` and call `noteAudioBlocks`; `NotesTree` already reads saved documents this way for Compress. Generation requires a read-only note. In a read-only note only highlight marks and comment paragraphs can change, and the block list ignores both, so the saved document yields the same blocks as the open editor. Fail preparation if loading fails. Do not add save coordination, a hidden editor, or another document store.

`NotesTree` prepares the blocks itself; no callback passes through the Notes parent wiring. Stop note playback before showing the modal, even if generation targets another note: send the existing `cha-note-audio-changed` notification when the dialog opens. The playback hook stops on any such notification and reloads its stored clip count only when the notification names its own note. Send the same notification after Close to refresh saved clip availability after a successful run; do not add an event bus.

Playback can recompute block positions with the same helper when it starts or locates a block. Comments and annotations remain excluded and do not create clip ordinals. Changes to narrated content remain outside scope; no content comparison, remapping, or invalidation is added. Known effect: if a note is made editable, changed, and made read-only again, Play from here plays the old clip for each ordinal while it highlights the new block at that ordinal, without a warning. Extra new blocks have no Play from here; extra old clips never play.

## Settings and credentials

Keep the existing Notes settings fields and API key selector. Persist only the service origin and key reference for audio. Saving settings performs no network request. Resolve the key value in the native application when generation starts; never send it to the webapp.

Validate required generation settings when starting an attempt. Accept HTTP or HTTPS origins, including loopback and private hosts; normalize a trailing slash. Reject paths, embedded credentials, queries, or fragments in the configured origin. Missing audio configuration does not affect note reading, saved clip lookup, or playback. Ignore unused or obsolete settings and use the existing warning logger.

The voice is a required argument for each run and must be exactly `boroda`, `grant`, `dzig`, `gerdt`, or `Kore`. The dialog starts with no selection every time. Remove the current assignment of `info.voice` to the selector and remove descriptions from the voice options.

## Native bridge and attempt state

Keep the existing method names and context-epoch mechanism:

| Method | Input | Result |
| --- | --- | --- |
| `noteAudio.info` | `{ id }` | `{ count }`, derived from stored clips; no service call. The native client already offers `noteAudio` only on macOS. |
| `noteAudio.start` | `{ id, voice, blocks: string[] }` | Initial attempt status after admission. |
| `noteAudio.status` | `{ id }` | Current status for that note in the admitted vault epoch. |
| `noteAudio.source` | `{ id, ordinal }` | Existing `MediaResource` shape, with stored MIME type and local URL. |
| `speech.release` | `{ resource_id }` | Existing release response. |

The native start method validates, once and before it launches the worker, that the note exists, is a normal read-only note, and belongs to the admitted context. It also checks the argument types, the voice, and the limits in [Validate before upload](#validate-before-upload), then serializes the JSONL and passes the ready string to the worker. Reject a second start while an attempt is active, in addition to the dialog's immediate double-click guard. Do not extract text again from the stored document.

Keep one in-memory status record in `Application::Impl`, protected by the existing lifecycle mutex. Retain it until the next attempt or context teardown so the dialog can read the terminal result. No attempt ID, status history, or persistent job record is needed for this modal flow.

Status contains:

- `id`: note ID; the existing native state also retains the vault epoch.
- `phase`: `uploading`, `queued`, `generating`, `downloading`, `saving`, or `complete`.
- `completed` and `total`: optional progress counts. Omit unavailable service counts instead of substituting zero. On success these contain the committed clip count.
- `done` and `error`: terminal state and a safe error message. On failure retain the phase that failed.
- `finished_at_ms`: present only when `done` is true; the native completion time in Unix milliseconds, captured after the final commit or at terminal failure.

Remove the separate `failed` display field; failures contribute to completed service work, and final result inspection determines whether the attempt succeeds. Map conversion and database work to the single `saving` phase. There is no `preparing` phase: start returns the initial status with phase `uploading`. Before the first status arrives, the dialog shows only the running timer.

The dialog has ready, running, and terminal states. Ready allows voice selection and Close; Generate requires an explicit voice. Running starts immediately on the Generate action, disables voice changes and duplicate starts, and prevents Close, Escape, background dismissal, and navigation. Terminal success shows Audio ready with the committed clip count; terminal failure shows the failed phase and error. Both stop the timer and enable Close, leave the dialog open, and require closing and reopening for another attempt. There is no Cancel control or automatic playback.

The dialog reads local status about once per second with one request at a time. This is separate from remote service polling every 10 seconds. A local status-read failure does not mean native generation failed: keep the dialog running and locked, keep its timer running, show the communication error, and retry the same status read on the existing one-second cadence. Clear the communication error after a successful read. Do not apply the remote service's three-retry limit to local status reads or declare generation finished because its status is temporarily unavailable.

A preparation failure or an explicit start rejection that confirms no worker was launched can end the dialog's wait. A lost or timed-out start reply cannot: read the existing native status without resubmitting start. A retained terminal result whose `finished_at_ms` precedes this dialog's Generate action belongs to an earlier attempt and must not finish this one. Missing or earlier status alone does not prove that a pending start will not run; keep waiting until the native outcome is known or the context is torn down. No new attempt ID, retry timer, or recovery store is needed. An application/context teardown follows the existing teardown path and does not create recovery work.

### Timing and lifecycle

Capture `started_at_ms = Date.now()` in the dialog at the Generate action, before preparation and bridge dispatch. The dialog owns the running display timer and continues it through retries, conversion, commit, and temporary loss of local status. When it reads a terminal native status, stop that timer and set the final elapsed value to `max(0, finished_at_ms - started_at_ms)`. Both values use the same machine's wall clock. This includes preparation but excludes the delay in receiving completion, even when polling is delayed by several seconds. Replace the current native `elapsed_ms` with the single terminal `finished_at_ms` value; do not add a native ticking timer. For a preparation failure or a confirmed rejection before worker launch, stop the display using the dialog's current time. A communication error alone never freezes the timer.

Use the existing worker for HTTP, waits, parsing, encoding, and database saving. Do not run these operations on the macOS UI thread. Capture configuration and the serialized JSONL once at start; release application locks during network and conversion work. Check the vault epoch before progress publication, before storing, and before terminal publication. An old worker must not overwrite a new context's status or write to its store.

Keep the existing shutdown interruption flag and worker join behavior. It interrupts local transfers and waits during application teardown; it is not a user Cancel action and does not cancel the remote batch. There is no overall generation deadline. All ordinary terminal paths release temporary files; a process crash can leave operating-system temporary files, with no new startup recovery or cleanup service.

## Service operation

Follow the local [gem_audio manual](../../gem_audio/manual.md) and [protocol parser](../../gem_audio/src/gem_audio/protocol.py). Keep service access in C++ using the existing curl helpers.

### Validate before upload

Native start performs these checks once, before it launches the worker:

- Require 1–1,000 block texts. Zero blocks produces `No text to narrate`, a stopped timer, and Close, with no upload or batch creation.
- Require 1–5,000 code points per block. Count Unicode code points, not UTF-8 bytes or JavaScript UTF-16 code units. Do not normalize or truncate text. The bridge JSON parser already rejects invalid UTF-8 and lone surrogates, so add no separate encoding check.
- Serialize the complete UTF-8 JSONL input and require at most 8,388,608 bytes, including separators and the final newline. Identify the offending one-based block or note limit in errors.

Known effect: a note with a block above 5,000 code points or with more than 1,000 blocks cannot get audio in this version; the user splits it by hand.

Use the existing JSON serializer, one compact object per physical line. Each record has exactly this structure, with its ordinal, text, and selected voice substituted:

```json
{"key":"0","request":{"contents":[{"role":"user","parts":[{"text":"Paragraph text"}]}],"generationConfig":{"responseModalities":["AUDIO"],"speechConfig":{"voiceConfig":{"voice":"Kore"}}}}}
```

### Upload, create, poll, and download

1. Start the upload with `POST /upload/v1beta/files`, `x-goog-api-key`, `{}` metadata, and the manual's resumable-upload headers, including the exact JSONL byte length.
2. POST the whole JSONL file once to the returned secret upload URL with offset zero and `upload, finalize`. Header names are case-insensitive. Use the returned URL as specified by the service; authenticate this transfer with the secret URL, without forwarding the API key to it. Remove the current check that the URL starts with the configured origin: the operator's public origin can differ, and no credential goes to this URL. Permit only HTTP(S), retain normal TLS verification, and do not follow redirects with credentials. Keep the URL out of logs and errors.
3. Capture `file.name`, then POST `{"batch":{"inputConfig":{"fileName":"files/..."}}}` to `/v1beta/models/gemini-3.8-flash-tts:batchGenerateContent`. Keep the returned batch name for this attempt.
4. Inspect the creation response, then poll `/v1beta/{batch-name}` every 10 seconds while `done` is false. `BATCH_STATE_PENDING` means Queued; `BATCH_STATE_RUNNING` means Generating. Parse decimal-string counts. Completed is successful plus failed; total is `requestCount`. If counts are absent or invalid, omit them. Never infer Queued from `pendingRequestCount`.
5. When `done` is true, inspect any operation error and obtain `response.responsesFile`. Download `/download/v1beta/{file-name}:download?alt=media` into the attempt's temporary directory. Validate resource names before building paths. A succeeded batch still requires per-item inspection.

Do not automatically retry upload start, upload finalization, or batch creation. A lost creation response is an ambiguous failure: report that another manual attempt may duplicate remote work. The remote batch may continue after any client failure. No remote delete or cancel request is available.

### Read retries

Add one small read-retry helper around polling and result downloads, not around the whole operation. Preserve error categories from the transfer function instead of reducing all failures to a generic string.

| Failure | Action |
| --- | --- |
| Network/transport failure or HTTP 500 on a read | Retry the same read up to three times, waiting 10, 20, then 40 seconds after each failure. |
| Other HTTP status | Terminal failure. |
| Invalid JSON, invalid resource/result, item error, invalid audio | Terminal failure. |
| Local file I/O, size-limit violation, conversion, or database failure | Terminal failure. |
| Application shutdown interruption | End local work through existing teardown. |

A successful read resets its retry count. Successful polling resumes the normal 10-second interval. Do not add a normal poll delay on top of a retry delay. Recreate/truncate the result file for each download attempt; a failed partial download must not be appended to the next response. A curl write failure caused by disk I/O or a size guard is a local failure, not a retryable network error.

Keep the current phase and elapsed timer during retries. Keep ordinary per-request connection/inactivity limits to detect a stalled transfer; they are not a batch deadline. Reuse shutdown-aware waits. Do not introduce jitter, configurable retry policies, partial-download resume, or automatic resubmission.

### Results and bounded memory

Download to a temporary file, then parse JSONL one record at a time. The service allows WAVs up to 32 MiB per item, so a whole-note result must not be loaded into memory as one string. Keep a bounded result-line buffer, one decoded WAV, and the vector of converted file paths indexed by ordinal. The vector also records which keys have been received; no separate result cache is needed.

Require exactly one successful result for every expected canonical key. Reject duplicates, unknown keys, missing keys, item errors, malformed Base64, wrong MIME type, and invalid WAVs. Use validated ordinals for local filenames, never arbitrary remote keys. Process records in any arrival order. Do not write a replacement database set until all expected clips have converted successfully.

Keep service error bodies and raw transport URLs out of user-visible errors. Report a safe phase, block number when available, and a useful action. Never log API keys or secret upload URLs.

## Native conversion and storage

Retain the current AudioToolbox `ExtAudioFile` implementation rather than introducing a Swift bridge or external encoder. Verify mono signed PCM16 at 24 kHz, decode through the native reader, and write AAC in an M4A container. Use the existing internal profile of mono, 24 kHz, 64 kbit/s. No user codec controls are needed. If the encoder rejects the profile, fail the attempt with a conversion error; do not silently change pace or pitch.

Read and write frames in bounded buffers. Reject empty or truncated input, and check output finalization errors before treating a clip as complete. Use native decoded-frame information to verify that the WAV was read fully. Close input/output handles before deleting or storing files. Remove each temporary WAV after conversion; retain compressed temporary files only until the atomic save completes or fails.

Keep the `note_audio` table and primary key `(note_id, ordinal)`. The table exists only in the uncommitted working tree; no vault contains it. Define the final columns directly in `CREATE TABLE`:

```sql
note_id INTEGER NOT NULL REFERENCES note_items(id) ON DELETE CASCADE,
ordinal INTEGER NOT NULL CHECK (ordinal >= 0),
mime_type TEXT NOT NULL,
audio BLOB NOT NULL,
PRIMARY KEY (note_id, ordinal)
```

Preserve the table's STRICT declaration. Add no column check or upgrade step. Do not store the voice: nothing reads it, and the dialog never recalls a previous voice. Remove the `voice` column, the `voice` parameter of `note_save_audio` and `save_note_audio`, and `voice` from the info result. A test vault that ran the working-tree build keeps the old table; drop that table by hand once. Do not create a separate audio-set table.

`note_save_audio` performs one transaction: verify the note exists and is a normal read-only note, delete its previous rows, insert all converted clips with contiguous ordinals and MIME type, then commit. Read each compressed file into memory only when inserting it. A failure at any point rolls back the deletion and inserts, preserving the previous set. The application must hold the correct admitted vault context for this store operation.

Clip lookup returns bytes and stored MIME type through the existing `MediaResources` path. Do not require service configuration for `info` or `source`. Missing rows are playback errors. Deleting a note uses the existing foreign-key cascade. Because the clips reside in the vault database, use the existing database backup/restore path and verify it carries the rows; add no separate audio-file backup scheme.

## Playback

Retain `useNoteAudio`, the existing local resource fetch, and `HTMLAudioElement`. Expose `play(position)`, `stop()`, `canPlay(position)`, and a reactive playback state to `NotesScreen`. Use three user states: stopped, playing, and paused. Playing and paused both own the playlist even when no clip is currently audible.

| Event | Result |
| --- | --- |
| Play from here while stopped | Resolve the clicked block's ordinal, claim speech ownership, and load that clip from its beginning. |
| Space while playing | Set paused immediately and pause the current audio element if present. |
| Space while paused | Resume the current element, or allow the pending next clip to start when ready. |
| Clip ends | Load the next ordinal immediately; if paused, keep it ready without starting. At the last clip, stop. |
| Stop, playback error, or lifecycle exit | Invalidate pending loads, stop audio, release resources and ownership, clear highlight, and become stopped. |

Use the existing serial/token guard for asynchronous source fetches and `play()` completions. A late callback from a stopped playlist must not play a clip or clear a newer playlist. Stop remains valid during loading and between clips. Release any resource returned after its request became stale.

While stopped, show Play from here only for a clicked heading or paragraph with saved audio. While playing or paused, show Stop playing in the note context menu, independent of the clicked block's eligibility. Preserve the other menu actions. Opening or closing the menu has no playback side effect. After choosing Play from here, return focus to the read-only reading area so Space works without another click.

Handle non-repeating Space keydown only in the reading area while the playlist is playing or paused. Prevent default page scrolling there. Leave events in inputs, dialogs, and menus unchanged; account for the read-only editor's DOM structure rather than ignoring the entire editor as an input. Do not consume Space while stopped. Remove the existing pointer/click/double-click playback listeners and their click-delay state.

At the first actual playback start of each clip, apply the temporary block highlight and reveal the block's beginning if it is outside the reading viewport. Track this once for that clip; resuming the same clip does not scroll again. Do not attach scrolling to `timeupdate`, a timer, or pause/resume. Pause keeps the highlight; stop, error, and completion clear it. Do not write editor marks or saved annotations.

Start with sequential clip loading and no artificial transition delay. Keep the ownership claim across release of individual clip resources. Validate audible boundaries on the target Mac before adding buffering. Only if measured loading gaps fail the PRD, add one next-clip preload within this hook; do not build a playlist cache or preload the whole note.

### Shared speech ownership and cleanup

Reuse `claimSpeechPlayback(stop)` for the entire note playlist and for chat speech. Claim before asynchronous loading starts. Retain the claim while paused or between clips; release it only when that source stops or completes. Starting either source calls the other source's stop callback, including when the other is paused. A stopped source requires a new explicit start.

Keep `beginSpeechPlayback()` for the existing audible-output/microphone coordination. Its end callback can run on pause or between clips, but must not release the playlist's ownership claim. These are separate existing responsibilities; do not add another ownership registry or modify the existing microphone echo-tail policy for this feature.

Call the same note `stop()` on dialog opening (through the audio-change notification), note departure, transition to editable mode, vault change, and unmount/application close. Release the native media resource, abort pending local fetches, revoke object URLs, detach element callbacks, and remove the transient highlight. Closing the generation dialog never resumes old playback. After a successful commit and Close, refresh the stored clip count; do not autoplay.

## Implementation order

1. Add the shared webapp text extraction and extend the start bridge argument. Remove the C++ extraction function and its now-unused declarations/tests. Verify generation and playback use identical ordinals.
2. Complete native read-error classification, retries, progress parsing, result validation, and terminal status. Keep the existing worker and lifecycle ownership.
3. Replace the `voice` column with `mime_type` in `CREATE TABLE`; verify atomic replacement, conversion finalization, and media lookup.
4. Present the required visual states for approval. Then align the dialog, menu, voice options, phase labels, highlight, keyboard behavior, and block-start scrolling with the PRD.
5. Verify speech ownership across pauses and transitions, then perform the focused automated and macOS checks below. Remove obsolete click timers, progressive-scroll code, remembered-voice contracts, and tests that require those behaviors.

## Verification

Use existing Vitest, C++ unit-test, and native-test facilities. Add tests for failure-prone behavior, not snapshots that merely repeat the implementation. Use a local fake service or the existing HTTP test support for deterministic responses and injectable waits for retry tests; do not make tests sleep for real retry intervals.

| Coverage | Required checks |
| --- | --- |
| Block extraction | Nested lists/quotations, headings, repeated paragraphs, hard breaks, visible link text, comments, image/code-only notes, whitespace-only blocks, and Unicode code-point boundaries. |
| Start and dialog | Saved-document preparation, exact block payload, fresh voice selection, no remote work before Generate, no duplicate starts, modal locking, empty-note rejection, and stop-on-open without resume-on-close. Local status failures keep the modal locked and timer running until a confirmed outcome; a lost start reply triggers status reads without another start, and an earlier terminal result is ignored. Delayed terminal reads use native completion time for final elapsed time, including preparation and excluding polling delay. |
| Service | Exact JSONL limits, pending versus running state, absent/malformed counts, failed items counted as complete, out-of-order results, duplicates/missing/unknown keys, malformed audio, and succeeded batches containing item errors. |
| Retries | Network and HTTP 500 failures followed by success; exactly three read retries on persistent failure; reset after successful polling; no retries on other HTTP/local errors or POSTs; download restart without partial-file concatenation. |
| Storage/lifecycle | Previous set survives conversion and insertion failures; successful replacement removes old extra clips; note deletion cascades; wrong/departed epoch and deleted note cannot receive a commit or stale status. |
| Playback | Middle-block start, ordered completion, Stop menu while paused/loading, Space repeat suppression, inputs/menus unaffected, pause/stop during pending loads, no click pause, one reveal per clip, highlight cleanup, and chat takeover of a paused playlist. |
| Native macOS | Convert very short headings, normal and long paragraphs; fully decode stored M4A; check duration, pitch, pace, word boundaries, and transition delays in CHA. Verify UI repaint during conversion and commit. |
| Persistence | Restart with service offline; play saved clips. Back up and restore a vault and verify audio bytes and playback. |

Run the relevant frontend tests and type checks, bridge/schema checks where changed, and C++ storage/service tests. Finish with the PRD's acceptance scenarios on macOS. The PRD's native feasibility result is evidence for the encoding approach, not a substitute for in-app listening and backup/restore verification.
