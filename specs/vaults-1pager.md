# Assistant vault Download and Open: one-page design

Date: November 13, 2026.
Status: High-level technical design. A detailed technical design follows from it.

## Goal

The user downloads vaults from R2 and opens vaults with commands in the
Entrance / Welcome prompt. This works in ChaWeb, without Settings or a
restart in normal operation. The desktop application does not offer these
commands. Existing maintenance failures can still require a restart.

## Operations

- **`/download`.** Perform local eligibility checks, then show the warning
  and the exact command `/download confirm`. Confirmation replaces the active vault's database
  with the R2 copy and reopens it, keeping the existing single `.bac` backup.
- **`/open <name>`.** Open a registered local vault. If none matches,
  download it from R2 as a new local vault, register it, and open it.
  Opening the active vault reports that it is already open and does nothing.

To refresh an inactive vault, the user does Open and then Download.
The text after `/open` is the name, including spaces. Local lookup uses the
existing vault-name comparison. If no local vault matches, the text identifies
the R2 object `<name>.sqlite3` in its exact case. As on desktop, the downloaded
vault takes its name from the companion definition, or from the object name
if no companion exists. Registration and opening use that resolved vault
name, and the result notice states it. A collision with a registered vault
name or local destination fails before inbox deletion or file installation.
A missing R2 database is `not_found`; the notice explains that remote lookup
requires the R2 file name without `.sqlite3`, in its exact case.

## Key decisions

1. **Native commands, only in ChaWeb Welcome.** Native code handles the
   commands before any model sees them, through the existing input request.
   The application checks the existing ChaWeb host flag; desktop commands
   are refused. Plain-language requests still go to the model, whose
   instructions tell it to reply with the exact command to type. The
   existing refusal of commands while Welcome generates a reply remains.
   There is no model tool, new command control, or new request type.

2. **Confirmation before replacing existing local data.** Download warns
   that local changes absent from R2 will survive only in `.bac`, and that
   running work will stop. The
   confirmation is stateless: `/download confirm` authorizes Download for
   the request's active context. Before showing the warning, the job checks
   the context, main-process status, protection, and presence of an R2 key
   using local data. Showing the warning makes no R2 request. Confirmation
   repeats these checks; the warning reserves nothing.
   Open requires no confirmation: it does not replace existing local data.
   Open with a download can still discard D1 inbox data and obtain an old
   snapshot; those are accepted limits, not a guarantee against data loss.

3. **Application-owned background work.** The command handler parses the
   request, hands it and its context to an application `BackgroundJobs`
   job, and returns. It never waits for the job, takes the lifecycle lock,
   or performs network work. A session thread cannot drain itself.
   These jobs survive a vault drain and are joined at application shutdown.
   The D1 worker cannot own the operation because the drain stops it.
   There is no command queue or persistent confirmation state.

4. **Block the application for the whole operation.** The job takes the
   lifecycle lock before its checks and holds it through the transfer and
   the side effects. The only release is the one in remote Open (decision
   5). Download drains first and then transfers within the same
   maintenance operation, as Download in Settings does today. Remote Open
   transfers under the lock without a drain, as the desktop R2 download
   does today; its drain happens later, in the vault switch. While the
   lock is held, every other application request waits, so ChaWeb does not
   answer until the operation ends. The job transfers the database and
   companion definition into temporary files and validates them before it
   replaces, installs, or registers anything. Transfer or validation
   failure removes the temporary files and leaves the vault's files
   unchanged. The download itself detects missing objects; no preliminary
   HEAD request is needed.

5. **Check once, under the lock that covers the side effects.** The checks
   resolve the target, reject protected targets, and, for downloads,
   require the main process and the active vault's R2 credentials. They
   also cover the request's context and registry and destination
   collisions. Nothing can change between the checks and the side effects,
   so there is no second check. Download keeps its checks, drain, transfer,
   backlog deletion, replacement, and reopen within one maintenance
   operation. Remote Open saves in one locked step: check, transfer,
   validate, delete the backlog, install, and register. It then releases
   the lock and calls the existing vault switch with the original request
   context. The switch takes its own lock and rechecks that context before
   draining. If the context has changed between saving and switching, the
   vault stays saved and registered, but this command does not open it.

   Only one vault command runs at a time. The application sets a flag
   when it launches the job and clears it when the job ends, on every exit
   path, as Upload does with its flag today. While the flag is set, the
   application refuses another vault command at once; no job waits behind
   another. So no two transfers run at the same time, and no other vault
   command runs between the save and the switch of a remote Open. A
   destination or name collision still prevents a later command from
   overwriting an already saved vault.

6. **Reuse the existing drain and maintenance behavior.** A switch or
   replacement stops the old D1 worker, shuts down live sessions, cancels
   replies and note work, and attempts a final outgoing D1 send. There is
   no separate running-work check. A final-send failure is logged and does
   not prevent the operation. For Open, unsent changes remain in the old
   vault; for Download, they survive only in `.bac`. Maintenance changes
   the context even when the vault name stays the same.

7. **Discard the queued D1 backlog when installing an R2 snapshot.** The
   job deletes the backlog present at the deletion boundary in this
   machine's inbox for the downloaded vault, using the downloaded
   database's D1 key. It keeps the table. No key or no table means there
   is nothing to delete. Download does this after the drain and the
   transfer, and before replacement; Open with a download does it within
   the locked save step, after validation and before installation and
   registration. No worker for the downloaded
   vault starts before deletion succeeds. A deletion failure prevents
   installation and registration. Opening an existing local vault does
   not clear its inbox.

   When Open actually switches vaults in ChaWeb, whether local or downloaded,
   D1 startup uses the existing recovery mode that creates a missing inbox
   table before synchronization, as daemon startup does today. Download
   retains that recovery behavior too. This extends the daemon startup rule
   to vault switches; it adds no automatic recovery while a vault remains
   open. Opening the already active vault remains a no-op. Existing D1
   startup failure handling and desktop behavior are unchanged.

   Clearing the backlog deliberately discards old queued records and changes newer
   than the R2 snapshot. It is not a synchronization cutoff: other
   machines can enqueue records immediately afterward, including old
   changes they had not yet sent. The inbox need not be empty when the
   new worker starts, and later records are not guaranteed to be newer
   than the snapshot.

8. **Reuse existing failure outcomes; do not add rollback machinery.**
   A failed check changes nothing. A failed transfer or validation leaves
   the vault's files unchanged: for remote Open, the application continues
   as before; for Download, the drain has already stopped running work and
   changed the context, and the existing maintenance recovery reopens the
   unchanged database. After that point, use the existing maintenance
   recovery and file replacement behavior. If a downloaded vault has been
   registered but cannot be opened, it stays registered. A failure after
   closing, retargeting, or replacing a
   database can leave the application unavailable and require a daemon
   restart; returning to the old vault is not guaranteed.

   Warnings, start notices, and results use the Welcome transcript of the
   context active when the notice is posted, identifying the operation
   and target. A partial success reports that the vault was saved but not
   opened. If the application is unavailable, ChaWeb reports the existing
   unavailable/restart-required failure outside the transcript. Reporting
   such a failure must not depend on opening Welcome.

9. **Browser context includes daemon identity and epoch.** Bootstrap gives
   the page a random token identifying this daemon instance and the
   existing context epoch. Every vault request, including direct audio
   URLs, carries both in the query string. The adapter rejects a different
   daemon token and passes the supplied epoch to the application's
   existing check. Missing or stale context is refused with `vault_changed`;
   the adapter never substitutes the current context. Restart creates a
   new token; no clock-based or persistent global counter is needed.

   On a context change, the browser resets to Welcome with fresh bootstrap
   state. It discards old vault state, session/note routes, unsent drafts,
   and pending actions, stops playback and voice input, and ignores late
   responses from the old context. It never automatically replays a
   mutation. Updating the epoch while retaining old page state is not
   sufficient. These rules also apply to Download of the same vault.

## Execution summary

1. Parse and dispatch the command. The application refuses it while
   another vault command runs. An unconfirmed Download performs local
   eligibility checks, shows its warning, and stops. An Open of the active
   vault reports that and stops. Other operations post a start notice.
2. Take the lifecycle lock, check once, and do the operation while the
   application is blocked. There is no progress indicator.
   - Download: check, drain, transfer and validate temporary files, clear
     the D1 backlog, replace with `.bac` backup, and reopen within one
     maintenance operation.
   - Open, local vault: use the existing vault switch's checks and drain.
   - Open, remote vault: check, transfer and validate temporary files,
     clear the D1 backlog, install and register under the lock. Release
     it, then use the existing vault switch with the resolved name and
     original request context. A rejected switch leaves the downloaded
     vault saved but not opened by this command.
3. Complete or recover using existing maintenance behavior. Publish the
   resulting context and availability, and start a D1 worker only for a
   usable resulting vault. Successful Open and Download use the D1 recovery
   startup described in decision 7. Report success, partial success, or failure.
   Browsers reset when their context becomes stale.

## Scope and accepted limits

- Password-protected targets, merge, upload, and freshness checks are out
  of scope. A protected active vault may be the source of Open and supply
  the R2 key for downloading an unprotected target.
- Desktop behavior is unchanged, including its current D1 inbox handling
  after Download. Shared transfer code must preserve that behavior.
- R2 can be older than local or other machines' data. Download preserves
  replaced local data only in the single `.bac` copy. Open with a download
  accepts an old snapshot without confirmation.
- Deleted D1 backlog is not restored if a later step fails. A D1
  error can also leave the deletion's remote outcome uncertain. This
  design adds no cross-system rollback or snapshot/sync ordering protocol.
- During Download and Open with a download, ChaWeb does not answer on any
  client until the operation ends. A request that waits longer than the
  web server's time limit fails in the page; the page recovers after the
  operation. The start notice can become visible only then.
- Download drains before it transfers. A failed transfer still stops
  running work and resets browsers, although the vault is unchanged.
- The drain stops running work on all sessions and clients attached to
  this daemon, not on independent CHA applications. Open does this without
  a warning. Browser context resets discard unsent drafts.
- Some maintenance failures require a daemon restart. A Welcome transcript
  result is available only while the application remains usable.
