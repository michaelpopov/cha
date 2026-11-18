# Desktop D1 sync data integrity

**November 18, 2026** · Proposed design, revised after review (minimal scope)

## Scope

Fix the P0 findings in the [Astra architecture review](../docs/astra-arch-review.md#1-d1-sync) for macOS and Windows Desktop. Keep one active editor, one worker per open vault, the 30-second loop, the pass at vault open, and the final send attempt at close. The worker still sends through Cloudflare D1 and uses R2 for complete copies.

The fix is small. Each operation gets a random ID when it enters the outbox. The D1 queue groups pieces by that ID. A receiver records the IDs that it applied. With these three parts, each record applies once and in sender order. Notes keep their paths (R54), and a turn delete keeps its position (R52). There are no stable note, folder, or turn IDs, no baseline ID, and no enrollment state.

The key rule: a receiver acknowledges an operation only after its effect and its receipt are durable in the same vault transaction. An R2 copy carries the receipts with the data that they describe.

The Desktop workflow still assumes that the user lets the previous machine finish sending before working on the next one (R33). cha-daemon does not run D1 sync after this change (§6).

## Changed requirements

The user approved these changes to the [existing D1 requirements](d1-alt-design.md#2-requirements) on November 18, 2026. All other requirements stay, including note paths (R54), turn positions (R52), and the missing-table rules of R46.

| Requirement | New rule |
| --- | --- |
| R9 | Still no data versions, conflict checks, or content comparisons. The vault holds receipts: the IDs of the operations whose result it contains. R2 replacement compares receipt sets (§4). |
| R23 | Each operation has a random ID, and every retry reuses it. A receiver applies an operation once, using its receipt. A partial write is retried with the same ID. |
| R6, R40 | Acknowledgement deletes the exact operation IDs that were handled, with one request for each page. |
| R14, R16, R41 | A record whose content cannot be applied is still skipped, and the worker also writes an Event. A transient failure keeps the record in D1, and the next loop tries again (§2). |
| R46 | No automatic eviction. A full D1 database stops sending until space is free (§2). The missing-table rules stay. |
| R57 | Replacement of the active vault is refused when local changes are still pending after the final send, or when the R2 copy lacks a receipt of the current vault (§4). |
| R3, R21, R46 | During the upgrade only, the worker makes the new queue table of each machine that has only a legacy table, this machine included (§5). |
| R17, R51 | cha-daemon does not run D1 sync (§6). |

## 1. Local state and queue format

| State | Representation and purpose |
| --- | --- |
| Outbox | The existing `sync_queue` table gets an `op_id` column. Allocate a random 128-bit ID once at enqueue and store it as 32 lowercase hexadecimal characters. Retain the same ID and bytes for all attempts. |
| Receipts | A new table `sync_seen(op_id TEXT PRIMARY KEY) STRICT`. A receipt means that the vault contains the result of the operation. It records both local origin and incoming apply. It holds no payloads. Receipts have no expiry. |

Add a receipt when an outbox row is created, in the transaction that records the change. For a saved note that uses the existing `changed` flag, add the receipt when the worker enqueues `note.update` and clears the flag in one transaction. This keeps save coalescing. [Config enqueue](../src/workspace/workspace_config_store.cpp#L975) · [Note flag collection](../src/storage/sync_storage.cpp#L292)

When CHA empties the outbox without sending, it deletes the receipts of the discarded rows in the same transaction. This happens in `sync_clear_outgoing`, which serves Disconnect, the cleanup of an R2 download, and a new vault copied from another local vault. Without this rule, a later replacement of that vault fails the receipt check (§4) for changes that no other machine ever got. When the worker drops outbox rows because no other queue table exists (R11), it keeps their receipts. The vault contains those changes, and a later peer gets them through R2. [Outbox cleanup](../src/storage/sync_storage.cpp#L108)

Each D1 queue table is named `q2/<vault>/<machine>`, with the existing naming rules of `q/` tables: vault names compare without ASCII letter case, machine names use the existing comparison, and identifiers use the existing escaping. Old builds parse only `q/` names, so they never send to `q2/` tables and never evict them.

```sql
CREATE TABLE IF NOT EXISTS "q2/<vault>/<machine>" (
    seq INTEGER PRIMARY KEY,
    op_id TEXT NOT NULL,
    piece INTEGER NOT NULL,
    pieces INTEGER NOT NULL,
    record TEXT NOT NULL,
    UNIQUE (op_id, piece)
);
```

Number pieces from one, with `1 <= piece <= pieces`. Keep the existing UTF-8-aware split of 1,900,000 bytes. The row with `piece = pieces` makes the operation ready. The sender writes it only after D1 confirms every earlier piece for that recipient. A single-piece operation is ready at its only insert. [Current D1 client](../src/app/d1_client.cpp#L212) · [Current split logic](../src/storage/sync_storage.cpp#L173)

## 2. Send, apply, and acknowledge

### Send

1. If the outbox is empty, skip the send. Otherwise list the queue tables of the vault with one request. If the `q2` table of this machine is missing, stop the worker with the existing missing-table error (R46). The recipients are the other `q2` tables of the vault.
2. With no recipients, delete the outbox rows locally (R11) and keep their receipts.
3. Otherwise send the outbox rows in order, and the pieces of each row in order. Each piece is one batch with one `INSERT ... ON CONFLICT (op_id, piece) DO NOTHING` for each recipient. A piece counts as confirmed only after a successful result for every recipient. An ambiguous result is a failure.
4. Stop the send pass at the first failure and keep that outbox row. This includes a timeout, a partial multi-recipient write, and a crash. The next pass starts the operation at piece one with the same ID. Delete the outbox row only after the final piece is confirmed for every recipient. Finish one operation before the next, so that each receiver gets the operations of a sender in order. If the sender stops after confirmation but before the local delete, the next pass sends the same ID again, and the receivers acknowledge it by its receipt.

### Receive

1. Read a page of up to 50 ready operations from the **head**, ordered by the `seq` of the final piece. The page gives `op_id` and `pieces`, and `record` when `pieces = 1`. Every page starts at the head; there is no `seq > previous` cursor. When the result is too large, halve the page size as today.
2. Handle the operations of the page in order:
   - If the receipt exists, add the ID to the acknowledgement list. This also covers a retry that overlaps an earlier acknowledgement, when only some of its pieces remain.
   - If `pieces > 1`, read the pieces by `op_id`, one piece for each request. Require one row for each ordinal, all with the same `pieces` value.
   - Apply the record and write its receipt in one local transaction. Check the receipt again inside that transaction. Use the existing chat and global reservations. Add the ID to the acknowledgement list.
   - On a permanent failure, skip the operation: write a log warning and an Event, write no receipt, and add the ID to the acknowledgement list.
   - On a transient failure, stop at this operation. It stays in D1 for the next loop.
3. After the page, or at a stop, delete the listed IDs with one request: `DELETE FROM <table> WHERE op_id IN (...)`. A page has at most 50 IDs, below the limit of 100 bound parameters. If the delete fails, end the pass. The next pass reads from the head again, and the receipts make the repeated operations no-ops.
4. Continue with the next page until a page is shorter than the limit or the pass stops.

Keep the existing post-commit notifications for notes and the chat mirror. A failed notification does not change whether the operation committed. Received config publishes the validated workspace snapshot after commit, as today.

For 1,000 single-piece operations, the receiver uses about 20 reads and 20 deletes, as today. Cloudflare allows 1,200 API requests in 5 minutes for each user. [Current receive loop](../src/app/d1_sync.cpp#L339)

### Failure classes

- **Transient:** a D1 request failure, a failed chat or global reservation, a config revision change during the reservation (the existing retry), and a local SQLite error with result code `BUSY`, `LOCKED`, `IOERR`, `FULL`, or `CANTOPEN`.
- **Permanent:** all other failures. Examples are invalid JSON, an unknown kind, missing or invalid fields, a missing parent, target, or forum, a config delta that fails validation (R14), inconsistent pieces, and other SQLite errors such as a constraint failure.

Today a SQLite error is a plain `std::runtime_error`, and the note apply path wraps it as `NoteFailure::saving`. Keep the SQLite result code through these layers so that the worker can classify the error. [SQLite error](../src/storage/sqlite_storage.cpp#L128) · [Note apply wrapper](../src/storage/note_storage.cpp#L2679)

A transient failure stops only the receive pass. Sending continues, and local edits leave the machine as usual.

### Capacity

Remove automatic queue eviction from the worker. A full D1 database leaves the outbox and all queue tables intact. The worker continues to receive and acknowledge, which frees space. [Current eviction](../src/app/d1_sync.cpp#L193)

If space does not come back, for example because a machine is not used, the user deletes that machine's queue table in the Cloudflare console. When that machine opens the vault again, the worker stops at the missing table (R46). The user downloads the vault from R2, and the download makes the table again. The records in the deleted table are lost for that machine. The R2 copy carries their changes when the uploader had received them.

Before implementation, test whether D1 accepts `DELETE` when the database is full. If it does not, a full database also stops receiving, and only a table delete in the Cloudflare console frees space.

### Diagnostics

Use the existing log and Events surfaces ([record_event](../src/app/events_operations.cpp#L7)). Report a skipped record with its ID and a short reason, a missing queue table of this machine, and transport or capacity trouble. Record a condition when it starts and report recovery when it clears. One in-memory last-reported condition for each worker is sufficient. Keep payloads and credentials out of the messages. Event texts are visible UI text, so show them for approval before implementation.

## 3. Targets, forums, and config

### Notes and chats

- Notes and folders stay addressed by path (R54). A turn delete stays addressed by position among the turns with a response or error entry (R52, R59). With each record applied once and in sender order, these find the same item on every machine when the user works on one machine at a time.
- `note.update` carries `folder_summary`, and the sender reads it with the other note fields. When the update creates a missing note (R49), it sets the flag from the record. Today only `note.create` sets it. [Sync note fields](../src/storage/sync_storage.cpp#L52) · [Update fallback](../src/storage/note_storage.cpp#L2523)
- A record whose target is missing follows the permanent-failure rule. Because a receipt marks every applied operation, a repeated delete is acknowledged by its receipt and never reaches this rule.

### Forums and config

Add the missing relational `forums` rows for all forums of the validated candidate in the same transaction as the config change and its outbox record. Today the commit adds them only for received config. Run the same insert at workspace open before the first receive pass, to repair forums that Settings made before this change. Keep removal and prune behavior separate. [Config transaction](../src/workspace/workspace_config_store.cpp#L1623)

Received config continues to validate and apply a delta with the existing D1 credential rules (R56). Write the receipt in the transaction that changes config and forum rows. When the delta makes no change, write the receipt in its own transaction; today that case commits nothing. A delta that fails validation is skipped with an Event (R14). [Incoming config](../src/workspace/workspace_config_store.cpp#L1840) · [Current turn apply](../src/storage/sync_chat.cpp#L406)

## 4. R2 snapshots

### Upload

No change. Collect the note flags, attempt the final send, and refuse the upload while outbox rows or flags remain (R26). The copied database includes the receipts. [Upload preparation](../src/app/vault_operations.cpp#L1123)

### Replace the active vault

1. Stop the worker and drain local writers with the existing maintenance path. After the final send attempt, check the **current** database. If outbox rows or note flags remain, refuse the replacement before the download.
2. Download and validate the candidate in staging with the existing rules. Then check that every receipt of the current vault exists in the staged copy. Read both receipt lists in `op_id` order with the existing SQLite handles and compare them. A missing receipt means that the copy can lack a change that this machine applied and acknowledged, or a change that this machine sent. Refuse with a reason. Nothing is installed. The remedy is a new upload from a machine that has received all changes.
3. Clear the staged outbox and note flags as today. Install through the existing file replacement and `.bac` handling.
4. Reopen and start the worker as today; the existing recovery start makes a missing table. A queued operation that the copy already contains is acknowledged by its receipt. Newer queued operations apply normally.

These checks apply to every replacement of the active vault. A vault that never synced has no receipts, so its check always passes. The staging check makes no D1 request, and the replacement does not clear or acknowledge any D1 queue. A failure before installation keeps the current vault. Show the error texts of this section for approval before implementation. [Current Desktop replacement](../src/app/vault_operations.cpp#L1250) · [Current staged cleanup and install](../src/app/r2_database_transfer.cpp#L1124)

The check deliberately refuses some older copies. Add from R2 has no receipt check.

### Add from R2, copy, missing table, and Disconnect

- **Add from R2.** No change. The new vault keeps the snapshot's receipts and D1 credentials, and its outbox and flags are cleared (R57). This machine has no table for it yet, so the worker stops until Save D1 credentials makes the table. [Add a second computer](../docs/AdminManual.md#add-a-second-computer)
- **New vault copied from a local vault.** No change, except the receipt rule of §1 when the outbox is cleared. The new name has no table, so the vault needs Save D1 credentials. [Current copy path](../src/app/vault_operations.cpp#L660)
- **Missing queue table of this machine.** No change (R46). The worker stops with the existing error. An R2 download or Save D1 credentials makes the table again. Records that were in a deleted table are lost for this machine. Without automatic eviction, a table goes missing only through Disconnect or a manual delete.
- **Disconnect.** As today (R45). It also deletes this machine's legacy table, and the receipt rule of §1 removes the receipts of the discarded outbox rows.

## 5. Upgrade from the current queues

Legacy tables `q/<vault>/<machine>` hold pieces with no operation ID. The new build never writes to them.

- At the schema upgrade, each existing outbox row gets a random `op_id` and a receipt. Pending local work is then sent in the new format.
- In each loop, the new build first reads this machine's legacy table, if it exists, with the current receive code, and then reads its `q2` table. Legacy records write no receipt and keep their current faults. Reading the legacy table first keeps each sender's order, because a sender wrote all of its legacy records before its first new record. A missing legacy table is normal and does not stop the worker.
- When the worker starts, it lists the tables of the vault once. For each legacy table with no `q2` table for the same machine, this machine included, it makes that `q2` table. So the first machine to update does not stop at a missing table, and changes made with the new build wait for a machine that still runs the old build until it updates. Save D1 credentials and an R2 download also make the `q2` table of this machine.
- The new build does not delete legacy tables, because old builds can still write to them. After all machines run the new build, the user can delete the legacy tables in the Cloudflare console.

## 6. cha-daemon

cha-daemon shares the worker and apply code. It has no Settings and no R2 transfer, so it cannot use the replacement checks or Save D1 credentials. In this change, cha-daemon treats every vault as not connected: it uses no machine name for sync, starts no worker, writes no outbox rows, sets no note flags, and makes no D1 request. This includes the D1 inbox clear of a Welcome download. It writes a log warning when it opens a vault that has D1 credentials. [Daemon worker start](../src/app/application.cpp#L404) · [Welcome inbox clear](../src/app/vault_operations.cpp#L336)

Before the upgrade, delete the D1 tables of the Linux machine in the Cloudflare console. Otherwise the Desktop machines keep sending to them.

The user approved this design point on November 18, 2026. The implementation changes shared code that cha-daemon runs, so it still needs the user's explicit permission under the ChaWeb rule in `AGENTS.md`.

## 7. Implementation and acceptance

Implement in this order:

1. Remove automatic eviction. Add the `forums` row fix and the repair at open.
2. Turn D1 off for cha-daemon, after the user's permission.
3. Add the outbox IDs, receipts, `q2` tables, send and receive, failure classes, Events, the legacy read, and the creation of `q2` tables for machines that have only a legacy table.
4. Add `folder_summary` to `note.update`.
5. Add the R2 replacement checks, the receipt cleanup in `sync_clear_outgoing`, and the legacy table delete at Disconnect.

Before implementation, test these D1 behaviors through the REST API: `DELETE` on a full database, the batch result of `INSERT ... ON CONFLICT DO NOTHING`, and `DELETE ... WHERE op_id IN (...)` with 50 parameters.

Reuse the existing D1 transport injection and Desktop vault fixtures. Use a SQLite-backed queue double for `seq` reuse and interleaving tests.

| Scenario | Required result |
| --- | --- |
| A multipart record interleaves with a one-piece record from another sender, or an insert fails midway for one recipient | Each record applies once from its own pieces. The failed outbox row keeps its ID and is retried. |
| An insert commits but its response is lost, the sender crashes before the outbox delete, the receiver crashes after apply, or the acknowledgement fails | The same ID is retried. The effect applies once. Acknowledgement deletes only that ID, also when a retry insert and an acknowledgement overlap. |
| An acknowledgement empties the queue and new rows reuse `seq` values | All new operations are read from the head. No unread row is deleted. |
| A `chat.entries`, `note.create`, or `chat.delete_turn` record arrives twice | It applies once: no second turn, no `Name (1)` note, and no delete of another turn. |
| D1 is full, local storage is busy, or a reservation fails | No data is lost. The receiver retries the transient failure. Sending continues while receive retries, and receive continues while D1 is full. |
| A record has invalid content, or a config delta fails validation | The record is skipped with an Event. Later records apply. Outgoing sends continue. |
| A Settings-created forum receives the first peer chat before a local chat is opened | The forum row exists. A failed config transaction produces neither a row nor an outbox record. Missing rows are repaired at open. |
| `note.update` recreates a missing folder summary note | The note keeps `folder_summary`. |
| The R2 copy contains one queued operation and lacks a later one | Replacement acknowledges the first by its receipt and applies the later one once. Test receipts from local origin and from incoming apply. |
| The R2 copy lacks a receipt of the current vault, or outbox rows or flags remain | Replacement fails before installation. The current vault and the D1 queue do not change. |
| Disconnect with unsent changes, then a later replacement from a machine that has all sent changes | The discarded changes do not cause a refusal. |
| Upgrade with pending outbox rows, legacy inbox rows, and a peer that still runs the old build | The first updated machine starts with a new `q2` table of its own. Old outbox rows are sent with new IDs. Legacy rows apply before `q2` rows. The peer gets a `q2` table and receives the new changes after its update. Old builds do not write to or drop `q2` tables. |
| cha-daemon opens a vault with D1 credentials | No D1 request, no outbox rows, no note flags, and one log warning. |

## 8. Accepted risks

| Risk | Example |
| --- | --- |
| A wrong target after changes on two machines before they synchronize (R33). | Both machines make a note `X` in one folder. One receiver adds a suffix (R15), and later records by path can reach the other note. Turn positions can also differ. |
| Duplicate turns after a rename to a temporary label and back (R29). | The new share writes new operations with new IDs, so the receiver adds the turns again. |
| Records in a queue table that was deleted by hand are lost for that machine. | The user deletes a table in the Cloudflare console to free space. |
| A full D1 database stops sending for all machines until space is free. | A machine is closed for a week while another machine edits large notes. |
| Pieces of a discarded operation stay in D1. | The sender disconnects after it sent some pieces of a large record. Nothing deletes the pieces without a final piece. |
| Receipts are never removed. | At about 300 operations a day, receipts add about 9 MB a year to the vault and to each R2 upload. |
| A skipped config delta can leave the configuration different on one machine. | The Event names the record. An R2 download from a correct machine repairs the vault. |
| Old builds keep their current faults during the upgrade. | An old build can still evict a legacy table or misread interleaved legacy pieces. |
| An old-build Disconnect leaves a `q2` table that a new build made for it. | Delete that table in the Cloudflare console. |
