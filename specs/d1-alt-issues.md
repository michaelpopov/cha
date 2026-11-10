# D1 synchronization acceptance issues

November 10, 2026 · Revision `145511879a1b9d963c7475a9014bbed44fbf8d1b`

All five confirmed defects were fixed and passed retest on the applicable macOS and Ubuntu hosts. Acceptance remains open for the Windows host requirement, the live D1 capacity gate, and four conflicting K04 merge-deletion expectations.

The ledger tracks 182 plan cases. It contains 1,523 PASS, 0 FAIL, 7 OBSERVED, 186 BLOCKED, 0 NOT RUN, and 1 N/A entries. These totals include case rollups and equivalent variants. The U05 aggregate remains blocked by its Windows variants. [Run ledger](../build/d1-alt-qa-14551187/ledger.csv), [verdict totals](../build/d1-alt-qa-14551187/ledger-summary.json), [fix retest evidence](../build/d1-alt-qa-14551187/fix-retest/evidence.md)

## Resolved issues

### D1-QA-06 — Malformed piece numbers stall the receive queue

**High · F08, R41, R53 · Reproduced on live D1 and with controlled transport**

**Fix verified.** The decoder now preserves an identifiable malformed row for the Worker's invalid-piece path. F08 passed on macOS and Ubuntu with controlled transport. Both live D1 variants also passed on macOS.

1. Create a standard disposable queue. Insert three otherwise valid records at rowids 1–3. Give the middle row `piece = 'garbage'` or `piece = 1.5` and `pieces = 1`.
2. Run the Worker with valid credentials and an empty local outgoing queue.

Expected behavior is to warn and skip the identifiable bad row, apply both valid records in order, and acknowledge the handled group. D1 accepted both inserts and returned the bad values with valid rowids. The Worker rejected the entire read, applied neither valid record, and left all three rows in D1. The controlled variants produced the same result and logged `Invalid D1 queue rows.` The unchanged bad row blocks the same read on later passes.

See [live D1 rows before and after](../build/d1-alt-qa-14551187/live-bad-metadata.jsonl), [controlled probes](../build/d1-alt-qa-14551187/chat-faults-retest.jsonl), [D1 row parsing](../src/app/d1_client.cpp#L251).

### D1-QA-07 — Fractional chat metadata is accepted and stored

**Medium · V11, R41 · Controlled Worker**

**Fix verified.** The chat parser now validates timestamps and token counts as integers before storing a turn. V11 passed on macOS and Ubuntu. The malformed turns were skipped and the following note applied.

1. Start with one valid terminal turn. Send another complete `chat.entries` record whose reply has `created_at = 1.5`. In a fresh fixture, use `input_tokens = 4.5` instead. Follow each record with a valid sentinel note.
2. Run one receive pass and inspect the chat, the sentinel, and the acknowledgement.

Expected behavior is to skip the malformed turn as a unit, acknowledge it with a warning, and apply the sentinel. In both variants the chat gained a turn and two entries. The sentinel applied, and the Worker acknowledged the entire prefix. The fractional values went through integer conversion rather than field validation.

See [receive probes](../build/d1-alt-qa-14551187/chat-faults-retest.jsonl), [chat entry parsing](../src/storage/sync_chat.cpp#L38).

### D1-QA-08 — The receiver accepts more rows than its requested limit

**Medium · V12, R8, R40 · Controlled transport**

**Fix verified.** The client now rejects a result larger than its requested limit. Both V12 variants passed on macOS and Ubuntu with no local writes or acknowledgement.

1. Return 51 ascending, individually valid queue rows from a request with `LIMIT 50`.
2. Observe the local writes and the remote prefix delete.

Expected behavior is to reject a response that could not have come from the issued query, with no local apply or acknowledgement. The Worker applied all 51 rows in one group and acknowledged through rowid 51. In a second controlled run, the malformed 51-row response omitted a genuine rowid 4 and ended at rowid 52. The Worker acknowledged through 52, deleting the unreturned row. The previously reported zero, negative, duplicate, and descending rowid variants now reject their responses.

See [malformed result probe](../build/d1-alt-qa-14551187/chat-faults-retest.jsonl), [unreturned-row probe](../build/d1-alt-qa-14551187/oversized-group.jsonl), [D1 row parsing](../src/app/d1_client.cpp#L251).

### D1-QA-09 — An incomplete saved D1 credential prevents a vault from opening

**High · M05, L13, R44 · macOS Application and Ubuntu arm64 daemon**

**Fix verified.** The vault now opens with unusable D1 credentials and keeps the key available for repair. M05 and L13 passed on macOS and Ubuntu. The Ubuntu daemon also preserved local work and pending sync state through restart.

1. Use a disposable vault with ordinary note content. Add a saved `type = "D1"` configuration row under `system/keys/api_key_7/config.toml`, with a display name, account ID, database ID, and an empty `api_token`.
2. Open the vault with a valid `machine` in `app.toml`. Repeat with `machine` absent. Use the same two states when starting the daemon.

The vault should open with sync inactive and allow unrelated local work. On the Mac, the Application aborted opening with `Key config … has invalid api_token`. On Ubuntu, the daemon exited before serving its bootstrap endpoint with the same diagnostic. Both machine-name states failed on each platform. Removing only the synthetic credential row from the disposable Linux fixture let the daemon start.

See [macOS startup probes](../build/d1-alt-qa-14551187/continuation/connection-matrix.jsonl), [Ubuntu startup and recovery](../build/d1-alt-qa-14551187/continuation/linux-incomplete.jsonl), [saved D1 key validation](../src/workspace/workspace.cpp#L624).

### D1-QA-10 — A late Disconnect reply overrides macOS Settings navigation

**Medium · U05 · Native macOS WebKit host**

**Fix verified.** The form now checks that it is still mounted before applying the Disconnect reply. Both U05 navigation variants passed twice in native macOS WebKit. Windows remains blocked.

1. Open D1 sync in a disposable vault with saved credentials. Start Disconnect and confirm it.
2. Hold the successful native reply at the WebKit delivery boundary. Navigate back to the top-level Settings screen, then release that reply.

The active screen should remain Settings. Both fresh profiles showed Settings before the reply was released, then changed to API Keys. The backend had successfully removed the credential in both runs. The delayed operation changed the screen after the user had left the form.

See [first native reproduction](../build/d1-alt-qa-14551187/completion/native-delayed-final.jsonl), [independent confirmation](../build/d1-alt-qa-14551187/completion/native-delayed-confirmed.jsonl), [Disconnect form handler](../webapp/src/components/D1SyncSettings.tsx#L100).

## Latest macOS and Linux run

- The Mac waited 50.77 seconds for seven outgoing pieces. Ubuntu reopened with matching note hashes for the two imported chapters and the large flagged note, and stored the cancelled reply. Failure on the third send left four records queued on the Mac and the two accepted records on Ubuntu. The combined close used a seeded terminal cancellation. The earlier generating native run exercised actual cancellation and sending. [Combined close and handoff](../build/d1-alt-qa-14551187/final-matrix/handoff-final.jsonl), [native cancellation](../build/d1-alt-qa-14551187/completion/native-reservation-retry.jsonl)
- Native reconnect received a temporary `session_stopping` response during a held reservation, displayed the new entry automatically after release, and accepted another message. ChaWeb passed unmount, immediate remount, and visibility recovery after a 503. It resumed the normal idle cadence after the bootstrap refresh. [Native reservation](../build/d1-alt-qa-14551187/final-matrix/native-held-automatic.jsonl), [ChaWeb timing](../build/d1-alt-qa-14551187/final-matrix/browser-final-settled.jsonl)
- The Linux manual-copy variants confirmed clean startup, shared-name queue consumption and dirty replay in both arrival orders. Each dirty replay produced a suffixed note and a duplicate completed turn. A separate ChaWeb edit stayed queued during Ubuntu suspend and reached the peer after resume. [Manual copy and suspend](../build/d1-alt-qa-14551187/final-matrix/manual-synthetic.jsonl), [durable turn counts](../build/d1-alt-qa-14551187/final-matrix/manual-replay-confirmed.jsonl)
- Explicit credential Save and R2 replacement recovered from a controlled full-database eviction. Save preserved newer local content, and R2 supplied only the captured snapshot. The source edit made before recipient registration was absent from the receiver. The later edit arrived. [Eviction and registration](../build/d1-alt-qa-14551187/final-matrix/delivery-retry.jsonl)
- The remaining editor, summary, credential and maintenance results are indexed by case in the run evidence. [Completion evidence](../build/d1-alt-qa-14551187/final-matrix/evidence.md)

## Earlier macOS and Linux continuation

- The plain live handoff delivered all 17 record kinds from ordinary macOS application operations to a second Mac profile and the Ubuntu daemon. A return entry submitted in ChaWeb reached the Mac. The protected handoff also verified multipart note content and empty acknowledged queues on both receivers. [Live handoff](../build/d1-alt-qa-14551187/continuation/live-cross-final.jsonl)
- Live protected R2 replacement removed the old destination content, retained source credentials, and created a new queue when needed. A separate replacement applied and acknowledged a record seeded into the existing destination queue before the vault opened. [Protected R2 replacement](../build/d1-alt-qa-14551187/continuation/live-r2-protected.jsonl)
- All 16 Ubuntu daemon startup variants passed. Dropping either platform's queue halted its Worker. Ubuntu recreated the table on restart. The Mac kept a local edit through ordinary reopen with its table missing and sent it after an explicit Save recreated the table. ChaWeb showed the updated text and name when a remotely edited note reopened. [Startup matrix](../build/d1-alt-qa-14551187/continuation/linux-lifecycle-final.jsonl), [Ubuntu queue loss](../build/d1-alt-qa-14551187/continuation/linux-drop-retry.jsonl), [Mac queue loss](../build/d1-alt-qa-14551187/continuation/desktop-drop.jsonl), [note reopen](../build/d1-alt-qa-14551187/continuation/linux-notes.jsonl)
- The K10 matrix checked 24 import, merge, and receive combinations. A connected Worker's next request used the retained credentials. An unconnected Worker made no request. All 18 variants with source D1 rows logged a warning. [Credential preservation](../build/d1-alt-qa-14551187/continuation/config-next-request.jsonl)
- Controlled checks added evidence for send ordering and limits, note and chat operations, interrupted processes, credential actions, WAL-origin and old-schema delivery, and R2 upload. The Mac WebKit host verified the approved credential-screen errors and disconnect confirmation. [Continuation evidence](../build/d1-alt-qa-14551187/continuation/evidence.md)

## Coverage and prerequisites

- **P02, live database capacity.** Disposable database creation worked. The Cloudflare subscription endpoint returned HTTP 403, so the account plan and safe capacity target could not be verified. The controlled full-database eviction tests provide separate evidence.
- **Windows.** There was no reachable Windows test host. Its packaged UI, receive and origin paths, vault delivery, both native shutdown timings, and the Windows legs of the live handoffs remain blocked.
- **K04, merge deletion.** Four variants ask a merge source to remove one or more destination-only forums. The product manual defines merge as an overlay that keeps destination-only files. Both merge partitions preserved those forums and chats while applying the provider change. All 14 executable import, merge, and receive variants passed. [K04 results](../build/d1-alt-qa-14551187/completion/config-pruning-final.jsonl), [documented merge behavior](../docs/UserManual.html#L992)
- **T19, live credential transfer.** Automatic approval review rejected the proposed live run because the upload and Ubuntu transfer would export credentials stored in the vault. The command did not run. All four required HOST variants ran using synthetic credentials, a loopback D1 and R2 service, and the Ubuntu daemon. Prior live Cloudflare handoffs are separately recorded. [Completion evidence](../build/d1-alt-qa-14551187/final-matrix/evidence.md)
- **Expectation recorded for T18.** R26 takes precedence over the clean-disconnected assumption in design section 12.1. Both name variants refused the upload with the approved message. Disconnect cleared credentials, records, and flags without making a D1 request. After restoring a valid name, the normal loop drained the pending work and upload succeeded. [Credential cases](../build/d1-alt-qa-14551187/credential-cases.jsonl), [restored name](../build/d1-alt-qa-14551187/completion/restore-name.jsonl)
- **Fixture applicability.** A pre-sync schema cannot contain queued records or set note flags. An empty new vault inherits the active configuration, including D1 credentials when present, and starts with no outgoing work. A chat with a missing forum cannot also exist under that forum in a valid local database. These cells are excluded from the executable count.

## Evidence and observations

- Cloudflare accepted nine 1,900,000-byte fan-out variants, with up to three recipients and an encoded HTTP body of 11,400,166 bytes. All returned bytes matched. In three deliberately failing batches, D1 returned HTTP 400 with error code 7500, and none of the inserts persisted. An empty and a populated queue read each reported one row read and zero rows written. The production Worker drained all 28 fixture rows after the live 50 MB limit error. The live logs count observer calls. Worker request totals remain unmeasured. [D1 probe log](../build/d1-alt-qa-14551187/live-d1.jsonl), [Cloudflare D1 limits](https://developers.cloudflare.com/d1/platform/limits/), [Cloudflare D1 errors](https://developers.cloudflare.com/d1/observability/debug-d1/)
- A native macOS send flushed multipart note data on quit, with the longest measured successful pass taking 18.5 seconds. Ubuntu `cha-daemon` applied the Mac's changes and produced a return record through ChaWeb. The Mac received that record on reopening. Chrome connected to the Ubuntu daemon through a loopback SSH tunnel. An idle session's next read arrived after 30,011 ms. [Handoff log](../build/d1-alt-qa-14551187/live-handoff.jsonl), [ChaWeb screenshot](../build/d1-alt-qa-14551187/chaweb-handoff.png)
- The initial Mac run displayed a live incoming entry in both an idle and a generating chat. Both chats accepted a new message after reconnecting. The generating chat saved the partial reply as cancelled and sent exactly one cancellation record. The latest run also used an explicit reservation barrier. [Idle native probe](../build/d1-alt-qa-14551187/live-native-retry.jsonl), [generating native probe](../build/d1-alt-qa-14551187/live-native-generating.jsonl), [barrier retest](../build/d1-alt-qa-14551187/completion/native-reservation-retry.jsonl)
- All 42 executed standalone-main delivery variants passed, including protected replacement and protected local copy. New delivery cleared queue and flags, retained source credentials when present, and preserved the synthetic note content on copy and R2 routes. R2 replacement registered after the installed database was clean. The continuation adds 54 WAL-origin and old-schema variants. [Standalone delivery matrix](../build/d1-alt-qa-14551187/delivery-matrix.jsonl), [WAL and older-schema delivery](../build/d1-alt-qa-14551187/continuation/wal-delivery.jsonl)
- The two deliberate acknowledgement-failure variants reproduced the accepted replay behavior. Lost acknowledgement before a delete replayed the change; lost response after a committed delete left nothing to replay. Compatible sender interleaving produced one writer's record; incompatible interleaving skipped the affected sets. Both runs still applied the sentinel. [Replay probes](../build/d1-alt-qa-14551187/controlled.jsonl), [Interleaving probes](../build/d1-alt-qa-14551187/ordering.jsonl)
- The Q03 incoming configuration probe applied the provider change to an open idle chat without adding a local no-turn notice or sending a record. This was an observation on the tested route. [Q03 probe](../build/d1-alt-qa-14551187/notice.jsonl)
- Retests of the earlier findings passed for invalid rowid order and range, prompt-only cancellation, fractional delete positions, invalid character settings, and oversized incoming files. The five earlier findings are closed for the retested variants on this revision. [Rowid and retry probes](../build/d1-alt-qa-14551187/retry-faults.jsonl), [Chat retests](../build/d1-alt-qa-14551187/chat-faults-retest.jsonl)

## Regression results

| Suite | Result |
|---|---|
| macOS CTest | 1,787 passed, 2 optional live-provider tests skipped. Two fixture setup errors in the first parallel run passed on serial retest and in the isolated full rerun. |
| macOS local integration | 24 passed. |
| macOS native WebKit UI | Passed at 1200 px and 430 px. |
| Web checks and production builds | 1,718 tests passed. Desktop and ChaWeb production builds succeeded. |
| Ubuntu arm64 daemon CTest | 1,587 passed, 2 optional live-provider tests skipped. |
| Ubuntu daemon/ChaWeb HTTP integration | 15 passed. |
| Dedicated live R2 tests | Round-trip and stale ETag protection passed using unique objects. |
| Latest focused macOS CTest | 162 passed. |

The campaign used macOS 26.7.1 arm64 and Ubuntu 25.10 arm64. The native QA host used the packaged runtime and web assets with the repository's instrumentation build flag to select a disposable profile and drive the UI. The [evidence index](../build/d1-alt-qa-14551187/evidence.md) records fixtures, command outcomes, and cleanup. The [temporary runner diff](../build/d1-alt-qa-14551187/qa-runner.diff) records the probes.
