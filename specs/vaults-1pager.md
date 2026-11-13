# Assistant vault Download and Open: one-page design

Date: November 13, 2026.
Status: Short technical design. A detailed technical design follows from it.

## Goal

The user downloads vaults from R2 and opens vaults with commands in the
Entrance / Welcome prompt. This works in the desktop application and in
ChaWeb, without Settings and without a restart.

## Operations

- **`/download`.** Download the active vault from R2, replace the active
  vault's database with it, and reopen the vault.
- **`/open <name>`.** If `<name>` is a registered local vault, open it.
  If not, download `<name>` from R2 as a new local vault, register it, and
  open it. If `<name>` is already the active vault, report that and do
  nothing.

To refresh a vault that is not active, the user does Open and then Download.

## Key decisions

1. **Two safety categories.** (A): Can the step run in the command handler?
   (B): Can the step lose data?

   | Step | A: in the command handler | B: data loss |
   | --- | --- | --- |
   | Download into a new file | Safe | Safe |
   | Open an existing local vault | Not safe | Safe |
   | Replace the active vault's database | Not safe | Not safe |

   A step that changes the active vault must first drain the application.
   The drain asks each live session to shut down and waits until it is
   released. The command handler runs on the Welcome session's thread. If
   that thread ran the change, it would wait for itself.

2. **A confirmation only where data can be lost.** The application cannot
   tell whether the R2 copy is behind the local file or behind other
   machines. This is a known limit.
   - Download: local changes that R2 does not have are deleted. Only the
     `.bac` copy keeps them. The warning names this risk and says that
     running work stops (decision 6). The user accepts it with
     `/download confirm`.
   - Open, with or without a download: no confirmation. No local data is
     lost, and the user cannot prevent the other effects. The command
     itself is the user's request, as with the desktop Vault selector
     today. The effects of a download (an old copy, deleted D1 inbox items)
     are known limits.

3. **Native prompt commands, not model tools.** `/download` and
   `/open <name>` join the existing slash commands in `text_input.cpp`
   (`/mcast`, `/clear`, `/detach`). Native code handles them before any
   model sees the text. ChaWeb and the desktop send them through the
   existing input request, so one native change serves both clients. The
   commands work only in Entrance / Welcome.
   - The handler only parses the command. A syntax error gets the usage
     notice at once, as for `/clear`. A valid command goes to the
     application through one new call with the operation, the name, the
     `confirm` flag, and the epoch. The handler then returns. No code on a
     session thread takes the lifecycle lock or makes a network request,
     because the drain holds that lock while it waits for the sessions.
   - The confirmation keeps no state. Only `/download` has a `confirm`
     form. `/download` without `confirm` only gets the warning and the
     exact command `/download confirm`, which runs the operation. Native
     code keeps nothing between the two. The text after `/open` is always
     the vault name, so a name can contain spaces.
   - The warning, the start notice, errors, and the result are notices in
     the Welcome transcript. The job posts them with the existing
     `post_session_notice`. Only the usage notice for a syntax error is a
     temporary notice from the handler, as for the existing commands.
   - The existing code refuses a command while the session generates a
     reply.
   - There is no model tool, no new UI control, and no new client request.
   - A request in plain words goes to the model as usual. The Assistant
     instructions tell the model to reply with the exact command to type.

4. **The change runs on a separate application thread: `BackgroundJobs`.**
   The new application call copies all data into a command: the operation,
   the name, the `confirm` flag, and the epoch. It launches a
   `BackgroundJobs` job with that command and returns immediately. It never
   waits for the job. The job does all checks, one time (execution line 2).
   - `BackgroundJobs` belongs to the application, not to a vault. The
     drain does not stop it or wait for it, so the job can wait for the
     lifecycle lock. Only shutdown joins it. Upload already uses it.
   - The D1 sync worker cannot run the change. The drain stops and joins
     it, and the change replaces it with a new worker for the new vault.
     Also, it does not exist when the vault has no D1 key.
   - There is no queue or command slot. If two commands run at the same
     time, the one that reaches the drain second fails the epoch check,
     because the first one changed the epoch.

5. **Transfer first.** After its checks, the job downloads the R2 files
   into temporary files and validates them. This runs before the drain and
   without the lifecycle lock, so the application continues to work and
   other requests do not wait. If the transfer or the validation fails, the
   job removes the temporary files, and nothing in the running application
   changes. Only a ready file goes to the next steps. Today one download
   function does the transfer, the validation, and the replacement. It is
   split into these two parts.

6. **"Change the world."** When the new file is ready, or at once for Open
   of a local vault, the job uses the existing drain:
   - It checks the command's epoch. If the context changed after the
     command, the operation fails. Each maintenance operation changes the
     context, not only a vault change.
   - It stops the old D1 worker, so it applies no more incoming items.
   - It shuts down all live sessions and cancels all replies, note
     compressions, summaries, and folder-store work.
   - It tries one final send of the outgoing D1 queue.

   There is no check for running work. The Download warning says that
   running work stops. Open stops it without a warning. If the final D1
   send fails, the operation continues with a warning in the log, as
   Download in Settings does today. For Open, the unsent changes stay in
   the old vault's own file. For Download, they stay only in the `.bac`
   copy.

7. **A database from R2 starts with an empty D1 inbox.** In D1, each
   machine has one inbox table for each vault. Other machines put their
   changes in it, and this machine's D1 worker applies them. Items in the
   inbox can be older than the R2 file, so they must not be applied to it.
   - The job deletes all items in this machine's inbox for that vault
     before any D1 worker for that vault starts. For Download, this happens
     after the drain and before the file replaces the local file. For Open
     of a vault with no local file, it happens after validation and before
     the vault is registered.
   - The job uses the existing `D1Client::delete_through` with the D1 key in
     the downloaded file, because the new D1 worker uses that key. The table
     stays, so other machines continue to send to it. If the file has no D1
     key, or the table does not exist, there is nothing to delete.
   - If the delete fails, the operation fails, and nothing is replaced or
     registered.
   - Open of a local file does not delete the items. They are changes that
     the local copy still needs.

8. **Old clients must not reach the new vault.** Welcome, sessions, and
   notes have the same IDs in all vaults. Each client request carries the
   context epoch that the client received. The application's existing epoch
   check refuses an old epoch with `vault_changed`, and the client then
   reloads its state.

9. **Out of scope:** password-protected target vaults (the commands refuse
   them), merge, upload, and any check of whether R2 is newer. A protected
   active vault can be the source: Open from it to an unprotected vault
   works, and Open with a download can use its R2 key. The existing
   download code refuses a protected downloaded file.

## Main execution lines

### 1. Command, in the Welcome session

1. The user types `/download`, `/download confirm`, or `/open <name>`.
2. Native code parses the command before any model sees it. A syntax
   error gets the temporary usage notice at once.
3. The handler gives the command to the application through the new call
   and returns. The call launches the job. No file has changed yet.

### 2. Checks and vault change, in the job

1. The job checks the request:
   - Under the lifecycle lock, with an epoch check, as `list_r2_vaults`
     does: the name and the target, and protection. For Open of the active
     vault, it reports that and stops. For Download and Open with a
     download, it also checks the main process and copies the active
     vault's R2 key.
   - Without the lock, for Download and Open with a download: for Open, it
     checks that the destination file does not exist. Then it checks that
     the R2 object exists with one HEAD request (`get_r2_database_etag`,
     used only as an existence check). A name that is not local and not in
     R2 gets `not_found`.

   On any failure, the job posts the error and stops. All notices from the
   job are transcript notices in Welcome (decision 3).
2. For `/download` without `confirm`, the job posts the warning and the
   exact command `/download confirm`, and stops.
3. Otherwise, the job posts a short notice that the operation started.
4. For Download, and for Open of a vault with no local file, the job
   downloads the R2 files with the copied R2 key into temporary files and
   validates them. There is no lock and no drain. On failure, it removes
   the files, posts the error, and stops. During the transfer, the only
   visible state is the notice that the operation started. There is no
   progress indicator.
5. The job runs one path:
   - Download: epoch check, drain, final D1 send (a failure only writes a
     warning), empty the D1 inbox, replace with the existing `.bac` backup,
     reopen.
   - Open, local vault: `switch_vault` (epoch check, drain, open).
   - Open, no local vault: empty the D1 inbox, register the vault, then
     `switch_vault`. If the open fails, the downloaded vault stays
     registered.
6. The application publishes the new epoch and starts a D1 worker for the
   resulting vault.
7. The job posts the result notice in the Welcome of the vault that is
   active when it posts the notice:
   - After success: the resulting vault.
   - After a failure before the change: the old vault.
   - After Open saved a downloaded vault but could not open it: the old
     vault, and the notice states both facts.

### 3. Clients after the change

- **Desktop.** The change comes from a job, not from a bridge request.
  Today the desktop context-change handler requires a request ID. It must
  also accept a change without one. It then resets the window and shows
  Welcome.
- **ChaWeb.** Today the adapter gives each application call the daemon's
  current epoch, so the epoch check always passes. Instead, the page gets
  the epoch from bootstrap and sends it with every vault request, including
  the input request and direct audio URLs. The adapter gives that epoch to
  the call. nginx does not forward request headers to the daemon, so the
  epoch goes in the query string. An old page gets `vault_changed` and
  reloads bootstrap.
- **Epoch start value.** Today the epoch starts at 1 in each process, so
  after a daemon restart an old page's epoch can be valid for a different
  vault. Instead, the epoch starts at the clock time in milliseconds when
  the process starts. A new process then starts above all epochs of the
  earlier process. JavaScript numbers hold this value exactly.

## Known limits

- The application cannot tell if the R2 copy is old. For Download, the
  user accepts the risk with `/download confirm`. Open with a download
  asks nothing, and the copy can be older than the copies on other
  machines.
- Download deletes local changes that R2 does not have. Only the single
  `.bac` copy keeps them. This includes changes that the user makes during
  the transfer.
- If the final D1 send fails, the unsent local changes stay only in the
  `.bac` copy.
- A Download or an Open with a download fails if another maintenance
  operation runs during the transfer, for example an upload or a
  configuration import.
- Changes that other machines made after the R2 upload and that waited in
  this machine's D1 inbox are not applied on this machine. The other
  machines still have them.
- If the replacement fails after the D1 inbox delete, the old file stays,
  but the items that waited for it are gone.
- When the drain starts, running work in all sessions, tabs, and devices
  stops. For Open, this happens without a warning.
- The user must type the exact command. The model does not run vault
  operations.
