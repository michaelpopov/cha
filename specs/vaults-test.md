# ChaWeb vault commands — one-time acceptance test

Run this test once. The acceptance standard is
[vaults-1pager.md](vaults-1pager.md). Run each case once. Repeat a case only
when a setup problem stopped it before it reached the behavior under test.
Record each case in one ledger: steps, expected result, actual result,
evidence, and a verdict of `PASS`, `FAIL`, `BLOCKED`, or `NOT RUN`. A case
without the evidence for its expected result is not a pass.

Use only live services, browsers, existing tools, and disposable data. Do not
add test hooks or fault injection to the code. Section 9 lists the design
points that this run does not cover, and why. Exhaustive D1 record and
protocol tests are in [d1-alt-test.md](d1-alt-test.md).

## 1. Setup

- Build the daemon, the ChaWeb assets, and the desktop application from the
  same source state. Record the revision.
- Use two Linux `cha-daemon` services, A and B, with separate configuration
  directories, sockets, and HTTPS origins. Use the socket activation and the
  packaged nginx configuration from [the Admin Manual](../docs/AdminManual.md)
  and [the nginx example](../packaging/linux/nginx-chaweb.conf.example)
  without changes. Choose a unique run prefix `V`. Set the `machine` in A's
  `app.toml` to `V-a` and in B's to `V-b`.
- Use one disposable R2 bucket and two disposable D1 databases, `D1-main`
  and `D1-second`. Confirm that they are test resources before any write.
- **The desktop is only the publisher.** On one desktop host, create the test
  vaults, save their R2 and D1 keys (`D1-main` unless stated otherwise), seed
  them, and upload them. Use this host for every upload, because R2 upload
  ownership follows the host. Set the desktop `machine` to `V-desk` while you
  save the keys. Then remove `machine` from the desktop `app.toml` and
  restart the desktop. After that, desktop edits do not go to D1, so a marker
  that the desktop uploads exists only in R2. Check each such marker with a
  D1 query: it must not be in any inbox.
- **Starter vaults.** Prepare them on the desktop, close them, and copy each
  database and definition into the daemon's configuration directory.
  Correct the `data` path and register the vault in that `app.toml`. A gets
  `V A Start`, with R2 and D1 keys. B gets `V B Start`, with R2 and D1 keys,
  and `V B Plain`, with no keys.
- **Test vaults and R2 objects.** Record each object basename (the R2 name
  without `.sqlite3`).

  | Vault | Content and purpose |
  | --- | --- |
  | `V Atlas One` | Named chat with a completed reply, a note, a visible configuration value, a cached audio clip. Main live vault. |
  | `V Delta Two` | Different content. Use the same Welcome, first chat, and first note IDs as Atlas where possible, so that a write to the wrong vault is visible. |
  | `V Gamma` | D1 key for `D1-second`. |
  | `V Big` | Large enough that a download to B takes more than 90 seconds, or limit B's download bandwidth. |
  | `V Swap` | Disposable vault for Download failures. |
  | `V Locked` | Password protected. |
  | `V Odd File` | Upload a companion whose `vault_name` is `V Odd Name`. |
  | `V Legacy` | Upload the database only, without a companion. The database has no D1 key. |
  | `V Junk` | Upload bytes that are not SQLite, with a valid companion. |
  | `V Clash` | Upload a companion whose `vault_name` is `V Atlas One`. |

- **Clients.** Two browser clients on A and two on B. A working provider for
  Welcome and for an ordinary chat that can give a reply of about 20
  seconds.
- **Tools.** Browser developer tools, a D1 console or `wrangler`, an R2 tool
  such as `rclone`, `sqlite3` for closed copies, the daemon logs, and a
  firewall rule on each daemon host that can block the R2 host and,
  separately, the D1 API host.

## 2. Evidence

- **Notices.** The warning, start, and result notices are in the Welcome
  transcript of the vault that is active when the job posts them. After a
  reload, they are still there. A start notice can become visible only after
  the operation ends.
- **Context.** The bootstrap `daemon_token` and epoch, the same two values in
  the query string of each vault request, and the daemon PID.
- **Files.** The registry and the selected vault in `app.toml`, the database
  and companion files, the `.bac` files, and no temporary files left after
  the operation.
- **D1.** `SELECT rowid, record FROM "q/<vault>/<machine>" ORDER BY rowid`
  in the correct D1 database, and the table's presence in `sqlite_master`.
  Find records by their content marker.
- **No R2 request.** Block the R2 host. The step must behave the same.
- **No model.** The provider log shows no model request for a command.

## 3. Live workflow

Run L01–L12 in order. Allow two minutes for one D1 delivery, because the D1
workers run every 30 seconds. Find the cause of a missing delivery before
you continue.

| ID | Do | Expected |
| --- | --- | --- |
| L01 | On the desktop, create and seed all test vaults, save their keys, and upload them. Then remove the desktop `machine` (setup). | R2 has the database and companion of each vault, except the special objects. Each snapshot has its D1 key. |
| L02 | In A's Welcome, send `/open V Atlas One`. Watch A's second client. | The input request returns at once. A start notice is in the `V A Start` Welcome. Atlas is registered, selected, and open in the same daemon process, with a new epoch. The result notice in the Atlas Welcome states `V Atlas One`. A's inbox `q/V Atlas One/V-a` exists. The seed content is visible. A's second client goes to the Atlas Welcome on its next request. |
| L03 | In B's Welcome, send `/open V Atlas One`. | Same as L02 for B. A is not affected. |
| L04 | On A, add a completed turn to the shared chat and create a new named chat. Then do the same on B. | Each change arrives at the other daemon. This checks D1 startup after Open. |
| L05 | B: `/open V B Start`. A: make a completed Atlas change X. Check that X is in B's Atlas inbox. Block R2 on B. B: `/open V Atlas One`. Unblock R2. | Atlas opens from the local copy with R2 blocked. X is not deleted. B applies X by normal receipt. The start notice is in the `V B Start` Welcome. |
| L06 | On the desktop, add an R2-only marker S2 to Atlas and upload it. On B, add a local marker B1 in a chat. Start a 20-second reply in another B chat. B: `/download`. Repeat `/download` with R2 blocked. Reload the page. | The warning names Atlas. It says that local changes absent from R2 survive only in `.bac` and that running work stops, and it gives `/download confirm`. The reply finishes. No file changes. The warning is the same with R2 blocked. It stays after the reload. |
| L07 | Start a 20-second reply in another B chat, and a note compression or summary if available. B: `/download confirm`. | A start notice is posted. The drain cancels the reply and the note work (provider and daemon logs). The active Atlas has S2 and not B1. The database `.bac` has B1. The definition `.bac` is the definition from before. PID and token stay the same, and the epoch changes. The result notice is in the Atlas Welcome. No temporary files remain. A has B1. Then one new change in each direction arrives. |
| L08 | On B, add marker B2. Reload Welcome and send `/download confirm` without `/download` first. | The Download runs. There is one `.bac` per file. The new database `.bac` has B2 and not B1. The registry does not change. |
| L09 | A: `/open V Delta Two`. | As L02, for Delta on A. |
| L10 | Before B opens Delta, create `q/V Delta Two/V-b` in `D1-main`, using the schema of an existing inbox, and add a named index on `piece`. On A, make a Delta change M1. Check that M1 is in B's new table. B: `/open V Delta Two`. Then make one new change on A and one on B. | B's table still exists with its index, and M1 is no longer in it. B's Delta does not have M1. A's inboxes and the Atlas inboxes keep their rows. Both new changes arrive. |
| L11 | On the desktop, add an R2-only marker to Delta and upload it. B: `/open V Atlas One`. Block R2 on B, send `/open V Delta Two`, and unblock R2. B: `/download confirm`. | The local Open of Delta needs no R2. The Download brings the new Delta marker. This is the refresh of an inactive vault: Open, then Download. |
| L12 | With Delta active on B, drop B's Delta inbox table. Wait for B's log message that D1 sync stopped. Create the table again with the named index. On A, make a Delta change M2 and check that it is in B's table. On B, make a local change B3. B: `/download confirm`. Then make one new change on A and one on B. | The Download succeeds. B's table and index still exist, and M2 is not in the table or in B's Delta. B3 is in the `.bac` only, and A never gets it. After the Download, both new changes arrive. |

## 4. Commands and names

| ID | Do | Expected |
| --- | --- | --- |
| C01 | In B's Welcome, send `/download yes`, `/download Confirm`, `/download confirm extra`, `/open` without a name, and `/open` followed by spaces only. Then send `/download confirm` with spaces around it and a tab between the words. | The invalid forms start no job and send nothing to the model. The valid form runs the Download. |
| C02 | On B, send `/open v odd file` (wrong case), `/open V Odd File.sqlite3`, and `/open   V Odd File  `. With Atlas inactive, send `/open v atlas one`. | The first two get `not_found`, and the notice says that remote lookup needs the R2 file name without `.sqlite3`, in its exact case. The third opens the vault. The case variant of a registered name opens the local Atlas without R2. |
| C03 | After C02, switch away, then send `/open V Odd Name`. Send `/open V Legacy`. | `V Odd File` was registered and opened as `V Odd Name`, the result notice states that name, and its inbox is `q/V Odd Name/V-b`. `/open V Odd Name` opens the local copy without R2. `V Legacy` takes its name from the object name, gets a local definition, and opens. It has no D1 key, so nothing is deleted in D1 and no D1 worker starts. |
| C04 | On B, send `/open V Clash`. Put an unregistered placeholder file `V Gamma.sqlite3` in A's configuration directory, and on A send `/open V Gamma`. Remove the placeholder. | Both fail before inbox deletion and installation. Atlas and the placeholder do not change. No new vault is registered. |
| C05 | With a reply running in another B chat and R2 blocked, send `/open` with the active vault's name, then with a case variant and outer spaces. | Each reports that the vault is already open. The epoch does not change, there is no drain, and the reply continues. |
| C06 | On B, open `V B Plain`. Send `/download`, `/download confirm`, and `/open V Gamma`. Then send `/open V Atlas One`. | The first three are refused with a reason that names the missing R2 key, with no drain and no transfer. The local Open of Atlas works without an R2 key. |
| C07 | B: `/open V Locked` (remote). Copy the `V Locked` files into A's profile and register them, then on A send `/open V Locked`. If a daemon can run on a protected vault, start A on `V Locked` and send `/open V A Start`, `/open V Gamma`, and `/download`. | Remote and local Open of the protected vault are refused, with no password request and no change. From a protected active vault, the local Open and the remote Open work, and the remote Open uses its R2 key. `/download` of the protected active vault is refused. |
| C08 | Send `/download` while Welcome generates a reply. Send `/download` in an ordinary ChaWeb chat. Send `/Open`, `/DOWNLOAD`, `/downloadx`, and a sentence that contains `/download`. In desktop Welcome, send `/download` and `/open V Atlas One`. | Welcome uses its existing refusal while it generates. The ordinary chat and the other forms use the existing input handling. The desktop refuses the commands. None of them starts a vault operation. |
| C09 | In Welcome, ask in plain words to refresh this vault, to continue after the warning, to open `V Delta Two`, and to refresh Delta while it is inactive. | The model answers with the exact commands: `/download`, `/download confirm`, `/open V Delta Two`, and Open followed by Download. No vault operation starts. |
| C10 | Get the warning in B's client 1. In client 2, send `/open V B Start`. In client 1, without a reload, send `/download confirm`. Then send `/download confirm` from the reset page. | The first confirmation is refused with `vault_changed`, and client 1 resets. No Download runs. The second one downloads `V B Start`. |

## 5. D1 backlog

| ID | Do | Expected |
| --- | --- | --- |
| Q01 | Before B opens Gamma, create `q/V Gamma/V-b` in both `D1-main` and `D1-second`, and put a copy of a real record in each. B: `/open V Gamma`. Then open Gamma on A and send one change each way. | The `D1-second` table is empty and still exists. The `D1-main` table keeps its record. The changes go through `D1-second`. |
| Q02 | On B with Atlas active, drop B's Atlas inbox, wait for the stop message, and do not create the table again. B: `/download confirm`. | The Download succeeds with nothing to delete. After it, B's Atlas inbox exists again and sync works. |
| Q03 | With Delta active on B, drop B's Atlas inbox. B: `/open V Atlas One`. Then, with Atlas active, drop B's Atlas inbox again, wait for the stop message, and send `/open V Atlas One`. Then open `V B Start` and open Atlas again. | The first Open creates B's Atlas inbox before sync. While Atlas stays active after the second drop, sync stays stopped, and Open of the active vault does not repair it. Switching away and back creates the table again, and sync works. |
| Q04 | Block the D1 API host on B. B: `/download confirm` on Atlas. Block it on A, and on A send `/open V Big` (not yet on A). Unblock both, and send each command again. | B: the drain runs, the deletion fails, the database is not replaced, the unchanged Atlas reopens with a new epoch, and the failure notice is in the Atlas Welcome. A: `V Big` is not registered, and A stays on its vault with no drain. After the unblock, both commands work. |

## 6. Failures, blocking, and timing

| ID | Do | Expected |
| --- | --- | --- |
| F01 | With a reply running in another B chat: `/open V Junk`, `/open V Missing`. Then B: `/open V Swap`. With the R2 tool, replace the `V Swap` companion in R2 with one whose `vault_name` differs. B: `/download confirm`. | `V Junk` fails validation, `V Missing` gets `not_found`, nothing is registered, there is no drain, and the reply continues. The `V Swap` Download drains, fails validation, and reopens the unchanged database with a new epoch. Files and `.bac` do not change, and no temporary files remain. |
| F02 | During B's `/open V Big`, block R2. Repeat during `/download confirm` with `V Big` active. | Open: nothing is registered, there is no drain, and no temporary files remain. Download: the unchanged database reopens. |
| F03 | Block the D1 API host on B. On B with Atlas active, make a local change B4. B: `/open V B Start`. Unblock, then `/open V Atlas One`. | The Open succeeds, and the log shows the final-send failure. B4 stays in Atlas's local queue and reaches A after the second Open. |
| F04 | Start a 20-second reply in another B chat, then `/open V Big`. Send requests from B's second client, and from A, during the transfer. Repeat with `/download confirm` while `V Big` is active. | The input request returns at once. B's other requests wait and finish after the operation. A answers normally. For Open, the reply finishes during the transfer, and the switch then stops work. For Download, the reply is cancelled before the transfer. |
| F05 | During each operation in F04, send a second vault command from another client. After the operation, send a new command. | The second command never runs as a second job. It is refused because a vault command runs, or it waits and then gets `vault_changed`. The new command works. |
| F06 | Repeat F04 so that the operation takes more than 75 seconds. Send a chat input from another client during it. | Waiting requests fail with the web server error. The pages recover after the operation without a restart. The chat input is not sent again by the page. It is stored at most once, in the context that accepted it. |
| F07 | Stop B with `systemctl` during `/open V Big`. Start B again. | B stops within its grace time. `V Big` is not registered, no temporary files remain, the old vault is active, and the token is new. |

## 7. Browser context

| ID | Do | Expected |
| --- | --- | --- |
| H01 | In developer tools, record the bootstrap response and the vault requests through nginx: notes, session list and create, snapshot, input, stop, delete, voice, and the direct audio URL. | Bootstrap needs no context and returns the token and epoch. Every vault request has both in its query string. |
| H02 | Replay a read, an input, a delete, and a direct audio URL with: no token, no epoch, a wrong token, a stale epoch, and a malformed epoch. Use IDs that exist in the current vault. | Each request gets `vault_changed`, reads and changes nothing, and the audio URL returns no audio. |
| H03 | Before a local Open and before a same-vault Download, keep B's clients on: Welcome with a draft, a chat with a draft, the session list, a note, cached audio playback, and voice input. | On its next request, each client goes to a fresh Welcome. Drafts and routes are gone. Playback and voice input stop, and late voice text is not sent. No old content appears. A's clients do not change. |
| H04 | Restart B with the same vault. Use an old page and a captured request. Replay A's context on B. | The token changes. The old page gets `vault_changed` and resets. The captured request and A's context get `vault_changed`. |

## 8. Desktop

| ID | Do | Expected |
| --- | --- | --- |
| D01 | On the desktop with `machine` set, put a record in its inbox while receipt is stopped, then use Settings to download a new vault from R2 and to replace the active vault. Switch vaults with the Vault selector. | The behavior is as before. The desktop deletes no inbox records. |

## 9. Finish

Run the existing checks on the build that you tested, from the repository
root. The commands are in [README.md](../README.md).

```sh
ctest --test-dir build/ninja -L daemon --output-on-failure
npm --prefix webapp run check
make itest-daemon
```

**Coverage.**

| 1-pager item | Cases |
| --- | --- |
| Operations and name rules | L02, L05, L08, L11, C01–C05 |
| 1. Native commands, only in ChaWeb Welcome | C01, C08, C09 |
| 2. Confirmation before replacing local data | L06–L08, C06, C10 |
| 3. Application-owned background work | L02, F04, F07 |
| 4. Block the application for the whole operation | F01, F02, F04, F06 |
| 5. Check once; one vault command at a time | C04–C07, F05 |
| 6. Existing drain and maintenance | L07, F03, F04 |
| 7. D1 backlog and recovery startup | L02–L05, L10, L12, Q01–Q04, D01 |
| 8. Failure outcomes and notices | L02, L06, L07, Q04, F01, F02 |
| 9. Browser context | L02, C10, F06, H01–H04 |

**Not covered.** These need a fault inside the code or a second process on
the same profile. Check them in code review or unit tests:

- the refusal in a secondary process (main-process check);
- a remote Open whose switch is refused after the save, so that the vault
  stays saved but not opened;
- a failure after the database was closed, retargeted, or replaced, which
  leaves the application unavailable: the report outside the transcript and
  no D1 worker for an unusable vault;
- a file replacement failure after the backlog deletion;
- a D1 deletion whose remote outcome is uncertain.

Report every failed, blocked, and not-run case with its evidence. The run
passes when every case in sections 3–8 passes.

**Cleanup.** Stop the daemons. Remove the disposable R2 objects, both D1
databases, the test profiles, and the desktop test vaults. Keep the ledger
without keys or other secrets.
