# ChaWeb vault and D1 acceptance test plan

Verify that ChaWeb can open an R2 snapshot, open an existing local vault, and
replace the active vault after confirmation. For each path, check what happens
to the receiving machine's D1 inbox and whether synchronization resumes. Use
the [vault design](vaults-1pager.md) as the acceptance standard. The previous
run tested revision `5e681581`. Use the changes after that revision and the
[implementation handoff](../vaults-handoff.md) to identify the changes from the
previous stage. Run the five cases below in order.

## Time budget

| Work | Minutes |
| --- | ---: |
| Verify build and access, deploy, and seed fixtures | 45 |
| V01 · Open from R2 and clear the target inbox | 25 |
| V02 · Open locally and receive a queued change | 15 |
| V03 · Confirm Download, replace the snapshot, and resume sync | 30 |
| V04 · Recover a missing inbox | 15 |
| V05 · Refuse installation when D1 clearing fails | 20 |
| Investigate failures and retry broken setup | 40 |
| Record results and clean up | 20 |
| **Total** | **210** |

Start the clock at preflight. Reuse build artifacts and QA tooling after
verifying the artifacts match the tested revision. Move on when a case has
enough evidence for a verdict. Use the investigation time for a missing D1
delivery or an invalid fixture. At 190 minutes, start reporting and cleanup.
Record any unfinished checks and the reason.

## Setup and evidence

- Use two `cha-daemon` instances on the Ubuntu VM with separate profiles,
  sockets, and HTTPS origins. Run ChaWeb through the packaged nginx setup.
  Assign distinct machine names `V-a` and `V-b`, where `V` is a fresh run
  prefix. Start each daemon on a local starter vault with R2 access and a
  D1 key for `D1-main`.
- Use the desktop publisher to upload `V Atlas One` and `V Reserve`, each
  with a valid companion and a key for `D1-vault`. Use a disposable D1
  database for each alias and a designated R2 test bucket. Keep R2 writes
  within the run prefix. Close the publisher after the uploads.
- Give Atlas a shared chat with a completed turn and a visible seed marker.
  Give Reserve a distinct seed marker. Confirm the first snapshots exist in
  R2 and that neither target is registered on either daemon. Use a working
  text provider for the short synthetic chat turns. Put a unique marker in
  each human message and confirm the turn completed before waiting for sync.
- Record the revision, fixture names, and initial daemon PID and bootstrap
  context. For switches and replacement, capture the
  Welcome result, selected vault, PID and epoch before and after, and whether
  the daemon token stayed the same. Compare vault registration and file state
  where the case calls for it. Keep credentials out of evidence and reports.
- Check D1 by querying `sqlite_master` and running
  `SELECT rowid, piece, pieces, record FROM "q/<vault>/<machine>" ORDER BY rowid`
  in the intended database. Match records by content marker and verify
  applied changes in the receiving vault's transcript. Give each completed
  turn up to two minutes to arrive. If it misses the window, record both
  active vaults, the sender's local queue, the receiver's D1 inbox, and
  relevant worker logs before another switch or Download changes the state.
- Run one state-changing case at a time. Keep both daemons on the vault under
  test during delivery checks unless the case explicitly requires a switch.
  Restore any temporary network block before moving to the next case.

## V01 · Open from R2 and clear the target inbox

1. In A's Welcome, send `/open V Atlas One`. Verify prompt acknowledgement,
   a successful result, the visible Atlas seed, first local registration and
   selection, and creation of A's inbox in `D1-vault`. The PID and token
   stay the same, and the epoch advances. The start notice is in the starter
   vault's Welcome, the result notice is in the Atlas Welcome, and neither
   notice is in the other vault's Welcome.
2. While B remains on its starter vault, create B's Atlas inbox and an
   inactive Atlas observer inbox in `D1-vault` using the normal queue schema.
   Add a named index on `piece` to B's inbox. On A, make a small change
   `remote-old` and capture its real single-piece record in both inboxes.
   Copy it to an identically named B inbox in `D1-main`. Record the marker
   and row ID in each inbox.
3. In B's Welcome, send `/open V Atlas One`. Verify the same installation
   and context changes as on A. B's `D1-vault` inbox and index remain, and
   `remote-old` is absent from that inbox and from B's Atlas transcript.
   The observer inbox and the `D1-main` copy both retain the original record.
4. Make a fresh Atlas change from A and verify it on B, then make a fresh
   change from B and verify it on A.

## V02 · Open locally and receive a queued change

1. Open B's starter vault. On A, make a new Atlas change `local-pending`
   and confirm it is queued in B's Atlas inbox in `D1-vault`.
2. On B, send `/open V Atlas One`. Verify B opens the registered copy in the
   same process with a new epoch, and that `local-pending` appears in B's
   Atlas transcript. A received-and-applied marker is the evidence here,
   since D1 normally acknowledges received rows by deleting them. A download
   would delete `local-pending`, so its arrival also shows that the Open used
   the local copy.

## V03 · Confirm Download, replace the snapshot, and resume sync

1. Drop B's Atlas inbox and wait for a new `D1 sync stopped` log entry on B.
   Recreate the same inbox and its named index. Make `replace-incoming` on A
   and confirm it is in B's inbox. Make `replace-local` on B and confirm it
   exists in B's local Atlas and has stayed out of A's inbox and transcript.
2. In B's Welcome, send `/download`. Verify the warning names Atlas,
   describes the `.bac` backup and stopped work, and gives `/download confirm`.
   Verify that the epoch, vault files, backup inventory and queued marker
   are unchanged. Send `/download confirm`.
3. Verify B reopens Atlas at a new epoch in the same daemon. The active copy
   matches the first snapshot: it has the Atlas seed marker and none of the
   markers made later in this run, including `replace-incoming` and
   `replace-local`. The database backup contains `replace-local`, and the
   definition backup matches the previous definition. The registry is
   unchanged and temporary transfer files are gone. B's inbox and index
   survive, and the queued `replace-incoming` record is gone.
4. Verify one new Atlas change in each direction. Confirm `replace-local`
   is still absent from A after those deliveries.

## V04 · Recover a missing inbox

With B on Atlas, drop B's Atlas inbox and wait for a new sync-stop log. Send
`/open V Atlas One`. Verify the already-open result, unchanged epoch, and
table still absent. Send `/download confirm` and verify the replacement
reopens Atlas, recreates B's inbox, and receives a fresh change from A.

## V05 · Refuse installation when D1 clearing fails

1. With B on Atlas, record B's registry, context, active database identity,
   visible markers, and backups. Block the D1 API for B while keeping R2
   reachable. Send `/open V Reserve`. Verify the result reports an inbox
   clearing failure, Atlas remains active at the same epoch, and Reserve
   has no registration, installed files, or temporary transfer files.
2. With the block still in place, send `/download confirm` on Atlas.
   Verify the same clearing failure, Atlas reopened at a new epoch in the
   same process, and the prior data and backup files were preserved. Check
   that the failed Download leaves no temporary transfer files.
3. Restore D1 access. Retry `/open V Reserve` and verify successful
   registration, the Reserve seed, and B's Reserve inbox in `D1-vault`.

## Report and cleanup

Add the new run to [the QA report](vaults-report.md) with a verdict for each
case. Use `PASS` for verified expectations, `FAIL` for observed product
deviations, `BLOCKED` for missing prerequisites or invalid evidence, and
`NOT RUN` for cases not started by the cutoff. For failures, include the
expected and actual behavior, the shortest reproduction, and evidence. Keep
VAULT-QA-01 and VAULT-QA-02 in the report. Update VAULT-QA-01 with the result
of the V01 notice check. VAULT-QA-02 stays open, because this plan does not
run F07.
Stop the test daemons and remove the run's R2 objects, disposable D1
databases, and test profiles. Preserve the report and its evidence.
