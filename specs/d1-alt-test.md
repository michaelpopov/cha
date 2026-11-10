# D1 queue synchronization: one-time acceptance test plan

Date: November 9, 2026

Status: test design only. This document does not report executed tests and does
not request their execution.

Source of expected behavior: [d1-alt-design.md](d1-alt-design.md), including
R1–R60 and the approved points in section 16. The
[implementation plan](d1-alt-plan.md) and current code locate execution paths;
they do not replace the design as the acceptance standard.

## 1. Purpose and coverage rule

Execute one acceptance campaign before relying on D1 synchronization. Show
that a person can leave one machine, open the same vault on another machine,
and continue with the expected notes, chats, and configuration. Cover macOS,
Windows, and Linux `cha-daemon` with ChaWeb.

This is a one-time exercise, not a new CI system, benchmark, permanent fault
framework, or product feature. Use existing tests and transport seams, small
temporary scripts, disposable vaults, and one evidence directory. Do not add
versions, deduplication, persistent receive buffers, automatic recovery, or
other machinery just to make a test pass.

“All cases” means every specified operation, state transition, record kind,
boundary class, failure boundary, delivery route, and accepted risk below.
There are infinitely many possible contents and timings; a finite execution
cannot enumerate those. Use the explicit equivalence classes and boundary
values in this plan. A table cell that lists alternatives requires a separate
variant for **each** alternative, not a choice of one example. Expand all
specified matrices into the run ledger before execution.

The normal scenario uses one writer at a time. Deliberate overlap appears only
in the concurrency, failure, and accepted-risk cases. Test the specified
outcomes there; do not require convergence or exactly-once delivery where the
design accepts loss, overwrite, or duplication.

## 2. Execution, fixtures, and evidence

### 2.1 Execution modes

| Mode | Use | What it cannot prove |
|---|---|---|
| LIVE | Real CHA operations, production curl transport, disposable Cloudflare D1 and R2 resources. Inspect actual remote queues and receiver databases. | A rare fault was exercised merely because a normal request succeeded. |
| CONTROLLED | Production storage, client, Worker, and application paths with the existing D1 transport seam or loopback HTTP server. Inject a precise response, storage failure, or scheduling barrier. | Cloudflare accepts the request or enforces the assumed limit. |
| HOST | Actual macOS/Windows host or Linux daemon/ChaWeb. Observe process lifetime, screens, reconnects, timers, and navigation. Combine with LIVE or CONTROLLED as stated. | Native host shutdown passed merely because a backend unit test passed. |
| EXISTING | Existing backend, frontend, and integration tests with retained output. | Uncovered variants, real D1 compatibility, or an untested platform. |

Use LIVE for P01–P05 and the handoffs in section 15. Use CONTROLLED for
deterministic failure and ordering cases. Use HOST for section 13 and native
shutdown. Run ordinary success paths through public application operations;
direct SQL insertion is for fixture preparation and explicitly malformed or
incoming-record cases, not a substitute for testing outgoing hooks.

Current useful entry points include:

- `src/app/d1_client.*` (`D1Client::Transport` and the test endpoint),
  `src/app/d1_sync.*` (one loop and send flush), and
  `tests/support/mock_http_server.h`.
- `tests/storage/unit_sync.cpp`, `tests/storage/unit_sync_chat.cpp`,
  `tests/app/unit_d1_sync.cpp`, and `tests/app/unit_r2_database_transfer.cpp`.
- Workspace, note, session, bridge, and native-runtime tests; frontend tests
  for `D1SyncSettings`, `ApiKeySettings`, `Notes`, `LiveChat`, and ChaWeb.

Use these where sufficient. A test name alone is not evidence that all its
plan variants passed. If temporary instrumentation is necessary, retain its
diff. It may observe requests, transaction boundaries, and event timing or
hold an execution barrier; it must not replace the behavior being measured.
Remove it after the campaign. Do not change `docs/`.

### 2.2 Machines and remote resources

Prepare these isolated roles, using the same build revision and recorded
working-tree changes:

| Role | Environment | Initial state |
|---|---|---|
| A | Packaged macOS app, machine `test-mac` | Source of the initial snapshot. |
| B | Packaged Windows app, machine `test-win` | Independent copy of the same vault, received through R2 download (a supported clean route), not a raw file copy. |
| C | Linux daemon and supported ChaWeb browser, machine `test-linux` | Independent copy installed through the existing manual setup. |
| D | A fresh desktop profile, machine `test-new`, reused as needed | No local vault or D1 queue; used for bootstrap and replacement. At the end of each case, disconnect D or drop its queue, so that its queue does not change the recipient counts of later cases. |

Use a unique campaign prefix in vault names and resource names. Use two vaults
in the same D1 database, plus a second D1 database for credential replacement.
Prepare plain and protected copies. Keep real `app.toml` files separate, so
an R2 transfer cannot accidentally supply a machine name.

The user has supplied Cloudflare D1 access variables in `~/.zprofile`:

| Variable | Purpose |
|---|---|
| `CHA_D1_ACCOUNT` | Cloudflare account ID for the D1 test resources. |
| `CHA_D1_API_KEY` | Cloudflare API credential for accessing D1 resources. |

Use these variables for the LIVE D1 checks and resource setup. If they are
absent from the test process environment, load their values from `~/.zprofile`;
do not ask the user for their location again. Obtain each database ID when
creating or selecting the disposable test databases. Configure test vaults
through the normal D1 credential path. Do not print the variable values or
include them in logs, evidence, this document, or committed files.

Use a dedicated D1 database for the full-capacity probe. Never fill or evict
queues in a database holding real user data. Confirm the account's current
limits and the selected resource before that probe. A free-plan test database
is preferable to filling a paid database. If a suitable resource is not
available, mark the probe blocked; a simulated full response does not pass it.
Use disposable R2 objects, not the user's existing backup object.

Budget the LIVE requests. The Cloudflare limit of 1,200 API requests in 5
minutes applies per user, so the three machines and the test scripts share it
(C4). Above it, all API requests of the account fail for 5 minutes. Keep LIVE
cases below that rate, record the request count of each LIVE case, and do not
cause a LIVE lockout; S16 tests the lockout with CONTROLLED responses. On a
free-plan account, also count rows written against the daily limit of 100,000
(C5): each record writes one row in each recipient queue, and each
acknowledgement counts too. Put the large and capacity cases on separate days,
or use an account whose limits permit them. When a case meets a quota or
lockout error that it did not intend, mark it `BLOCKED`, not `FAIL`, and run
it again after the limit resets.

For initial manual installation on C, follow the clean-copy steps of design
section 5.5 (R58): a successful R2 upload on the source, no edits after it,
close CHA, then copy. Separately test daemon restart with pending local work.
Do not clear that work as part of startup. T19 tests a manually delivered
*dirty* copy as an accepted risk.

### 2.3 Fixture contents

Use synthetic, uniquely identifiable content. Keep a manifest independent of
the sync implementation, with expected paths, stored IDs, text hashes, fields,
and logical entry order.

1. At least three forums, two characters, two personas, two providers, styles,
   voices, ordinary API keys, and R2 credentials. Include dependent references,
   pictures, a saved-key allocation counter, and local D1 credentials. Use
   dummy ordinary secrets for transport checks and a valid test provider or
   loopback provider for chat generation. Never use personal content.
2. Top-level and nested folders; editable and read-only notes; empty and rich
   text; links, images, Unicode, quotes, backslashes, and newlines; scroll
   positions; compressed text; summaries; generated ANNOTATIONS. Give A and B
   different numeric note IDs for equivalent paths.
3. Chats with temporary and real labels, duplicate labels, complete/cancelled/
   failed turns, `@-` messages, attachments, folders, and character overrides.
   Include a running first turn, a running later turn, an interrupted turn,
   and a previously shared chat with its history cleared. Make local entry
   numbers differ between machines.
4. Local audio and audio-attempt rows for notes and entries, and event history.
   Keep hashes/counts so exclusions and normal local deletion effects can be
   distinguished. Include an unrelated note and chat that each case must not
   alter.
5. Old-schema databases, current-schema databases, WAL-mode databases with
   committed data still in the WAL, and delivered snapshots containing both
   queued records and flagged notes. Include a dirty source with no D1 setup.
6. Records at the byte boundaries in section 8. Generate both compressible
   and poorly compressible content. Measure the **serialized record's UTF-8
   bytes**, not character count or source-note size. Hash before splitting
   and after reassembly. Include a record with more than 50 pieces.

Reset to a known checkpoint between independent cases. A checkpoint includes
the local databases, the `app.toml` files, and the D1 state: the list of queue
tables and their rows for each campaign vault. Do not let deliberate
duplicate, collision, eviction, or invalid-record cases contaminate a later
success case. Keep paused receiver Workers from consuming evidence queues
until the required inspection is complete.

### 2.4 Common assertions

Apply these to every relevant case, in addition to its specific expectations:

- Check the local user result, committed data, local `sync_queue`, note flags,
  actual remote recipient rows, receiver data, acknowledgements, and visible
  state. An HTTP 200 or a log saying “sent” is not enough.
- Check the outgoing record at the real operation boundary. Where the user
  operation already has a transaction, its change and record commit together
  or both roll back. Flag collection also commits its record and flag clear
  together. Network I/O must not hold that SQLite transaction open.
- On successful receive, compare semantic content: note paths and decoded
  content, configuration row names/IDs/values, chat identities and logical
  entry order. Do not demand equal local note IDs, entry numbers, SQLite
  files, or local-only tables. Check stored-ID references separately.
- Incoming application adds no outgoing record and no new note flag. Preserve
  unrelated pre-existing pending records/flags; zero is the assertion only
  when the fixture started clean. Do not erase local work to hide an echo.
- Audio, audio attempts, and event history are absent from records. Existing
  local cascades from note/chat deletion or clearing remain valid. An incoming
  `note.update` removes that note's audio even when text is unchanged.
- An invalid complete record changes nothing from that record, emits a warning,
  and is acknowledged. A chat reservation failure is different: retain the
  record and stop that receive pass. Retain an unfinished piece set at queue
  end. Do not delete a prefix past either retained item.
- Ordinary IDs are preserved. Notes resolve by path; chats resolve by forum ID
  and session ID, never label. No timestamp comparison, version state,
  delivery ID, deduplication, or registry is required or introduced.
- Local saves do not fail because D1 is unavailable or a record is large.
  Existing local validation, size limits, and storage errors still apply.
- After failure or recovery, perform one ordinary local edit and one incoming
  edit to prove that the database, reservations, and UI remain usable.

### 2.5 Run ledger and verdicts

For every case and expanded variant, record:

```text
Case/variant; requirement IDs; mode; sender/receiver platform; build/revision;
fixture/checkpoint; exact operations and injected boundary; expected result;
actual result; local queue/flag counts before and after; remote rowids and
piece metadata; request/event timeline; database comparisons; screenshots
where relevant; verdict; evidence paths; defect or unresolved question.
```

Use `PASS`, `FAIL`, `BLOCKED`, or `NOT RUN`. For the explicitly identified
external observations, use `OBSERVED` and record the result; absence of an
observation is not a pass. An accepted-risk case can pass when the risk occurs
as designed. Do not relabel an untested platform or a missing prerequisite as
an accepted risk.

Keep sanitized request bodies, response errors, SQL statement shapes, timing,
and row counts. Compare credentials in memory and retain only equality
booleans and test key IDs. Do not retain API tokens, Authorization headers,
passwords, or real credential-row contents. Synthetic payloads can be kept.
Protected-vault records are intentionally plaintext in D1; use only synthetic
content there. Record case times consistently, with an explicit timezone.

## 3. External assumptions and early gates

These checks implement design section 15. Run them first, even if code already
exists. Do not count controlled substitutes as LIVE passes.

| ID | Procedure | Expected result / decision |
|---|---|---|
| P01 | LIVE: submit the production batch shape with one 1,900,000-byte piece per recipient, for 1, 2, and the largest recipient count in the campaign. Use ASCII, multi-byte UTF-8, and quote/backslash-heavy JSON. Read every recipient back. | The API accepts each tested body; every piece is byte-identical. Report raw and HTTP-encoded body sizes and tested fan-out. A failure is a design blocker for that size/fan-out, not permission to silently reduce the requirement. |
| P02 | LIVE, isolated capacity database: fill eligible queue tables until an actual insert produces `Exceeded maximum DB size.`. Retain the response. Run the application's count/drop path; attempt the retained write on the next loop. | D1 permits the drop while full; one eligible largest table is removed; a later write can proceed once sufficient space is available. No immediate retry and no second drop in the same loop. If drop is impossible, the full-database strategy fails its gate. |
| P03 | LIVE: batch two or more inserts with an intentionally invalid first, middle, then last statement, using fresh rows each time. Inspect all tables after each response. | OBSERVED: document actual batch atomicity and response shape. Either atomic or partial application is compatible with R23. Feed the observed failure shape into controlled retry tests. |
| P04 | LIVE: query an empty queue, then a populated queue, and inspect D1 metadata. | OBSERVED: record rows read/written and request count. Do not assert zero cost or use design estimates as a hard threshold. |
| P05 | LIVE: populate enough large rows to trigger the actual result-size limit, then let the real Worker reduce its read count and drain them. | Exact error is recognized, reads recover down to a usable group size, and all complete records apply. Confirm the limit/error against current official D1 documentation when executing. |
| P06 | HOST: hold a chat reservation through a database change; attempt reconnect while held, then release. Repeat with an idle and generating chat on macOS, Windows, and ChaWeb. | No stale controller writes; desktop reconnects after `reloading`; ChaWeb shows the change through its own read/recovery path. A generating turn is cancelled. Failure blocks the affected screen path. |

## 4. Machine identity, connection, and schema

| ID | Procedure | Expected result |
|---|---|---|
| M01 | Start with `machine` absent, empty, integer, boolean, array, and containing `/`, separately. Open a normal vault and perform ordinary local operations. | Start and unrelated operations work. Invalid supplied values warn and act as no name. No Worker, new records, or new flags. Saving D1 credentials uses the approved missing-name error, sends no D1 request, and saves nothing. |
| M02 | Use valid machine names containing spaces, Unicode, `%`, `_`, and a double quote. Use valid vault names with spaces, Unicode, `%`, and `_`. | Names survive app configuration and table quoting. Each quote in an SQL identifier is doubled. Data cannot change the SQL structure. Do not invent stricter name validation than the design or existing host validation. |
| M03 | Use vault/machine names differing only in ASCII letter case; use similar names with different non-case characters. Include `Vault`, `Vault2`, `V_ult`, and `V%ult`. | ASCII case variants refer to the same identity; exact comparison prevents wildcard matches. Other vaults and this machine's own queues are not recipients. Non-ASCII names do not acquire an undocumented Unicode case-folding rule. |
| M04 | Enumerate normal tables, views, `q` names with missing/extra/empty components, and valid queues for multiple vaults/machines. | Only actual, correctly parsed queue tables participate. No registry table is required. Recipient lookup cannot treat an unrelated object as a destination or eviction candidate. |
| M05 | Cross machine present/absent with complete credentials present/absent. Repeat with incomplete credentials. | Only a valid name plus usable stored D1 credentials connects. Disconnected operations add no records/flags. Incomplete optional setup does not stop unrelated local operations. |
| M06 | Open a pre-sync plain database containing all fixture content, then a protected one. Reopen each twice. Also open copies of the existing local plain vaults on the test Mac, including the largest. Copy them while CHA is closed, with their WAL sidecars, to a place outside the user's vault directory. Never connect such a copy to D1 or R2, and keep only counts and verdicts as evidence. Delete the copies after the check. | `sync_queue` exists with whole-record text storage; `changed` is non-null, initially 0, and restricted to 0/1. Existing content remains. Migration is repeatable and does not create duplicate schema or records. Each real-vault copy opens, migrates, saves and reopens a note, and keeps its note, chat, and configuration counts. |
| M07 | Open current-schema vaults containing queued records and flags. Exercise ordinary open, close/reopen, restart, and password unlock. | Pending work survives until the normal send/collection path handles it. No delivery cleanup occurs on ordinary open. Wrong password cannot modify the database or start its Worker. |
| M08 | Force a schema migration or transactional queue insertion to fail, including SQLite busy and write failure. Reopen after removing the fault. | No partly committed migration/user transaction; existing data remains usable. A failed local transaction does not leave a record for a change that did not commit. |
| M09 | Inspect schema and requests after successful and failed sync. | Only the planned local queue/flag and remote piece columns are needed. No per-record versions, registry, receiver progress table, shared-chat flag, or receipt tracking. |
| M10 | Two machines deliberately use the same name, including a case-only difference. Send from a third machine. | Both use one queue and may consume each other's incoming work. Record the O8 risk; do not expect automatic unique-name allocation or conflict detection. |

## 5. D1 credentials and connection actions

Run the success and visible-error variants on both desktop hosts. Section 13
adds the exact screen checks.

| ID | Procedure | Expected result |
|---|---|---|
| C01 | Save complete new credentials with a named machine; hold the CREATE response before success. Inspect local settings during the hold. | Remote own-table creation precedes local credential commit. Only after success are credentials stored and one Worker started. Saving credentials itself emits no record. |
| C02 | Save again with the table present, then with it missing. Seed remote incoming rows in the present-table case. | Creation is idempotent; existing rows survive. Missing table is recreated. Pending local records and flags remain available for normal processing. No duplicate Worker. |
| C03 | Keep the saved token by leaving the token field blank; separately replace it and change account/database. | Blank keeps the current token; replacement uses the new values. Credential changes produce no record. Active work uses the committed connection, with no old Worker continuing after replacement completes. |
| C04 | Fail CREATE through DNS/connection/TLS error, timeout, HTTP authentication failure, server failure, and unsuccessful API result. Try new connection and replacement. | Approved D1-unreachable error; nothing new saved. Previous settings and pending work are retained on failed replacement. No Worker starts for an uncommitted connection. |
| C05 | Let CREATE succeed, then fail local credential commit. | Approved local-save error; local state/connection stays at its previous committed value. A newly created remote table may remain; do not require a compensating delete absent from the design. |
| C06 | Fail credential loading; then recover and reopen Settings. | Approved load error. Recovery shows stored IDs and token presence, without returning the token value in the detail response. |
| C07 | Disconnect with pending records, flags, and incoming rows. Test online and offline D1. Also disconnect with saved credentials and no valid machine name (R60). | Local credentials, outgoing queue, and flags clear together; Worker stops. One own-table drop is attempted. Offline drop warns but does not undo local disconnect. Other machines/tables stay intact. With no valid machine name, the local part completes, and no table drop is attempted, because no queue table name exists. |
| C08 | Cancel the Disconnect dialog, then separately fail its local transaction after confirmation. | Cancel has no effect. Local failure shows the approved remove error and preserves credentials, queue, and flags; no remote drop can discard the queue while local disconnect failed. |
| C09 | Hold a send/receive request during disconnect and during successful credential replacement. Release it as the action completes. | No old Worker resumes against a cleared queue, different connection, or replaced database. No late callback changes another vault's screen. Already accepted remote writes may remain. |
| C10 | Reconnect after a successful disconnect. Make new edits. | Explicit Save creates the table and starts synchronization. Previously cleared outgoing work does not reappear; new operations synchronize. |
| C11 | Change `machine` after local work was queued. Separately add `machine` to a vault that has credentials but no name, and therefore no pending work. Separately change only the letter case of the name. Open on desktop, recover with Save where needed, and inspect the old queue. | Missing new-name table stops the Worker: before sending when work exists, otherwise at the receive step. Save recovers the new identity without clearing pending work. Old-name table remains and can still receive records until explicitly removed/evicted. A change of letter case only keeps the same queue table (O8): the Worker does not stop, and no second table is made. |

## 6. Sending, REST responses, and full-database handling

For S01–S13, inspect request bodies and all recipient queues before receivers
run. Seed a later sentinel record to show where a send pass stops.

| ID | Procedure | Expected result |
|---|---|---|
| S01 | Run with an empty local queue and no flags, then with one pending record and 0, 1, 2, and several other machines. | With no outgoing work, normal polling reads the own queue without a table-list request (design section 14). With work, discover this vault's recipients once for the send pass. Require the own table even with no other recipients. With none, remove the local record without an insert; otherwise one batch per piece has one insert per other queue. Receiving still runs. |
| S02 | Queue several distinct records, including rowid gaps and a large middle record. Run one loop. | Send in local rowid order and piece order. Delete each whole local record only after every piece's batch is confirmed. Local queue never stores pieces. Never send to or read back from a recipient's queue as delivery confirmation. |
| S03 | Use JSON with quotes, SQL-looking strings, backslashes, Unicode, and long content. | Content is bound in `params`, not interpolated into SQL. Each statement stays within the design's SQL/parameter limits; payload text survives exactly. Endpoint and Authorization use the chosen D1 credentials. |
| S04 | Fail discovery, first insert, middle-record insert, and last insert, separately. | Warning; committed user changes remain. Already confirmed records can be removed; failed and later records remain. No later send in this pass; retry on a later loop with no retry-count ceiling. An ordinary send failure does not disable the independent receive step. |
| S05 | Let some recipient inserts commit but report a failed batch. Retry on the next loop. | Whole local record remains; all recipients are retried. Recipients already written may get duplicates. No per-recipient receipt state or selective retry. |
| S06 | Commit a batch remotely, then drop the response. Repeat with local queue deletion failing after confirmed remote success. | The local record remains and can be sent again. Duplicate `note.create` gets a suffix; duplicate `chat.entries` appends again. No false exactly-once assertion. |
| S07 | Return HTTP 200 with top-level failure; per-statement failure; missing/short result list; invalid JSON; empty/truncated body; wrong JSON types. | No unconfirmed batch causes local deletion. Warning and later retry. Distinguish a malformed response from a valid empty query result. |
| S08 | Return HTTP 401, 403, 429, 5xx, and timeout. Include daily read/write quota errors, an SQL syntax error, and result-size error on a write. | Retain outgoing work and stop sending this loop. Only the actual full-database error enables eviction. No drop for rate limits, daily quotas, authentication, protocol, or ordinary SQL errors. |
| S09 | Keep a send failing for several loops and across restart, then restore access. | No retry limit expires or discards the record. Retry uses normal loop cadence; successful recovery drains it. Local saves remain available during network failure. |
| S10 | Own table is missing at discovery, with both an empty and a populated local queue. Separately remove a recipient after discovery. | Missing own table stops the Worker with the recovery error and prevents sends when work exists. An empty queue discovers the missing own table during receive. A vanished recipient causes ordinary send failure; local work remains. Desktop does not auto-create either table. |
| S11 | Add a recipient between discovery and sending; separately add one before the next loop. | Current recipient snapshot may miss the new machine, as accepted by R24. Next discovery includes it. No registration barrier or history backfill is expected. |
| S12 | Cause the exact full-database error. Populate eligible queues across two vaults with unequal row counts; give a fewer-row queue more bytes. Make one same-machine queue the largest and one non-queue table larger still. | Count all eligible queue tables across the database in one request; exclude every queue of this machine, including case variants and other vaults, and all non-queue objects. Drop the eligible table with most rows, not most bytes. Keep the failed outgoing record. |
| S13 | Repeat full handling with tied maximum counts, only own tables, zero-row eligible tables, failed table listing, failed count, failed DROP, and a candidate disappearing before DROP. | At most one eligible table is selected/dropped per loop. Either maximum is acceptable on a tie. With no candidate or a failed prerequisite, retain work and warn where a request failed. Never fall back to dropping own/non-queue tables. A zero-row candidate may not free enough space; retry only next loop. |
| S14 | Require two evictions before sufficient space exists. Count writes/drop attempts over three loops. | First failing loop drops at most one and stops sending; next loop retries from the record's first piece and may drop one more. Successful send occurs on a later loop. No recursive eviction or same-loop retry. |
| S15 | Observe real curl with a controlled connection/handshake stall and with a loopback server that stalls body transfer; also return a slow but progressing response. | Existing 10-second connection and 30-second low-speed policies apply. A slow, successful full send pass can exceed 10 seconds. Do not add a total pass deadline. Capture actual cancellation and timeout behavior without relying only on mocked status codes. |
| S16 | CONTROLLED: queue more than 1,200 single-piece records, for example from a book import. Let the transport accept 1,200 requests in a 5-minute window, then return HTTP 429 to every request for 5 minutes, then accept again. Run the loops through the lockout and after it. | Each loop stops sending at the first 429, with a warning, and keeps the failed and later records. No record is lost, skipped, or reordered. Receive requests during the lockout fail like other read failures (V05). After the lockout, later loops send the rest in rowid order. No back-off timer, retry ceiling, or request budget is added (C4, design section 14). |

## 7. Receiving, ordering, and acknowledgement

The design's group size is **50 D1 rows**, including pieces; it is not 50
assembled records. Use non-consecutive rowids in addition to ordinary ones.

| ID | Procedure | Expected result |
|---|---|---|
| V01 | Receive 0, 1, 49, 50, 51, 100, 101, and 1,000 small complete records. | Reads are ordered by rowid and limited to 50 initially. Drain all available complete work in the same loop. Acknowledge each handled group with one prefix delete; an empty read requires no delete. Any final empty read is accounted for separately from the approximate 40 requests for 1,000 rows. |
| V02 | Send dependent records: create parent, create child, update child; create forum, append chat; rename note, then update its new path. | Apply in queue order, not sorted by kind or name. Later records see earlier committed changes. |
| V03 | Put valid A, invalid B, valid C in one group; inject B's failure after a partial local mutation. Repeat for note, chat, and config kinds. | A and C commit. B rolls back as one record, warns, and is deleted with the handled group. A failed record cannot undo earlier records or leave half its own changes. |
| V04 | Append incoming rows while an earlier group is applying/being deleted. | `DELETE ... WHERE rowid <= last_handled` removes only handled rows. New larger rowids survive and are processed in order, this or a later loop. |
| V05 | Fail a read before any row and after one committed/acknowledged group. Restore access. | No fabricated rows or acknowledgement of unread rows. Prior committed groups remain; remaining rows can be retried. No busy failure loop. |
| V06 | Fail acknowledgement before remote delete; separately execute delete but lose its response. | Do not undo applied local changes. If rows remain, replay may duplicate effects; if delete committed, they do not replay. Do not claim loss or duplicates were prevented by a local receive checkpoint. Unhandled rows remain. |
| V07 | Terminate after local apply commits but before remote delete; separately terminate after remote deletion. Restart. | First boundary can replay; second does not. Local committed data survives. No partial transaction or unsupported exactly-once mechanism. |
| V08 | Force result-size errors at successive limits, down to one row; recover at an intermediate limit in another variant. Run the next loop. | Halve the integer row count until usable, never below one. Preserve order and assembly state during retry. At one, continued failure warns and ends the attempt without a hot loop or row loss. A fresh loop starts at 50. |
| V09 | Cause a chat reservation failure at the first, middle, and last row of a group; repeat for an assembled multi-piece chat record. | Stop receiving at that record. Previously handled prefix can be acknowledged; the blocked record's **entire** piece set and every later row remain. Next loop retries and applies after release. |
| V10 | Remove the own table before a read, between read groups, and before acknowledgement. | Worker stops and logs the recovery error. It does not silently create a new desktop queue, switch to another queue, or erase local outgoing work. Already applied data stays. |
| V11 | Queue only malformed JSON; then scalar/array JSON, missing/unknown/non-string `kind`, missing fields, and wrong field types. Follow each with a valid sentinel. | Each invalid complete record warns, leaves no partial change, and is acknowledged under R41. Sentinel still applies. Warnings contain no API token. |
| V12 | Supply malformed HTTP row structures, nonnumeric/invalid rowids, missing record text, and response shapes inconsistent with a query. | Treat an untrustworthy response as a read/protocol failure; do not acknowledge an invented prefix. Recovery with a valid response can proceed. Invalid *piece metadata in otherwise identifiable rows* is covered by F08. |
| V13 | Apply a mixed group containing note, chat, and config changes with clean outgoing state, then with unrelated pending local work. | No echoed record/new flag; existing pending work is not cleared. One notes event per read group that actually changes notes, including managed-folder effects, after committed changes. No event for a group with no note change. |

## 8. Piece boundaries and reassembly

Let `B = 1,900,000` bytes of serialized UTF-8 record text. Do not lower B to
make the real boundary test cheaper. Smaller hand-authored pieces are useful
for ordering faults, but cannot pass the production split-size cases.

| ID | Procedure | Expected result |
|---|---|---|
| F01 | Produce valid records of B−1, B, B+1, 2B−1, 2B, and 2B+1 bytes. Include a minimum-size valid record. | At most B bytes per piece; exact multiples do not create an empty trailing piece. Single rows use `piece=1,pieces=1`; multi-piece numbering is complete and ordered. Whole-record hash and decoded contents match after receive. |
| F02 | Put 2-, 3-, and 4-byte UTF-8 characters across every possible split position around B. Also split near JSON escapes, newlines, and escaped surrogate sequences. | Cuts occur only at UTF-8 boundaries. JSON need only become parseable after complete reassembly. No lost, duplicated, or replaced bytes. |
| F03 | Round-trip large `note.create`, `note.update`, `chat.entries`, `chat.add_file`/`chat.add_note` where local limits permit, and `config.edit` with pictures. Exercise the shared split/send path for all other record kinds with valid large fields where their normal validation permits. | Same framing rule for every kind. No D1-specific save refusal or per-kind lower cap. For kinds bounded below B by existing validation, mark large production input inapplicable and test their serializer/send path without inventing invalid user operations. |
| F04 | Create/update a note at 12 MiB of decoded content and one byte beyond the existing limit. Use poor compression for the valid case. Try chat attachment text at its 256 KiB limit and above, and prompt limits from the actual configured build. | Existing content limits remain. Valid large content saves and transfers using pieces; over-limit local input retains its existing rejection and leaves no outgoing record for a failed operation. A large generated response/config record has no new D1-specific size limit. |
| F05 | Receive a complete set within one group; sets beginning/ending at rows 49, 50, and 51; and a valid record with 51 or more pieces spanning several groups. | Nothing applies until the last piece. No set row is acknowledged before the complete record is applied or deliberately skipped. Complete sets spanning groups do not re-read forever or duplicate their prefixes. |
| F06 | End the queue after the first or a middle piece. Run another loop with no added rows, then append the missing continuation. Repeat with process restart between loops. | Keep the entire unfinished set in D1. No partial application, persistent local receive state, or spinning. Later reads start from retained pieces and apply exactly one complete record if no other failure caused replay. |
| F07 | Fail sending at piece 1, a middle piece, and the final piece, including partial recipient success and lost response. Restore access on a later loop. | Keep the whole local record; retry from piece 1 to all recipients. An old incomplete prefix is warned/skipped when the new first piece breaks its sequence. Complete retries apply; accepted duplicates remain possible. |
| F08 | Inject orphan piece 2; missing piece; repeated piece; descending order; changed `pieces` count; zero/negative/out-of-range numbers; and `piece != 1` with `pieces=1`. Follow with a valid record. | Identifiable invalid rows/sets are warned and skipped, not applied as content. An incomplete set interrupted by another row becomes handled; the interrupting row is then processed normally. A valid following record is not swallowed. |
| F09 | Interrupt an open set with a whole record, a new first piece, and an orphan middle piece, separately. | Skip only the broken set and invalid pieces. Apply the standalone record or assemble the new set as appropriate. Delete only the resulting handled prefix. |
| F10 | Join all pieces into invalid JSON or a semantically invalid record. | Reassembly completes, then R41 applies: rollback/no change, warning, acknowledgement of the entire set, and continued processing. |
| F11 | Put a completed record before an unfinished set and another valid record after a reservation-blocked set. Test acknowledgement failure too. | Earlier handled prefix can be deleted; no delete crosses the unfinished/blocked set. Retry may duplicate an applied but unacknowledged record, never silently discard a blocked one. |
| F12 | Interleave pieces from two senders with compatible piece counts and then with incompatible counts. | Record observed loss/corruption/skips under unsupported concurrent writing. There is no record ID to distinguish compatible interleaving; do not assert reconstruction of the original two records. Parser/application failures still follow R41 and must not crash or damage unrelated content. |

## 9. Notes and folders

For N01–N06, run each outgoing operation on macOS and Windows. Deliver each
kind to all three receiver platforms. Verify all fields listed in design
section 7.2, including false/zero/empty values, not only text and names.

| ID | Procedure | Expected result |
|---|---|---|
| N01 | Create an empty and populated note, a top-level folder, and a nested folder/note. | Immediate `note.create` for each operation; complete path and item type, content, read-only, scroll, and compressed-text values. Receiver creates equivalent items under its own IDs. |
| N02 | Rename a note and a folder containing descendants. Delete a note, an empty folder, and a populated folder. | Immediate `note.rename`/`note.delete`. Subtree paths and ordinary local cascade behavior remain correct. After folder rename, a later child record uses the new path. No invented note-move operation. |
| N03 | Set read-only true and false; compress, recompress, and clear/change compressed text through each existing successful compression commit route. | Immediate `note.read_only`/`note.compress` containing the actual new value. Receiver changes the intended item. Failed compression/validation produces no success record. |
| N04 | Save text once and several times between loops; load replacement content into an existing note. Pause before collection and inspect flags. | Saves/content loads set the flag, not an immediate text-update record. One collection transaction captures the latest content and associated fields, appends `note.update`, and clears the flag. Subsequent save sets it again. |
| N05 | Save scroll alone on an unflagged note; then change scroll on a flagged note before collection. Use zero and a nonzero position. | Scroll-only creates no record/flag. A later content update includes the current scroll value. Receiver gets it as part of that content update. |
| N06 | Import a book with several chapters and import a nested folder tree with notes/read-only items. Force import rollback halfway. | One `note.create` per successfully imported item, parents first, content and metadata included. Rollback leaves neither imported items nor their records. Generated ANNOTATIONS are excluded. |
| N07 | Create a chat summary; replace an existing summary. Receive with the referenced chat present and absent. | New summary uses `note.create` with `summary_of`; replacement sets the text flag. Present chat gets the link; absent chat does not prevent note creation but leaves it unlinked. |
| N08 | Apply `note.update` to editable and read-only notes. Repeat with identical text and with changed text; seed note audio and attempt state. | Direct incoming write replaces all designated fields despite read-only state. Audio is removed even for identical text. Normal local save of a read-only note remains refused and does not flag/queue a failed save. |
| N09 | Update a missing note with an existing parent; then delete locally and deliver an older update. | Receiver creates the note from the full update. Resurrection is accepted under R49. No version/tombstone comparison. |
| N10 | Create/rename into a taken name: existing note, folder, `Name (1)`, `Name (2)`, and a gap in occupied suffixes. Update a path occupied by a folder. | Use the first available suffix according to the existing naming function; preserve the incumbent item. Updating a folder path creates a suffixed note. No ordinary item is overwritten merely to resolve a create/rename collision. |
| N11 | Apply each relevant note kind with a missing parent, a non-folder ancestor, missing target, or wrong target type. | Missing required parent/target is skipped with warning and no fabricated ancestor. Exceptions are missing-note creation by `note.update` and idempotent effects permitted by existing deletion behavior. No unrelated path is changed. |
| N12 | Use paths with the same leaf name in different folders, Unicode, quotes, slash-like characters accepted by note names, and different local numeric IDs. | Resolve exact path components as a JSON array, not a joined SQL/path string or source numeric ID. Only the intended item changes. Invalid paths/types follow R41. |
| N13 | Rebuild ANNOTATIONS after incoming create/update/rename/delete/compress and summary changes; use different source IDs on A and B. | Receiver rebuilds from its own notes and local IDs through the existing function. Stale entries disappear. No ANNOTATIONS payload, flag, or outgoing record, including from a local rebuild. |
| N14 | Create, rename, and remove forum-managed folders through configuration actions. | Configuration drives the folder effects on both machines. No duplicate note records for those managed operations. Subsequent user-note operations still record normally. |
| N15 | Save a flagged note, then rename it, delete it, toggle read-only, or compress it before collection, separately. Also collect first, then do each operation. | Record order reflects committed operation order. Collection uses the surviving note's current path/fields; deletion cannot leave a phantom flagged note. Receivers reach the ordered result without stale-path text overwriting an unrelated item. |
| N16 | Hold flag collection before commit; fail insertion or flag clear; save another edit immediately after a successful collection. | On failure both queue append and flag clear roll back. On success the later edit remains flagged for the next loop. No committed edit is lost between the queue snapshot and flag clear. |
| N17 | Fail serialization or local storage during create/rename/delete/import; separately lose D1 access during the same valid operations. | Local transaction failures retain prior local state and no phantom record. D1 failure does not reject the valid local operation; record/flag survives for retry. |
| N18 | Receive compressed/base64 content that is invalid, truncated, not a valid note document, or over the existing decoded-size limit. | Invalid record rolls back, warns, and is acknowledged. No partial note/audio/link mutation or unbounded decompression caused by ignoring the existing note limit. |
| N19 | Create a collision-induced suffix on the receiver, then send an update/rename/delete using the sender's original path. | Apply literal path semantics; there is no hidden mapping from original path to suffixed item. Record any resulting divergence as a consequence of accepted collisions, not an expectation of automatic repair. |
| N20 | On an open desktop note, receive text, metadata, rename, and delete while the editor has unsaved text and a pending debounce save. | Screen/tree reload through the existing path. Unsaved text can be replaced; an old editor callback must not save it back over the received content or into another note. Deleted/renamed selections resolve through existing screen behavior. |

## 10. Configuration, credentials, and stored IDs

### 10.1 Ordinary configuration operations

| ID | Procedure | Expected result |
|---|---|---|
| K01 | Through Settings, create, edit, rename where supported, and delete each of forums, characters, personas, providers, styles, voices, ordinary API keys, and R2 keys. Include associated picture rows and allocator rows when the operation changes them. | One `config.edit` per committed action, with only actually changed ordinary rows in `set`/`delete`. Stored IDs and references are unchanged. Keys/pictures reach receivers. D1 credential rows are excluded. |
| K02 | Make one action change several interdependent rows, such that an intermediate subset would be invalid. Include simultaneous additions, changes, and deletions. | Rows travel in one record and are overlaid on current receiver rows before one validation/save. No partly active configuration. A large record uses pieces, not separate independently applied row records. |
| K03 | Make a no-op action, a D1-only change, and an ordinary-row change alongside D1 key relocation. | No whole-configuration dump or unrelated rows. D1-only change/relocation emits nothing. Ordinary changes are still sent, including an ordinary key at the former D1 path. |
| K04 | Import a configuration removing zero, one, and several forums. Repeat the removal through merge and a received `config.edit`. Populate those forums with closed and open chats and notes. | All removed forums' chats are pruned in the same transaction as configuration changes. Surviving forums/chats remain. Managed-folder behavior matches ordinary configuration saves. Record contains `set` and `delete`, not a single deleted-forum shortcut. |
| K05 | Inject failure during candidate validation, row writing, orphan pruning, and local queue append, separately. | Atomic rollback of config, dependent deletions, and outgoing record. Active configuration remains the prior committed version. No partial relocation or counter update. |
| K06 | Receive an invalid required reference, invalid TOML, incompatible dependent deletion, and a mixed valid/invalid row set. Follow each with valid config. | Skip the whole invalid result with warning, acknowledge it, then apply the valid sentinel. Other records in the receive group still commit. |
| K07 | Include unused/obsolete settings accepted by the existing configuration policy. Separately change an active provider/character setting. | Do not add stricter sync-only validation that blocks an otherwise usable operation. Warn for ignored obsolete settings. Valid changes become active as after a Settings edit; a subsequent ordinary chat uses them. |
| K08 | Deliver the same ordinary IDs with different content from another machine, including a forum whose ID already has chats. | IDs are retained; records overwrite according to arrival order. No remapping, version comparison, or new conflict-copy rule. Chats continue to use that ID, as the design accepts. |
| K09 | Apply an incoming config record while a chat in a deleted forum is generating and another chat is idle. Hold maintenance/reservation, then release. | No live controller writes into pruned rows or stale configuration. On a retryable reservation failure retain the incoming record; on success apply atomically and leave surviving chats usable. Record any local configuration notices separately; none are transmitted as user content. |

### 10.2 R56 preservation matrix

Execute **every** combination of the following axes, using a fixture with an
unrelated pending record and a flagged note. Pause ordinary Worker draining
while checking that an operation itself preserved this pending work.

| Axis | Required variants |
|---|---|
| Operation | Configuration import; vault merge; received `config.edit`. |
| Destination | Complete D1 setup; no D1 setup. |
| Source D1 rows | Absent; same values; different values; malformed/incomplete D1 values identifiable as D1 rows. |
| Ordinary change | At least one valid ordinary change in every combination, to prove the operation was not simply rejected or ignored. |

For received records, inject source D1 rows directly: compliant senders omit
them, but preservation must not depend on sender filtering. An absent source
setup plus a populated destination exercises replacement-import deletion
pressure. Add explicit `delete` entries targeting the local D1 paths.

| ID | Procedure | Expected result |
|---|---|---|
| K10 | Run the complete preservation matrix above, then allow a Worker loop where connected. | Destination D1 **values or absence** remain unchanged. Source D1 rows are ignored with warning where present. Normal changes succeed and follow normal outgoing rules for local import/merge; received config produces no echo. Pending records/flags are not reset. The next request still uses the destination account/database/token, verified without recording the secret. |
| K11 | Repeat import, merge, and receive with a source ordinary key occupying the destination D1 key ID/path. Include a provider that references that ordinary key. | Source ordinary key keeps its ID and content. Only local D1 rows move to a free ID, preserving their values. Provider/reference IDs are not remapped. Connection and pending work remain. |
| K12 | Run the collision with the next allocator ID free, occupied, below the largest retained key, and with several high retained keys. Create another ordinary key afterwards. | Existing allocator finds a genuinely free ID; counter advances above all retained/allocated IDs. The next creation collides with neither D1 nor ordinary keys. No-collision case keeps the original D1 ID. |
| K13 | Inspect outgoing deltas from both local commit paths after relocation. On a receiver, seed D1 at that old ID too. | Old and new row sets filter D1 independently. Ordinary row at the former D1 path is included; moved D1 rows/values and D1-only deletions are excluded. Receiver preserves its own D1 values while keeping the ordinary key's ID. |
| K14 | Incoming config deletes the current D1 path; then deletes/sets it as an ordinary key in the same action. Repeat with no local D1 setup. | Deletion cannot disconnect a populated destination. An ordinary replacement keeps its ID and relocates local D1 if needed. An unconnected destination remains unconnected. No outgoing relocation record. |
| K15 | Force final validation/commit failure after candidate D1 relocation. | Original key paths, counter, credential values, connection, and pending work all remain. No partial ordinary changes or relocation record. |
| K16 | Import/merge a source containing D1 rows but no other changes; compare with a full file copy and R2 replacement from the same source. | Import/merge preserve local setup and warn about ignored source rows. Full delivery carries source D1 values instead, and clears delivered outgoing work under R57. These routes must not share the wrong preservation policy. |

## 11. Chats, terminal turns, and live reservations

### 11.1 Eligibility table

Inspect outgoing state before and after each transition. Exercise a real label
and **each temporary label recognized by the existing application**
(`is_temporary_session_label` in `src/storage/session_label.h`): `New session`
and the older form `temp-ts-cha-` followed by digits. Also use near-miss labels
that are real: `new session`, `temp-ts-cha-`, and `temp-ts-cha-12a`. Do not substitute “has any stored response” for the specified
`next_request_id` and first-turn-running rule.

| Label | First-turn state | Expected recording eligibility |
|---|---|---|
| Temporary | Not started, running, or finished | Not shared. |
| Real | Not started (`next_request_id = 1`) | Not shared. |
| Real | First turn running | Not shared. |
| Real | First turn complete, cancelled, or failed; counter > 1 | Shared. |
| Real | First turn/history deleted or cleared after finishing; counter > 1 | Still shared, including after restart. |

### 11.2 Outgoing and incoming behavior

| ID | Procedure | Expected result |
|---|---|---|
| H01 | In every unshared state above, add `@-` entries, note/file/folder attachments, character overrides, and any supported rename/clear/delete actions. Delete an unshared chat. | No chat content records until eligibility is reached. Deletion of an unshared chat emits no record. Welcome and notices remain local. |
| H02 | Let naming complete before the first response; then let the first response finish before naming. Repeat with complete, stopped, and failed first turns. | The transition to shared emits all then-existing transferable content once. Finished turns and `@-` messages follow the sender's entry order. Attachments and character settings are not entries (F6), so the design sets no order for them relative to the entries; check only that each one is present once. The triggering turn/action is not also emitted a second time (R29). Current label/identity accompany every chat record. |
| H03 | Make title completion and terminal-turn commit adjacent/concurrent in one application. Repeat rename from temporary after several finished turns. | Exactly one initial publication for a single transition. Include all finished turns, `@-` content, attachments, and effective character settings; no half turn or omitted earlier content. |
| H04 | Stop or fail the first and a later turn before any response bytes; repeat after partial response bytes. | O2 eligibility uses terminal state, even without response text. Preserve actual terminal state and any actual content; never fabricate a reply. A turn with only its prompt adds no record, also in the initial records, but it still makes the chat shared (R59). A turn with partial response bytes moves with its actual state. |
| H05 | Open a chat with an interrupted running turn; repair it as failed. Use both temporary and real labels. | Journal repair has the same transactional recording rules as a normal failure. If it makes the chat shared, initial records are produced; otherwise they wait for a real label. Reopening an already repaired chat does not publish again without another transition. |
| H06 | In a shared chat start/stream a later turn, then complete, cancel, or fail it. Pause before and after terminal commit. | No partial prompt/response publication while running. One `chat.entries` for the finished turn with its actual entries and state. Terminal mutation and record share a transaction. |
| H07 | Add `@-`, configuration notice, no-turn error, and Welcome content before and after sharing. | `@-` moves, including in initial publication. No-turn notices/errors and Welcome do not. A chat with only `@-` messages never becomes shared just because it has a real name. |
| H08 | Rename an open and a closed shared chat between real labels; rename two distinct chats to the same label. | `chat.rename` addresses forum/session IDs. Both chats remain distinct; label is not a unique key. Closed-chat repository actions record too. |
| H09 | Rename shared chat to temporary, change it, then rename to real again. | Temporary state suppresses later recording as designed. Returning to real emits all current content again; duplicate turns on receivers are accepted. No extra shared-state flag or automatic deduplication. |
| H10 | Clear history; delete the only turn; delete first/middle/last turn of several. Try to delete a turn that has only its prompt. Restart and add another turn or attachment. | Immediate `chat.clear`/`chat.delete_turn` for an eligible chat. The existing local delete does not accept a turn with only its prompt (F24), and no record is added. Eligibility survives lost turns because the counter is not reset. Later operation is incremental, not a repeat initial snapshot. |
| H11 | Delete an open, closed, and generating shared chat through their normal routes. | `chat.delete` uses eligibility before deletion. Ordinary local stop/delete rules still work; record is committed with the durable action where transactional. Receiver deletes the addressed chat. |
| H12 | Add a note with a live source; rename/change the source; attach two notes with equal leaf names from different folders. Deliver to a receiver with different local IDs. Deliver once more to a receiver that does not have the source path. | `chat.add_note` contains source path plus the saved copy's name/text. Receiver resolves source path but preserves the transmitted copy, not the receiver's latest source text. No source numeric-ID dependency. When the source path is missing on the receiver, the record is skipped with a warning and acknowledged (design section 10.3, R41); the chat gets no copy, and a chat that the record would create is not left behind. |
| H13 | Attach a note before first share, delete its source, then finish/naming the chat. Repeat with another surviving attachment alongside it. | Deleted-source copy stays locally but has no outgoing `chat.add_note`; surviving attachments move. No conversion into a file, source resurrection, or substitute path. |
| H14 | Add empty/nonempty files, Unicode names/text, maximum allowed file text, and multiple files. | `chat.add_file` retains name/text through initial and incremental recording. Existing attachment limit still applies. No filesystem access on receiver is needed to reconstruct transmitted text. |
| H15 | Attach and detach a folder, including before first publication and after sharing. Receive with folder present/missing and with different local IDs. | Correct `chat.add_folder`/`chat.detach_folder`, resolved by path. Missing folder warns/skips. Initial publication reflects what is actually attached, not already detached items. |
| H16 | Set/change/reset supported character, provider, and reasoning-effort overrides. Include initial publication and shared incremental changes. | `chat.character` carries the intended stored IDs/values; no ID translation. Subsequent receiver reply uses the resulting valid settings. Unsupported/malformed values follow ordinary validation/R41. |
| H17 | For every chat kind, receive with its forum present and absent; receive with chat present and absent. | Missing forum skips with warning. Otherwise create missing chat when needed using exact session ID and label; schema defaults `recent_pending=0`, `discardable=0`. Apply the operation afterwards: a delete of a missing chat ends absent, and a clear can end empty. No orphan/partial chat from a failed record. |
| H18 | Receive a finished turn and `@-` into a chat with non-contiguous, higher local entry numbers and local request IDs. Then submit a local reply. | Append with next free strictly increasing entry numbers, correct prompt/response association and terminal state. Do not import sender entry numbers. New local reply uses refreshed counters without collision. |
| H19 | Receive turn deletion at positions 1, middle, and last; invalid 0, negative, past-end, and wrong type; include no-turn entries and gaps from earlier deletions. | Position is among applicable turns, from 1, not request ID or entry number. Correct response entry is resolved locally. Missing/invalid target warns/skips without deleting another turn. Turns with only a prompt are not counted, on the sender or on the receiver (R59): put such turns before and between counted turns on both machines, and check that the receiver deletes the same logical turn. |
| H20 | Deliver valid chat records to idle, generating, and closed chats. Hold the live controller while the Worker reserves it; attempt a local submission/reconnect during the hold. | Reservation stops and waits before database write; shutdown reason is `reloading`. Running reply is saved cancelled. No stale entry number or concurrent write crosses the reservation. Release permits reopen with current data. |
| H21 | Make reservation fail, then succeed next loop. Put a valid earlier note record and later chat record around it. | Earlier committed prefix may be acknowledged; blocked and later records stay. No cancellation/applied half-record is treated as success. Retry eventually applies and releases all reservations. |
| H22 | Inject apply/commit exception after reservation; delete/clear the open chat; switch vault while callback is pending. | Reservation/lease is released on every exit. No permanent “busy” chat, reconnect loop tied to a removed chat, stale callback into a new vault, or stale database handle. |
| H23 | Let incoming chat/config maintenance cancel a running local reply; inspect the outgoing queue before and after cancellation and apply. | The induced cancellation ends a local turn, so design section 8.3 applies: in a shared chat, `cancel_turn` adds one `chat.entries` record for the cancelled turn, in the transaction of the turn change, before the Worker applies the incoming record (F27, design section 13). A cancelled turn with no response entry adds no record (R59). Applying the incoming operation itself emits nothing (R38), and the cancellation record does not repeat the received content. Do not hide the result by clearing the queue. |
| H24 | Replay identical entries; create the same forum/session ID independently on two offline machines; deliver in both orders. | Duplicate append and same-ID merging are accepted. No session-ID remapping or label-based second chat. Later operations still run without corrupting unrelated chats. |

## 12. Worker lifetime, isolation, and shutdown

| ID | Procedure | Expected result |
|---|---|---|
| L01 | Open a connected vault with queued incoming config, notes, and chats. Hold the initial D1 response; release it successfully. Repeat offline. | One initial loop finishes before the first content screens load. On unreachable D1, warning and ordinary local opening continue after normal request failure/timeout. No indefinite connection prerequisite for local use. |
| L02 | Observe two real 30-second intervals with no work, then add a local change and an incoming record. Separately use a controlled clock for boundaries. | One in-process Worker/thread for the open connected vault; periodic loops at the intended cadence. Flag collection precedes send, which precedes receive. No overlapping loops or busy empty polling. Long loops do not spawn catch-up Workers. |
| L03 | Keep vault X open while Y has D1 rows. Switch X→Y→X, with distinct database paths, passwords, D1 credentials, and table names. | Only open vault syncs. Y's rows wait while closed. Old Worker flushes/stops before new vault use; Y gets its initial loop. No cross-vault writes/events/credential use. |
| L04 | Close a vault and close the application with an unsent structural record, a flagged note, and a large record. Repeat at an idle loop boundary and while a periodic request is active. | Quiesce the active loop, collect flags, make one final send pass, then stop. No concurrent periodic/final pass. Unsent work remains if any send fails. Shutdown does not require receiving incoming backlog. |
| L05 | On each actual native host, make successful individual requests whose complete final send pass takes more than 10 seconds and then substantially longer, using several records/pieces. | Host remains alive until the complete attempt finishes; all confirmed records are removed. Former 10-second host deadline does not terminate the pass. Each request still has normal timeouts; no total drain deadline is introduced. |
| L06 | During final flush fail the first, middle, and final record/piece, separately. Repeat offline. | Stop sending at first failure; no retry in that same pass. Earlier confirmed records may be removed, failed/later records survive reopening. Host exits after the failed attempt ends rather than waiting forever for remote availability. |
| L07 | Close/switch while a reply is generating and while an editor save is pending. | Existing shutdown saves/cancels user work before the final send snapshot where required. Include resulting terminal records/flags in that pass; no database writer continues after its Worker/database lifetime. Reopen verifies retained local content. |
| L08 | Terminate the process before a local transaction commits, after commit but before send, during a multi-piece send, and after remote success before local deletion. | Uncommitted transaction rolls back. Committed unsent work survives. Partial sends retry from piece 1; successful-but-unacknowledged sends can duplicate. No general startup cleanup of pending work. |
| L09 | Suspend/sleep with work created less than 30 seconds earlier, then resume. | No guaranteed lid-close flush. Work can wait until wake and the next successful loop. Local work remains durable; normal sync resumes. |
| L10 | Enter maintenance for R2 upload/download and other existing database maintenance; hold its lock, then complete or fail it. | Worker is stopped/excluded while handles/database can be replaced. Recovery restarts the correct Worker only when appropriate. No stale password, old database handle, or temporary-file write. Failed maintenance leaves the original usable state. |
| L11 | On daemon start, cross own-table present/missing with pending local work absent/present. Repeat with plain/protected vault and D1 unavailable. | Connected daemon alone may create its missing table at startup. Creation retains pending work/flags; normal loop handles them. Offline errors do not silently discard content. No Settings/R2 workflow is added to ChaWeb. |
| L12 | Drop daemon's table while it runs, then wait a loop and restart. Perform the same test on desktop. | Both running Workers halt and log. Only daemon restart recreates automatically. Desktop ordinary restart/open does not; explicit Save or qualifying R2 replacement is needed. Recreated table restores future sync, not lost contents. |
| L13 | Make credentials unavailable or invalid while another unrelated local feature is used; close/reopen the disconnected vault. | Optional sync state does not block unrelated valid work. No new records/flags without connection; preserve already pending work unless explicit disconnect/delivery clears it. T18 tests R2 upload in this state. |
| L14 | Hold a network request while making an unrelated local save/read. Hold a local transaction briefly while a Worker needs it. | No network wait inside a local SQLite transaction; normal local work does not wait for remote timeouts merely because sync is in progress. Busy/lock outcomes follow existing local handling, without partial commits or deadlock. |

## 13. Screens, bridge contracts, and ChaWeb

This section verifies approved UI only. It is not approval for new labels,
loading copy, status panels, or layouts. Use the approved design/mockup and
existing error placement as the visual reference. Test both native hosts.

| ID | Procedure | Expected result |
|---|---|---|
| U01 | Open Settings → API Keys → D1 sync, with no saved setup and with saved setup. Repeat with saved credentials while `machine` is absent, then invalid. | Row `D1 sync`; description `Configure synchronization between machines`; fields Account ID, Database ID, API token; buttons Reset and `Save D1 credentials`; `Disconnect from D1` whenever D1 credentials are saved, also with no valid machine name (R60). No extra title/legend or nonessential explanatory copy. |
| U02 | Inspect empty/saved placeholders; edit each field, Reset, navigate away/back, save with blank current token, and save with a replacement token. Leave Account ID or Database ID empty or only spaces, and leave the token empty when none is saved. Open a saved form and change nothing. | Placeholders match design: `Paste account ID`, `Paste database ID`, `Paste API token`, or `Leave blank to keep the current token`. Reset restores saved form state without remote mutation. Detail/loading never exposes saved token value. Save stays disabled while a required value is empty, as on the R2 storage screen (F22). Unlike the R2 screen, Save stays enabled for saved values with no edit, because design section 12.3 uses it to make a missing table. |
| U03 | Open Disconnect and inspect/cancel/confirm using mouse and keyboard. | Title `Disconnect from D1?`; message `Disconnect this vault from D1 on this machine? Its D1 queue and its unsent changes will be deleted.`; buttons `Cancel` and `Disconnect from D1`. Cancel does nothing; confirmed behavior matches C07–C08. |
| U04 | Trigger each error in the text table below, then fix the condition and retry. | Exact approved text appears at the existing R2-style error location; successful recovery works. No generic success display after failure. Loading/busy wording must have separate approval if added; tests do not invent it. |
| U05 | Delay loading/save/disconnect; double-click the action; navigate or switch vault before reply. | One committed action/Worker, appropriate existing busy behavior, no credential leak, and no late reply modifying the new vault/screen. Form remains usable after failure. |
| U06 | Exercise native detail/save/disconnect requests through the real bridge, including missing/wrong argument types and stale context epoch. | Wire DTOs, policies, frontend client, and router agree. Invalid/stale calls cannot mutate the wrong vault. Detail returns token presence, not secret text. Existing native-only boundaries remain; ChaWeb gains no Settings/copy/R2 endpoint by accident. |
| U07 | Receive multiple note records in one group, then across two groups; include a group changing only chat content. Observe notes events/tree/editor. | One `notes.changed` per note-changing group. Tree and open note update after commit, including create/rename/delete and managed-folder changes. No note event storm per record or event for chat-only work. |
| U08 | Apply incoming chat entries/rename/clear/delete to open and closed chats on both desktop hosts, including a generating reply. | Idle view shows change after reconnect; generating view stops and later reflects committed data. Reconnect while reserved can fail temporarily but recovers after release. New submission uses correct entry numbers. Closed-chat list reflects normal rename/delete behavior. |
| U09 | ChaWeb: after a successful idle chat read, observe before 30 seconds and at the next scheduled read. Repeat while generating and when generation ends. | Idle success schedules one read after 30 seconds; active reply retains the 1-second delay. Transition uses one timer, without simultaneous idle/active timers. Incoming idle-chat change becomes visible on the next successful read. |
| U10 | ChaWeb: hide page before timer fires, keep hidden over several intervals, show it, change chat route, navigate away/back, and close/remount the page. | Hidden pages remain paused. Open/return reads immediately. Old timers/responses do not update another chat or continue after unmount; at most one scheduled read remains. |
| U11 | ChaWeb: fail reads while idle and generating, hold a reservation, recover, then return from hidden state. | Existing failure retry path remains effective. A successful idle read resumes the 30-second delay; active read uses 1 second. No permanent loading/error state after release, or multiplication of retry timers. |
| U12 | ChaWeb notes: leave a note page open while its content/tree changes remotely; navigate away and reopen. | Reopening shows current content. No live editor or note-write capability is introduced; immediate in-place refresh is not an acceptance requirement for ChaWeb notes. |
| U13 | Halt Worker through missing own table, then continue local use and inspect the log. | Error explains R2/Save recovery. The design permits no visible stopped-sync indicator; do not fail because no new banner exists. Local use stays available. |

Approved error text checks:

| Trigger | Exact text |
|---|---|
| R2 upload still has pending outgoing work | Changes are not sent to D1 yet. Try again when D1 can be reached. |
| Save, D1 request fails | D1 could not be reached. Check the account ID, database ID, and API token. |
| Save, absent/invalid machine name | This machine has no name. Add machine to app.toml, then save again. |
| Save, local failure | D1 credentials could not be saved. |
| Screen load, local failure | D1 credentials could not be loaded. |
| Disconnect, local failure | D1 credentials could not be removed. |

## 14. R2 upload, vault delivery, and recovery

### 14.1 Upload

| ID | Procedure | Expected result |
|---|---|---|
| T01 | Upload with queue/flags in all four combinations: empty/zero, pending/zero, empty/flagged, pending/flagged. Use reachable D1. | Under existing maintenance exclusion, collect flags and send once **before** checking. Upload proceeds only after both are clear. Snapshot contains final local content and no pending outgoing state. |
| T02 | Repeat the pending variants with D1 unreachable, partial write, own queue missing, and flag-collection transaction failure. | Refuse R2 upload while any outgoing work remains, using approved text. Do not replace the remote snapshot, clear pending work, or misreport success. Vault returns to usable state; restore access and retry successfully. |
| T03 | Queue has work but no other registered machines; separately there is incoming remote backlog but no local pending work. | No-recipient send can clear local work and allow upload. Incoming backlog alone is not the upload-block condition; this preparation is collection/send, not a receive drain. |
| T04 | Attempt a local save between final flush/check and snapshot creation. | Existing maintenance serializes/excludes the writer, so a successful snapshot cannot capture unchecked new outgoing work. A later edit remains for the next normal loop. |
| T05 | Upload a normally disconnected, clean vault; then exercise R2 authentication failure and existing overwrite/ETag conflict. | D1 never blocks the disconnected clean upload. Existing R2 failure/overwrite behavior remains; no sync-specific change discards local data. T18 covers a vault that has pending work but is no longer connected. |

### 14.2 Delivery matrix

Run each supported route below with queue only, flags only, both, and neither
in the delivered source. Repeat with D1 credentials present and absent. Use
both normal and WAL-backed temporary databases; include an old-schema source.
For protected variants, follow the route's existing password support.

| Route | Credential rule | Queue/flag rule and registration |
|---|---|---|
| R2 replacement of open plain vault | Source values replace destination values, including absence. | Clear delivered queue/flags before install; create own D1 table if connected; first loop runs before screens. |
| R2 replacement of open protected vault | Same; use existing supplied password. | Same cleanup and ordering, with encrypted database access/checkpoint. |
| R2 download as a new unopened vault | Keep source D1 values. | Clear before return/install. A missing own table on later desktop open uses explicit Save recovery, as design section 12.3 specifies. |
| Desktop New vault from a local source | Keep copied D1 values. | Clear destination before return; source unchanged. Different vault name means explicit Save is needed to register its table. |
| New empty vault | No inherited connection unless the existing creation route supplies one. | Starts with empty outgoing queue and zero flags. |
| Configuration import/merge into existing vault | Preserve destination values/absence under R56. | Keep destination pending work; not delivery cleanup. |
| Ordinary open/restart/reconnect; daemon missing-table startup | Preserve local values. | Keep pending work; not delivery cleanup. |
| Manual whole-file delivery (Linux installation) | Carries source D1 values. | Not a CHA delivery route, so CHA does not clean it (R58). Copy only after a successful R2 upload with no later edits; a dirty copy is an accepted risk (T19). Do not claim startup can infer provenance. |

| ID | Procedure | Expected result |
|---|---|---|
| T06 | Execute the supported delivery-route matrix. Inspect before first Worker access, then reopen installed DB through a fresh handle. | Queue empty and every note flag zero for delivered destinations; content, compressed text, IDs, links, and source D1 credentials intact. No copied source record is sent and no copied flag recreates it. Retention routes keep their prior pending work. |
| T07 | Download a snapshot with old source credentials over a destination with different credentials and pending records. Repeat with source having no D1 credentials. | Whole replacement follows the source setup, unlike import/merge. Delivered work is cleared. No old Worker uses the old database/password/connection after replacement. |
| T08 | Copy a local vault with pending records/flags; keep source Worker paused. Inspect source and destination independently. Repeat protected source where supported. | Destination clean, source pending work unchanged byte-for-byte apart from unrelated normal metadata. Source can later send its own work. Low-level maintenance copies do not accidentally get new-vault cleanup. |
| T09 | Use WAL-backed download fixtures. Hold cleanup/checkpoint handles; observe order through commit, close, checkpoint, sidecar removal, and install. | Queue/flags clear in one transaction; close cleanup handles; successful checkpoint flushes the cleaned main file; close all handles before removing sidecars/installing. Fresh reopen after sidecars are gone still sees cleanup and all content. |
| T10 | Fail cleanup delete, flag update, transaction commit, checkpoint, and make checkpoint busy, separately. | Abort installation on failure/busy checkpoint. Existing destination and vault config remain valid and unchanged; no partial replacement or loss of only-in-WAL content. Temporary failures use existing cleanup/recovery behavior. |
| T11 | Fail R2 download, validation, wrong-password access, and final install/rename, separately. Retry after removing the fault. | Existing vault remains usable; Worker resumes only against a valid installed/original database. A failed route cannot expose an unclean half-installed delivery. |
| T12 | Download a protected database as a new vault without a password, then replace an existing protected vault using its valid password. | New-vault route retains its existing rejection. Supported replacement cleans successfully. No new deferred-cleanup marker or unlock workflow is invented. |
| T13 | Seed incoming D1 records for destination before replacing its local vault. Keep another machine's queues populated. | Cleanup touches only delivered local outgoing state. Incoming D1 records survive table creation and are applied by the opening loop. Other queues are untouched. |
| T14 | New machine bootstrap, in two variants. (a) Set the machine name, then download the snapshot to replace the open vault. (b) Set the machine name, download the snapshot as a new vault, open it, then select `Save D1 credentials`. Inspect ordering. | `app.toml` name stays local; source D1 credentials arrive; outgoing cleanup precedes use. In (a) the download makes the own queue before the first loop (design section 12.2). In (b) the first open stops the Worker with the missing-table error, and Save makes the queue (design section 12.3). Once own queue exists, subsequent changes arrive. No premature sends of source work. |
| T15 | Make a source edit after R2 snapshot upload but before new recipient registration. Then edit again after registration. | First change may be absent on the new machine (R24); second arrives. No historical backfill from other machines' queues is expected. |
| T16 | Remove desktop's own table, open with pending work, then recover once through Save and once through fresh R2 replacement. | Missing table halts before send. Save resumes future synchronization and retains local work; it does not recover lost incoming content. Fresh snapshot supplies its captured content and clears delivered outgoing work. |
| T17 | Let D1 queue creation fail after an otherwise valid R2 delivery, then restore access and use supported recovery. | No source outgoing work is replayed. Installation/open behavior follows the route's existing error handling; no claim of active synchronization while own table is absent. Preserve a usable installed/original vault and log the remote failure. |
| T18 | Queue records and set note flags while connected. Then make the vault unconnected without Disconnect: remove `machine` from `app.toml`, and separately make it invalid. Request R2 upload. Restore the machine name, allow a loop, and request upload again. | R26 decides: upload is refused with the approved text while the queue has records or a note has a flag. Design section 12.1 says that an unconnected vault is never refused, but that sentence assumes no pending work, which is false here. While unconnected, no records or flags are added, and pending work is not deleted. After the name is restored, the loop sends the work, and the upload succeeds. Disconnect in this state also clears the block (R60). |
| T19 | Linux manual installation (R58). (a) Clean copy: give the source desktop vault pending records and flagged notes, upload it to R2, let incoming records arrive, close CHA without edits, copy the configuration directory to C, set `machine = "test-linux"` in its `app.toml` as the guide says, and start the daemon. (b) Dirty copy: copy a source vault that has pending records and flagged notes, with no upload. Then edit the same note and a setting on the source and let them synchronize. Start the daemon. Repeat (b) with the daemon started before the source edits. (c) Clean copy, but keep the source machine name in the copied `app.toml`. | (a) The upload succeeds only after the queue and flags are clear. The copy has an empty queue and no flags, and the daemon's first loop sends no record that came from the source. The daemon uses its own queue table, and the source queue is not read by the daemon. (c) Accepted O8 risk, as in M10: the daemon and the source use one queue and take each other's records. Record it; it is not a defect. (b) Accepted risk: the daemon sends the copied records again to all other machines, including the source. Record the duplicates, such as `Name (1)` and repeated chat turns, and each old version that replaces a newer one. These are not defects. Daemon restart still retains its own pending work (L11). |

## 15. Complete cross-machine scenarios

These LIVE scenarios join the parts. Do not pass them using direct remote
inserts alone. Generate ordinary changes through CHA, inspect D1 while a
receiver is paused, then use the real receiver Worker. After each handoff,
compare against the independent manifest and submit a new local operation.

| ID | Sequence | Required outcome |
|---|---|---|
| E01 | A creates/edits/deletes notes and folders, changes config, finishes and renames a chat, attaches notes/files/folders, changes character settings, and closes. Open B, compare, make further changes, close B, then open A. | Both directions preserve intended semantic content. Initial open shows received state. All generated record kinds and acknowledgements are accounted for; new operations work after handoff. |
| E02 | A and B separately generate the complete note/config record-kind matrix for C. C generates chat operations through ChaWeb for A and B. Include all six directed machine pairs using at least a shared-chat mutation. | Every receiver platform processes all 17 kinds. Every supported origin path is exercised: note/config UI on both desktops, chat operations on all supported clients. Do not require a nonexistent Linux note editor or Settings API. |
| E03 | Repeat a full note/config/chat handoff with protected vaults, including a large record and R2 replacement. Inspect queued record text. | Local protection/password behavior remains correct; D1 records are plaintext by design. Delivered content matches; no password is included in the record. |
| E04 | Disable D1 access. Make notes/config/chat changes and restart the sender. Attempt R2 upload, restore access, allow a periodic loop, and open the next machine. | Local work survives; upload refuses while pending; recovery sends it and clears the block. Receiver gets content. No arbitrary expiry or retry ceiling. |
| E05 | Import a deep note tree and a configuration changing dependent rows/removing several forums; include key collision with local D1 and a large picture record. | Ordered parents and atomic config changes apply. Receiver retains its own D1 values, prunes all removed-forum chats, rebuilds managed folders/ANNOTATIONS, and remains connected. |
| E06 | Keep a note unsaved and a chat idle on B; send changes from A. Repeat while B is generating. Repeat relevant chat view on ChaWeb. | Note replacement and chat reservation/cancellation follow design. All screens recover; no stale save/controller overwrites received content. |
| E07 | Evict a desktop queue with the controlled full path, then perform real Save and R2 recovery. Repeat daemon table loss/restart. | Desktop halts until explicit recovery; daemon restart recreates. Neither table recreation alone restores lost content. R2 snapshot supplies only its captured state. |
| E08 | End with a slow native close containing a recent import, flagged notes, a terminal cancelled reply, and multiple-piece records. Open another machine after close. Repeat close with a failed middle send. | Successful close waits for the full pass and handoff sees all confirmed changes. Failed close retains failed/later work on sender; receiver may see the accepted prefix/duplicates. |

Platform execution minimum:

| Coverage | macOS | Windows | Linux/ChaWeb |
|---|---|---|---|
| Every incoming record kind, plain vault | Required | Required | Required |
| Every outgoing note/config operation exposed by desktop | Required | Required | Shared backend checks only; no invented UI |
| Supported chat operations and eligibility | Required | Required | Required through ChaWeb plus shared backend |
| Protected open, ordinary sync, pending-work retention | Required | Required | Required |
| Credential screens, errors, confirmation, stale replies | Required | Required | Verify absence of native-only features |
| Real host shutdown exceeding 10 seconds | Required | Required | Daemon stop/start and pending-work checks |
| Idle/generating incoming chat refresh | Required | Required | Required, including visibility/timers |
| R2/copy routes and checkpoint failures | Required where route exists | Required where route exists | Manual setup and startup exception |

Deterministic content/fault partitions can run in the shared backend once if
that same code is compiled and its regression tests pass on every platform.
Do not use that allowance to waive platform-specific hosts, filesystem/WAL
behavior, encryption, curl, browser timers, or native bridge tests.

## 16. Coverage audit

### 16.1 Every record kind

For each row, retain evidence of real outgoing generation, complete wire
fields, successful incoming application, malformed input handling, and no
echo. Include initial-chat publication and incremental generation where both
exist. All kinds use the shared piece tests; do not impose a large payload
where the operation's own valid fields cannot reach B.

| Kind | Outgoing cases | Incoming cases |
|---|---|---|
| `note.create` | N01, N06, N07 | N01, N07, N10–N13 |
| `note.update` | N04–N05, N07, N15–N16 | N08–N13, N18, N20 |
| `note.rename` | N02, N15 | N02, N10–N13, N20 |
| `note.delete` | N02, N15 | N02, N11, N13, N20 |
| `note.read_only` | N03 | N03, N08, N11 |
| `note.compress` | N03 | N03, N11, N13 |
| `chat.entries` | H02–H07 | H17–H24 |
| `chat.rename` | H08–H09 | H08, H17, H20–H22 |
| `chat.delete` | H01, H11 | H11, H17, H20–H22 |
| `chat.clear` | H10 | H10, H17, H20–H22 |
| `chat.delete_turn` | H10 | H17, H19–H22 |
| `chat.add_note` | H02–H03, H12–H13 | H12, H17, H20–H22 |
| `chat.add_file` | H02–H03, H14 | H14, H17, H20–H22 |
| `chat.add_folder` | H02–H03, H15 | H15, H17, H20–H22 |
| `chat.detach_folder` | H15 | H15, H17, H20–H22 |
| `chat.character` | H02–H03, H16 | H16–H17, H20–H22 |
| `config.edit` | K01–K05, K10–K16 | K02, K04–K16 |

### 16.2 Requirement traceability

| Requirement | Primary coverage |
|---|---|
| R1 — independent local databases | M06–M08, E01–E03 |
| R2 — registration | C01–C02, L11–L12, T14 |
| R3 — own queue per instance | M02–M04, S01, E02 |
| R4 — periodic polling | L01–L02 |
| R5 — pull and apply | V01–V13, E01–E02 |
| R6 — delete after application, amended by R40 | V01, V03–V09, F05–F11 |
| R7 — record local operations for others | S01–S09, N01–N07, K01–K05, H01–H16 |
| R8 — ordered Worker application | V01–V04, F05–F11 |
| R9 — no data versions/checks/comparisons | M09, N09, H24, K08 |
| R10 — direct fan-out, no own readback | S01–S03, S05 |
| R11 — successful write is enough | S01, S06–S07 |
| R12 — changed config rows only | K01–K03, K13 |
| R13 — updated note loses audio | N08 |
| R14 — invalid config skips/warns | K06–K07, V03 |
| R15 — name suffix | N10, N19, S06 |
| R16 — absent parent/forum skips | N11, H15, H17 |
| R17 — all application platforms | E02 and platform execution matrix |
| R18 — content inclusion/exclusions | All record kinds; N06–N07, N13–N14, H07, K01; common assertions |
| R19 — in-process threaded Worker | L02–L04, L14 |
| R20 — open vault only, closed queues wait | L03, L10, T13 |
| R21 — bootstrap, amended ordering | T06, T13–T15, L11 |
| R22 — durable local queue and failures | M07–M08, S04–S09, S16, L04–L08, T18 |
| R23 — retries/duplicates/partial writes | P03, S05–S09, S16, F07, V06–V07, H24 |
| R24 — registration timing risk | S11, T15 |
| R25 — identity, superseded in part by R48/R54 | M02–M04, N12, H08, H17–H19, K08 |
| R26 — R2 upload block/download cleanup | T01–T13, T18 |
| R27 — terminal prompt/response pair | H02–H06 |
| R28 — append using next free entry numbers | H18, H20–H22 |
| R29 — initial/incremental sharing and receiver creation | Eligibility table; H01–H11, H17 |
| R30 — replaced by R53 | No obsolete independent assertion; F01–F12 |
| R31 — API/R2 keys move | K01, K10–K16 |
| R32 — note save unchanged, incoming overrides | N04–N05, N08, N20 |
| R33 — one writer, accepted simple behavior | Section 16.3; F12, E01–E08 |
| R34 — protected vaults/plain D1 text | M06–M07, T09–T12, E03 |
| R35 — open chat refresh | P06, H18, H20–H23, U08–U11 |
| R36 — machine/config/table discovery, no registry | M01–M04, M09, C01–C03, S01–S03 |
| R37 — 30-second loop/open/final flush and host wait | L01–L09, U09–U11, E08 |
| R38 — no echo | V13, N13–N14, K09–K15, H23; common assertions |
| R39 — notes/tree and ChaWeb reopen | N20, U07, U12 |
| R40 — 50-row groups and group deletion | V01–V09, F05–F11 |
| R41 — skip/warn/delete, reservation exception | V03, V09–V12, F08–F11, N11, N18, K06, H19–H22 |
| R42 — immediate structural note records | N01–N03, N15 |
| R43 — D1 Settings and explicit actions | C01–C11, U01–U06 |
| R44 — disconnected/D1 edits produce no records | M01, M05, C01–C03, K03, L13, T18 |
| R45 — disconnect clears local state and drops own queue | C07–C10 |
| R46 — full D1, missing table, explicit recovery | P02, S10, S12–S14, L12, T16 |
| R47 — note flag, content load, scroll and upload | N04–N05, N07, N15–N16, T01–T04, T18 |
| R48 — stored ordinary IDs | K01, K08, K11–K15, H16, H24 |
| R49 — update creates absent note | N09–N10 |
| R50 — one atomic config delta | K01–K06, K13 |
| R51 — daemon missing-table startup | L11–L12, E07 |
| R52 — positional turn deletion | H10, H19 |
| R53 — no D1 save cap; pieces for all kinds | P01, F01–F12, N17, S02–S03 |
| R54 — IDs except note paths, chat composite identity | N12–N13, H08, H12, H15–H19, H24 |
| R55 — omit deleted-source chat note | H13 |
| R56 — preserve destination D1 on import/merge/receive | Full section 10.2 matrix (K10–K16); T06–T07 |
| R57 — clear delivered outgoing work on every route | Full section 14.2 matrix |
| R58 — manual copy is not a delivery route; clean-copy steps | T19, L11 |
| R59 — prompt-only turn does not move; positions skip it | H04–H05, H10, H19, H23 |
| R60 — Disconnect shown for saved credentials without a machine name | U01, C07, T18 |

Approved-point audit: O2 → H02, H04–H05; O3 → M05, C07;
O5 → N14, K04; O6 → U01–U06, T18, and error text table;
O8 → M03, M10, C11; O10 → H07, K09, H23.
O4/O7 are replaced by R53 and must not reintroduce a size refusal.

### 16.3 Accepted-risk audit

Each risk in design section 13 has an explicit execution, not just a waiver:

| Accepted behavior | Cases |
|---|---|
| Duplicate delivery | S05–S06, V06–V07, H24 |
| Partial recipient writes | P03, S05, F07 |
| Change missed at registration | S11, T15 |
| Dirty manual copy sends old records again | T19 |
| Two configured items with same ID merge/overwrite | K08 |
| Two chats with same composite ID merge | H24 |
| Unsaved note text replaced | N20, E06 |
| Deleted note returns | N09 |
| Only-`@-` chat never moves | H07 |
| Prompt with no reply does not move | H04, H19 |
| Missing queue stops sync and can lose queued content | L12, T16, E07 |
| Sleeping machine keeps recent work | L09 |
| Deleted-source chat copy omitted | H13 |
| Incoming chat update cancels a reply | H20, E06 |
| Temporary-label rename suppresses/re-publishes content | H09 |
| Stopped Worker has only a log indication | U13 |

## 17. Questions that need a recorded expectation

These are coverage cases, not proposed changes to the design. The requested
test plan can be completed without deciding them now. Before an execution is
claimed comprehensive, record the applicable design interpretation or the
user's decision. Until then, run unaffected cases and mark the ambiguous
assertion `BLOCKED`; existing code alone is not the authority.

| ID | Exact case | Specified part and unresolved part |
|---|---|---|
| Q01 | Answered on November 9, 2026: a manual copy is not a CHA delivery route (design R58). T19 tests it. | — |
| Q02 | Answered on November 9, 2026: a turn with only its prompt does not move, and turn positions skip it (design R59). H04, H10, H19, and H23 test it. | — |
| Q03 | Incoming config produces a local no-turn notice in an open chat. | O10 explicitly leaves whether the receiver creates that local notice unverified. Observe it; do not require cross-machine notice equality. Transmission remains forbidden. This observation does not block other config acceptance. |
| Q04 | Answered on November 9, 2026: `Disconnect from D1` shows whenever D1 credentials are saved (design R60). U01, C07, and T18 test it. | — |

Also record any newly discovered contradiction with its exact design section
and minimal reproducer. Do not silently add an expiry, retry policy, mapping,
or UI recovery feature to close a question. If a clarification changes an
expected outcome, update the affected case and traceability entry before
executing it.

## 18. Run order, regressions, and completion

### 18.1 Suggested one-time run order

1. Freeze the build/revision and fixture manifest. Expand matrices into named
   variants; identify available hosts/resources, the LIVE request and quota
   budget (section 2.2), and unresolved Q cases.
2. Run P01–P06 early gates. A blocked live prerequisite is recorded, not
   replaced with a simulated pass. Continue independent controlled work.
3. Run schema/connection and outgoing note/config/chat tests, then client,
   Worker, piece, malformed-input, and transaction-failure tests.
4. Run lifecycle, UI, delivery, and native shutdown cases, including the
   supported protected/WAL variants.
5. Run the cross-machine LIVE scenarios. Execute destructive capacity and
   collision fixtures in isolated resources/checkpoints.
6. Run relevant existing regression commands on each platform. Reconcile
   every requirement, record kind, matrix cell, and accepted risk with the
   ledger. Retest a fixed failure and the paths affected by its fix; “one-time”
   does not mean keeping a known failure unverified.
7. Inspect evidence for secrets, report results, and remove only this run's
   temporary resources and instrumentation.

### 18.2 Existing regression commands

Commands below are the repository's existing routes at plan-writing time.
Record any changes in the executed build. These supplement the cases; they do
not replace LIVE/HOST evidence.

macOS/shared backend and frontend:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
make web-check
make itest-local
```

Linux:

```sh
make test-daemon
make itest-daemon
```

Windows: use `scripts/build-windows.ps1` through the existing build route,
then its generated CTest build directory and frontend checks. Test the actual
Windows host, not only a cross-compiled binary. Run desktop and ChaWeb
production frontend builds (`npm run build` and `npm run build:chaweb` from
`webapp`) to check both entry points. Run only the dedicated live R2 roundtrip
needed by this plan; do not run unrelated live-provider suites by default.

Useful existing tests can satisfy a case only after the ledger lists the
matching assertions/variants. For crash boundaries or exact timings not
already exposed, use a disposable test addition/runner, not a permanent new
subsystem. Broad load benchmarking, multiple simultaneous users, speculative
platforms, and unrelated provider functionality are outside this feature's
acceptance scope.

### 18.3 Completion criteria

The campaign passes when:

- Every applicable variant has evidence and a verdict; all requirements and
  all 17 record kinds are covered. Impossible combinations, such as pending
  sync rows in a pre-sync schema, have an explicit applicability explanation,
  not a fabricated pass.
- LIVE gates, all supported delivery routes, three-platform receiving,
  supported outgoing paths, approved screens, and both native shutdown paths
  pass. No unavailable host or untested external assumption is concealed by
  shared unit-test success.
- There is no unexplained loss of committed outgoing work, cross-vault write,
  partial transaction, deletion of an unhandled incoming prefix, credential
  overwrite contrary to R56, or delivery replay contrary to R57.
- Accepted duplicates, collisions, timing gaps, and source-note omissions are
  reported separately from defects. They are not “fixed” by an unsupported
  change in expected behavior.
- All blocking expectation questions have a recorded answer for the executed
  scope. Any unresolved case, missing platform, unavailable resource, or
  failing probe makes the result **incomplete**, not a full acceptance pass.

Final report: build/hosts, number of expanded variants by verdict, failed or
blocked IDs with reasons, links to sanitized evidence, external observations,
accepted risks observed, and exact cleanup completed. Do not report a pass
percentage as a substitute for unresolved required cases.

Delete only campaign-owned queue tables, capacity database, R2 objects,
temporary vaults/profiles, and temporary fault/observer code. Stop all test
Workers before cleanup so they cannot recreate tables or send more records.
Retain the sanitized ledger and results. Preserve pre-existing working-tree
changes and all non-test user data.
