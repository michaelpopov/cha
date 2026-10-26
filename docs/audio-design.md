# Background note audio technical design

Status: Technical design

Date: October 26, 2026

Requirements: [audio-prd.md](audio-prd.md)

## Scope and decisions

Move single-note audio generation into the native application. The setup dialog
closes after durable local acceptance. The application registers the remote batch,
resumes it when its vault opens, and commits complete audio with an Events entry.
Recovery is guaranteed only after the remote batch ID is stored.

Keep folder conversion on its existing path. Its dialog, target selection,
sequencing, skipping, progress, elapsed time, Stop action, errors, read retries,
and lack of restart recovery stay unchanged. The only folder integration is
admission against the same one-run limit.

Use the existing `BackgroundJobs`, vault database, credential store, service
client, native codecs, media resources, and Events history. Add one small table
for unfinished single-note attempts and one note-specific completion notification.
Do not add a job framework, waiting queue, scheduler, document snapshot, content
fingerprint, audio version store, persistent download cache, or remote API.

Keep support on macOS and Windows. Playback controls, narration rules, voices,
settings, and compressed storage format stay unchanged. This document specifies
implementation; this documentation task does not change the UI or application.
The single-note dialog follows the visual flow in the PRD.

## Existing code and changes

| Area | Paths | Change |
| --- | --- | --- |
| Single-note setup | `webapp/src/components/NoteAudioDialog.tsx`, `NotesTree.tsx` | Select a small single-note setup component when `folderMode` is false. Keep the existing folder component and loop. Reuse saved-document preparation. |
| Client and bridge | `webapp/src/api/client.ts`, `nativeClient.ts`, `src/bridge/notes_dispatch.cpp`, `bridge_protocol.*`, `bridge_router.cpp`, `resources/dto.yaml` | Add background start/status and completion delivery; reuse `nativeBridge` without changing its invocation interface. Keep the folder start/status/cancel contract. Generate `webapp/src/api/schema.d.ts` from the DTO source. |
| Application | `src/app/note_audio_operations.cpp`, `application.h`, `application_internal.h`, `application.cpp`, `vault_operations.cpp` | Own acceptance, recovery, retries, cancellation flags, and admission. |
| Service | `src/media/note_audio.cpp`, `note_audio.h` | Extract the existing submit, read, and result-processing steps so the background worker can resume by batch ID. Keep `generate_note_audio` as the existing folder wrapper. |
| Storage | `src/storage/note_storage.*`, `event_storage.*`, `src/workspace/workspace_config_store.*` | Add unfinished attempts and transaction-aware audio/event helpers. Include abandonment in existing note-change transactions. |
| Note changes | `src/app/notes_operations.cpp` | Interrupt the matching background worker after Make editable or deletion commits. |
| Playback | `webapp/src/notes/useNoteAudio.ts` | Handle affected-note completion independently of dialog lifetime; reuse existing stop, load invalidation, and availability refresh. |
| Conversion | `src/media/note_audio_codec.cpp` | Keep codec behavior; preserve enough error information to distinguish temporary file/storage errors from invalid audio. |

These are extensions to existing modules. Do not introduce a general job module
or restructure unrelated media code.

## Preserve preparation and folder conversion

`NotesTree` continues to load the saved note with `client.getNote(id)` and use
`noteAudioBlocks` on the existing ProseMirror schema. The helper is the single
source of block order for submission and playback. Keep headings and paragraphs;
skip comments, whitespace-only blocks, code, images, and the tree name. Preserve
text, hard breaks, and canonical ordinal keys: `"0"` through `"count - 1"`.

Native acceptance validates the target as a saved, read-only normal note, the
selected voice, and the settings actually used. Keep the existing limits:

- 1–1,000 blocks, each with 1–5,000 Unicode code points.
- At most 8 MiB of serialized UTF-8 JSONL, including newlines.
- Voices `boroda`, `grant`, `dzig`, `gerdt`, and `Kore`, with an explicit choice.
- An HTTP(S) service origin without a path, credentials, query, or fragment,
  and an existing usable API key reference.

Use `prepare_note_audio` for this validation and JSONL construction. Do not split
or truncate input. Ignore obsolete or unused settings with the existing warning
logger. Playback and saved-audio lookup need no service configuration.

Folder calls continue to use `noteAudio.start`, `noteAudio.status`,
`noteAudio.cancel`, and `generate_note_audio`. Extract shared service steps without
moving the background retry loop into that wrapper. In particular, the folder
wrapper keeps its current network/HTTP 500 read retries at 10, 20, and 40 seconds,
then fails. Other HTTP errors, including 401 and 503, keep their current folder
behavior. It creates no durable attempts or new lifecycle Events.

### One-run admission

Under `lifecycle_mutex`, both entry paths check for an unfinished attempt in the
current vault and for active folder work. A stored attempt reserves the slot even
before its recovery worker starts or while that worker waits after a failure.

If the attempt exists, reject before upload with the PRD message, using its saved
name and note ID:

> Audio generation is already running for “Chapter 1” (note 42). Make that note editable to abandon the attempt.

Preserve the existing rejection when folder work owns the slot. Do not queue the
new request.

Use the existing `note_audio_status` active check for folder work. The native
window has one current bridge connection, and the modal folder dialog prevents
starting another run, including between notes. Recovery starts only when the
native vault context opens, which ends the previous folder loop. No folder
reservation, new folder bridge methods, or teardown rules are needed. The folder
loop, including its existing start/status/cancel calls, stays unchanged.

## Durable attempt record

Create the table with the existing vault schema setup. Existing vaults start with
an empty table; there is no audio migration.

```sql
CREATE TABLE IF NOT EXISTS note_audio_attempts (
    id INTEGER PRIMARY KEY AUTOINCREMENT
        CHECK (id > 0 AND id <= 9007199254740991),
    note_id INTEGER NOT NULL REFERENCES note_items(id) ON DELETE RESTRICT,
    note_name TEXT NOT NULL,
    clip_count INTEGER NOT NULL CHECK (clip_count BETWEEN 1 AND 1000),
    service_origin TEXT NOT NULL,
    api_key_id TEXT NOT NULL,
    batch_id TEXT
) STRICT;
```

The primary key is the attempt identity. `AUTOINCREMENT` prevents an old worker
from matching a later attempt after the table becomes empty. There is no second
job ID. `batch_id IS NULL` means submission has not been registered; a non-null
batch ID means it can be recovered. No phase column is needed.

The store checks that the table is empty inside the acceptance transaction.
`SqliteTransaction` already uses `BEGIN IMMEDIATE`; together with the existing
store and lifecycle mutexes, this serializes admission. There is at most one
unfinished single-note attempt. Do not add an index or an additional slot table.

Store the note name captured at acceptance for event messages. Keep the origin
and key reference fixed for this attempt. Do not store key values, voice, note
text, request JSONL, progress counts, or temporary paths. Recovery derives all
expected keys from `clip_count`. The worker keeps voice and prepared JSONL only
until submission and registration are complete.

The note foreign key deliberately does not cascade away the record: deletion
must first write the abandonment event in the same transaction. Do not add a key
foreign key that would prevent deleting credentials. Event-history trimming is
independent of this table.

## Accepting a single-note start

Add `noteAudio.startBackground({ id, voice, blocks })`, returning
`{ attempt_id }` after acceptance. Keep the existing folder methods and DTOs.
The native operation uses this sequence:

1. Hold `lifecycle_mutex`, check the admitted epoch, native recovery ownership
   described below, and shared slot, and validate and prepare the request. No
   service request has occurred.
2. Begin the store transaction, recheck note eligibility and the empty attempt
   table, and insert the attempt.
3. Launch its `BackgroundJobs` worker. Its first action must acquire the same
   lifecycle mutex and check the attempt and interruption flag before any HTTP.
4. Commit the attempt transaction, then release the lock and return the attempt
   ID. The worker can now start service work.

Use a narrow store acceptance method with a launch callback while its transaction
is open. This makes a worker-launch failure roll back the unsubmitted row. If the
commit fails after launch, set the worker's interruption flag before unlocking
and roll back; the worker sends nothing. This uses the existing lifecycle lock
as the launch barrier, without a second worker queue or handshake object.

Only a confirmed pre-acceptance failure returns a definite rejection. Never
return a definite rejection after the commit. Storage failure that prevents
confirming the local outcome is reported as uncertain and resolved through status;
it must not enable automatic resubmission.

### Dialog and lost replies

The single-note setup component contains Voice, Close, and Generate, the existing
replacement warning when applicable, and inline actionable errors. Keep its
accessible name, `Generate note audio`. Do not show a title, progress, elapsed
time, Stop, or a terminal result. Opening it retains today's playback stop.

Disable duplicate submission immediately during preparation and acceptance.
Validation or confirmed local rejection restores the selector and Generate, even
while an error is displayed. Close on acceptance, without waiting for upload.
Unmounting or closing the dialog does not cancel an accepted attempt. No later
modal opens.

For a lost start reply, call `noteAudio.backgroundStatus({ id })` on the same
ordered bridge connection. The host's ordinary worker executes this read after
the start operation. Read the existing attempt table in the admitted epoch:

- If an unfinished row exists for this note, return `{ pending: true, attempt_id }`
  and close the dialog. The application has accepted responsibility for audio
  generation for that note, whether it accepted this start or already had work.
- If no row exists, return `{ pending: false }`. This does not prove rejection:
  the attempt may already have finished. Keep the uncertainty state below and
  direct the user to Events. A failed read or changed connection/epoch also
  leaves the outcome unknown.

Use no acceptance receipt, request-ID callback, or extra status history. A fast
terminal outcome with a lost reply can leave the dialog uncertain; its result
is in Events. An ordinary successful start reply still closes it immediately.

In the unknown state keep Close and dismissal available, disable Generate for
that unresolved invocation, and show: “Could not confirm whether audio generation
started. Check Settings → Events before starting again.” Do not automatically
repeat the start request or add a status polling timer. The durable attempt still
prevents another start while work is unfinished. The same table supports status
and recovery.

## Native worker and recovery

Keep one worker function with two entry points: a new attempt has prepared JSONL
and voice; a recovered attempt has a saved batch ID. Both use the same registered
batch processing path. Reuse cancellation-aware waits and `BackgroundJobs`.

| Stored state | Action |
| --- | --- |
| No row | No background single-note work. |
| Row, no batch ID, worker still submitting | Complete this submission once, then register or retire it. |
| Row, no batch ID, recovered on vault open | Atomically record submission uncertainty and remove the row. Never resubmit. |
| Row with batch ID | Read that batch, retrieve its results, and save them. Never upload again. |
| Row removed by another path | Discard late work. Do not save clips or write another final event. |

### Recovery ownership

Only the native desktop process holding the vault's existing main lease may
accept background starts or recover attempts. Use `store->require_main()` on
start and `store->is_main()` before recovery; do not add another lease or process
ID to the attempt. Map the secondary-process exception to `vault_in_use_message`
for this operation instead of its settings-specific error. Use the existing text:
“Close other CHA processes using this vault, then try again.” Its role is
re-elected when the vault is reopened, as it is today.

Activate recovery when the native bridge installs the note-audio completion
callback, then on the existing resource resume/vault-open hooks while that
callback is installed. This occurs independently of webapp bootstrap, a live
webview connection, or a dialog. A webapp reload does not disable it. The headless
daemon, secondary processes, and unsupported platforms leave attempt rows intact;
they neither submit nor monitor nor retire them on startup. Closing a main daemon
and reopening the vault in the native application allows recovery there. Folder
conversion keeps its existing process rules.

Load the row before allowing audio admission. The ownership check prevents a
second process from mistaking a live submitter's null batch ID for interruption.
This feature adds no cross-process UI notifications or distributed ownership for
simultaneously opened vault copies.

On pause, vault switch, or shutdown, signal the worker's interruption flag and
interrupt waits/transfers. Retain its row. Do not turn interruption into a terminal
error. Every database access, credential lookup, and completion publication checks
the captured epoch under `lifecycle_mutex`. No network request or conversion holds
that mutex. Copy a resolved key value for one request, then release the lock.
Never access the newly selected vault through an old worker.

Use the captured epoch and cancellation flag to prevent the old worker from
accessing the reopened context, including maintenance that reopens the same
vault. Launch recovery at most once for an attempt in the current epoch. Do not
call the global `BackgroundJobs::join()` merely to switch vaults, because it
permanently closes that worker facility. Old workers may finish unwinding while
the current worker runs, but their epoch or removed attempt ID prevents further
local writes. Any worker cleanup must check its own epoch and attempt ID before
clearing application state.

An epoch change interrupts local work even when maintenance reopens the same
vault. This includes forum deletion and vault transfer/update operations. A
registered attempt resumes by batch ID. An interrupted submission without a saved
ID follows the same uncertainty rule as a vault switch; do not automatically
resubmit it. This is the limit of reusing the current context lifecycle. A local
registration-write failure within the same epoch still retains the returned ID
and retries only that write.

Vault backup/transfer continues to copy the database as it does today, including
unfinished attempts. Opening a copied vault follows the same registered/unknown
recovery rules. Do not silently clear attempt rows during transfer or add a
device identity or synchronization protocol.

### Submit and register

Extract the current upload and creation sequence into a submit helper. Preserve
its JSONL format, endpoints, validated resource names, HTTP(S) restriction, TLS
checks, and no-redirect behavior. Do not forward the API key to the secret upload
URL or expose that URL in events or logs. Do not automatically retry upload start,
upload finalization, or batch creation.

Immediately after a valid creation response, keep its batch ID in the worker and
call the registration transaction. It checks the exact attempt ID, stores the
batch ID, and appends the Information start event with the captured voice and
clip count. If the row already has that same batch ID, registration is a no-op;
if the row has gone, discard the response. Register even if the creation response
already says the batch is complete or failed; inspect that outcome afterward.

On a storage failure, retry this local registration only, retaining the returned
ID and voice in memory. Do not perform another upload or creation. A process exit
before registration leaves the nullable marker and produces the uncertainty
warning on reopen. A lost or unusable creation reply that could conceal an accepted
batch produces the same warning during normal operation.

The service has no creation idempotency token or client-key lookup. A local
transaction cannot remove this submission-uncertainty boundary.

### Poll, download, and convert

For a recovered row, first GET the saved batch immediately. For an unfinished
healthy batch, poll every 10 seconds. There is no overall batch deadline. Read
operation errors and item errors; remote completion alone is not success.

Resolve `api_key_id` through the existing credential store before each service
request. Replacing that key's value takes effect on the next request. Changes to
the default Notes origin or key selection affect future attempts only. A deleted
key is not replaced by a different key. The unauthenticated upload transfer keeps
its current secret-URL authentication behavior.

Download completed results to the worker's existing private temporary directory.
Truncate the result file before a repeated download. Keep current streaming JSONL
parsing, result-line and WAV size bounds, and conversion one clip at a time.
Require exactly one valid, successful result for each expected canonical key;
reject missing, duplicate, unexpected, failed, malformed, or invalid results.
Use validated ordinals for local filenames.

Keep native AAC/M4A conversion and `audio/mp4` storage. Do not add a second audio
store or change conversion settings. If interrupted during download or conversion,
recovery fetches the same batch's results again. A live worker may reuse a complete
result file or converted clips it still owns after a temporary storage failure;
it must not treat a partial file or partial clip set as complete. Keep current
RAII temporary-file cleanup, without a cache or expiry policy.

## Transactions and note changes

`note_save_audio` and `event_append` currently start their own transactions, and
`Application::record_event` is best effort. They cannot be called as independent
operations for this lifecycle. Extract small helpers that use an already-open
transaction. Keep the current public wrappers, including folder saves and other
event callers, with their existing transaction behavior. Do not add nested
transactions or a general transaction framework.

Add these narrow store operations, all under the store mutex:

| Operation | Work in one transaction |
| --- | --- |
| Accept | Check eligibility and an empty attempt table; insert the attempt; arrange launch as described above. |
| Register | Find exact attempt; set its batch ID; append start event and trim event history. |
| Finish | Find exact attempt; verify a normal read-only target and the expected complete clip count; replace all stored clips; append success event and trim history; remove attempt. |
| Retire | Find exact attempt; append final error or warning and trim history; remove attempt. |
| Make editable / delete | Find affected attempt, append abandonment warning and remove it, then perform the existing note change. |

The finish operation reads clip files and uses the existing transactional audio
replacement. Rollback restores the old complete set and unfinished record if any
write fails. Publish success and the completion notification only after commit.

Every final event must find and remove the exact attempt in its own transaction.
If another path has removed it, return a no-op. This makes success, worker failure,
and abandonment mutually exclusive and prevents an old result from completing a
new attempt for the same note. If a final event cannot be written, retain the row
and retry the final transaction; never remove it and then attempt a separate log.

Extend `note_set_read_only(false)` and `note_delete` inside their existing
transactions. Before deleting a folder, use the note tree's descendants to check
whether it contains the pending target. Read the stored name before removing the
attempt. After commit, signal only the matching worker's interruption flag and
release the local slot. If the transaction fails, neither the note change nor
abandonment takes effect.

Making the note read-only again does not restore a removed attempt. Renaming,
moving, highlighting, comments, reading, and scrolling leave the attempt intact.
Do not introduce content revisions or stale-audio handling for edits after a
completed run. Do not cancel or delete remote work.

## Failures and Events

Preserve typed failure information at the service, filesystem, codec, and storage
boundaries. The current transfer boolean and generic conversion error combine
conditions that need different background behavior. Add only the distinctions
below; do not infer retryability by matching user-visible error strings. Folder
wrappers continue to apply their old policy to these failures.

| Failure in a background attempt | Action |
| --- | --- |
| Validation or definite local rejection | Inline dialog error; no HTTP and no accepted attempt. |
| Upload/create failure | No automatic POST retry. Atomically retire with an error, or a submission-uncertainty warning if acceptance cannot be ruled out. |
| Registered read: network failure or any HTTP 5xx, including 503 | Retain the batch and retry the same read. |
| Registered read: HTTP 401 for an existing saved key | Retain and retry, resolving the same key reference again. Warn to replace its value in Settings → API keys. |
| Saved key deleted, further service access needed | Retire with an error: the remote batch may continue; a new run needs a configured key. |
| Other HTTP 4xx, missing batch/results, invalid service response, terminal batch/item error, invalid results/audio, unsupported conversion | Retire with an actionable error; preserve previous audio. |
| Temporary local storage failure, including disk full or temporary-file I/O | Keep the row and old audio; retry the failed local step. If necessary, download the same batch again. |
| Vault interruption or shutdown | Retain the row; no final event. |
| Target made editable or deleted | Commit abandonment with the note change; ignore late work. |

HTTP 401 retry applies to registered reads. A 401 during submission follows the
no-retry POST rule. When complete results are already local, saving does not need
an API key, so key deletion does not prevent completion.

Use delays of 10, 20, and 40 seconds, then keep retrying at 40 seconds. A successful
step resets its consecutive-failure count. Successful unfinished-batch reads
return to the 10-second poll interval; do not add a poll delay to a retry delay.
The same capped delay applies to local registration, processing, and final-write
retries. Keep ordinary transfer connection/inactivity limits. No new timer service,
jitter, timeout setting, or batch deadline is needed.

Distinguish a result size limit from a disk write failure, and malformed audio or
an unsupported codec from an OS file error. Size/format failures are terminal;
local storage failures are retained. Do not hide either category in one generic
“conversion failed” exception.

Preserve the failed operation and native error code in converter errors. Explicit
filesystem failures and documented disk-full or temporary I/O codes, including
codes from native output creation, writing, and finalization, are recoverable.
Unknown native converter errors are terminal. Do not classify every error at a
write step as storage failure, or every native converter error as invalid audio.
Keep the mapping small and based on known platform error codes. Tests must cover
both recoverable output-file failures and terminal format/codec failures on each
supported platform.

Use the PRD's event messages and severities, with the saved note name and ID:

- Information at registration: `Audio batch started for “Chapter 1” (note 42): 20 clips, voice Kore.`
- Success with audio commit: `Audio ready for “Chapter 1” (note 42): 20 clips saved.`
- Error for terminal failures, naming the failed step and a useful action.
- Warning for submission uncertainty and abandonment.
- One recovery warning per attempt per vault-open session for retained work.

Keep warning suppression as one in-memory flag for the active attempt. Set it
only after a warning is recorded; reset it when the vault is reopened. For network
or storage failures, warn when the initial retries are exhausted. Credential
rejection can warn on its first occurrence. Explain that work is retained, identify
the target, and give the relevant repair action or Make editable to abandon it.
If storage cannot record the warning, log that failure and keep the attempt.

Registration and final transactions prevent duplicate lifecycle events. Do not
create events per poll, paragraph, phase, or retry. Preserve the existing Events
layout, newest-first ordering, and 200-event retention. Lifecycle events use the
originating vault; they contain no note text, credentials, upload URLs, or raw
service responses. Diagnostic logs must also exclude secrets.

## Completion delivery and playback ordering

Add a native `noteAudio.changed` event with `{ id, context_epoch }` in the existing
connection envelope. Publish it after a successful background audio commit to
live connections for that vault. This is a note-specific notification, not a new
event bus or durable queue. No subscriber is required for the work to finish.

Subscribe in the native client/application lifetime, not the dialog. Forward the
event to the existing `cha-note-audio-changed` handling with an affected-note-only
flag. Keep current numeric notifications from dialog opening and folder settling
with their existing stop behavior. Background completion for another note must
not stop current playback.

For a matching background notification, synchronously call the existing `stop()`
first: increment its load serial, abort the blob fetch, clear audio callbacks,
pause playback, release the resource/object URL, and clear highlighting. Then
refresh `noteAudio.info`. Keep its existing disposed-component guard; ordinary
info requests and replies are ordered, so the post-notice refresh resolves last.
Keep serial checks on source/blob replies and do not autoplay. The serial guard
already releases a source resource that arrives after a stop; retain that behavior.

The bridge must provide this ordering, without a stored audio generation number:

1. Serialize commit and `noteAudio.source` database reads with `lifecycle_mutex`.
   After commit and before releasing it, enqueue the affected-note notification
   through an enqueue-only bridge callback. The callback must not acquire the
   router's main mutex or call back into the application; use a small
   note-specific ingress list with its own mutex to avoid reversing lock order.
2. In `take_delivery`, while holding the router mutex, drain these notices into
   per-connection pending affected-note IDs before collecting pending replies.
   Put context changes first, then current-epoch audio notices, then replies.
   Keep pending notices until delivered; the existing delivery acknowledgment
   prevents a later batch from overtaking an in-flight batch. Discard notices
   when their connection or epoch is invalidated.
3. A source reply containing replacement audio cannot have been queued before
   the commit's notice. If a delivery snapshot precedes the notice, a later
   source reply cannot enter that snapshot. Thus the new-clip reply is delivered
   with the notice before it, or in a later delivery.
4. Dispatch the DOM notification synchronously in the native event handler.
   `stop()` invalidates pending loads before any replacement-source promise
   continuation can use the new clips. Older replies delivered after the notice
   are discarded by the same serial guard.

An already playing old clip can continue until the commit notice arrives, but
cannot advance into replacement clips. A source request admitted before commit
owns bytes copied from the old set; a request admitted afterward follows the
notice. This prevents mixing sets without changing `note_audio` or media URLs.

After a webapp reload or a missed connection, the playback hook's existing initial
`info` read obtains current availability. There is no playback to preserve across
that reload and no need to replay old completion events. The native runtime
already calls `take_delivery` for each connection on its 50 ms pump interval, so
draining notices there also delivers completion when no command is active. No
new wake predicate or polling timer is needed.

## Implementation order and verification

1. Add attempt storage and composable event/audio transaction helpers. Verify
   rollback, exact-attempt retirement, and abandonment before changing the UI.
2. Extract service steps while retaining the folder wrapper and its tests. Add
   the background worker, recovery hooks, error categories, and shared admission.
3. Add background start/status, the setup component, and ordered completion
   delivery. Keep the folder loop and controls on their existing path.
4. Run focused native/storage/bridge tests, webapp checks, and native audio smoke
   checks on the supported platforms. Do not change unrelated documentation.

Use the existing mock HTTP server, temporary vaults, injected service waits, and
bridge delivery tests. Assert behavior rather than adding a new test framework.

| Coverage | Required checks |
| --- | --- |
| Preparation and acceptance | Empty/oversized input and invalid required settings send nothing; launch/commit failure sends nothing; double click accepts once; valid start returns before HTTP completes. |
| Lost replies | A pending row for the target closes setup after a lost reply; an absent row, fast terminal completion, failed status read, or changed context leaves a dismissible uncertainty state; never resubmit automatically. |
| Registration and restart | Fail registration after creation and retry only its database write; restart at each lifecycle boundary; saved IDs resume without any POST; unregistered rows warn and retire. |
| Atomic outcomes | Fail any audio/event/delete write and retain the old complete set plus row; restart before/after commit; exactly one start and final event; event trimming leaves the row. |
| Recovery failures | More than three network failures, 503, and 401 retain work; replacing the same key recovers; deleted key retires only when service access is needed; repeated failures emit at most one recovery warning per vault-open session; temporary storage failure, including native output write/finalization failure, recovers; corrupt/partial/failed results and unknown codec errors never replace audio. |
| Recovery ownership | A secondary process cannot start or recover background work or retire the main submitter's null-ID row; a main daemon leaves rows intact; reopening in a main native process resumes the registered batch once. |
| Note and vault changes | Rename/move/comments/highlights succeed; Make editable and direct/ancestor deletion abandon atomically; race failure or commit against abandonment and a new attempt; no old-epoch writes or key lookups after switching vaults; same-vault maintenance resumes registered work and reports uncertainty for interrupted unregistered submission, without a new POST. |
| Playback ordering | Replace an open/playing note, including a source reply in flight and a completion with no other traffic; stop and invalidate before using new clips; refresh without autoplay; another note continues playing; offline playback works. |
| Folder regression | Existing dialog, progress, timing, sequencing, skip rules, Stop, error policy, and no recovery/events remain; background work blocks folder admission and an active folder note rejects a background start. |

Extend `tests/storage/unit_note_storage.cpp`, `unit_event_storage.cpp`,
`tests/media/unit_note_audio.cpp`, the application/vault tests, and
`tests/bridge/unit_bridge_router.cpp` for the native behavior. Extend the existing
webapp dialog, native bridge, and `useNoteAudio` tests. Retain folder regression
expectations instead of rewriting them to match background behavior. Run
`npm run check` in `webapp` and the repository's existing native test targets
appropriate to the changed modules during implementation.

The known limit remains remote creation without a saved batch ID. Reopening the
vault reports that uncertainty and requires an explicit new generation request;
it cannot recover or cancel an unidentified remote batch.
