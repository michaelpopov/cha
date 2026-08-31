# Session management design

Status: Implemented in part; storage sections superseded.

## Supersession notice

The authoritative [unified database design](design.md) supersedes this
document's storage layout, per-session/workspace database lease ownership,
physical-deletion database lifecycle, and database path ownership or
derivation. Current CHA stores every session and its archived state as rows in
the one database selected by `--data`; it does not move per-session database
files into `deleted/`. The storage-specific material below records the design's
history and must not be used as current operating or implementation guidance.

Unless separately changed, the following parts remain applicable:

- session rename and delete UI behavior;
- session-label rules; and
- live-session coordination.

For detailed current storage, lease, migration, and configuration ownership,
use `docs/design.md`. `docs/plan.md` and the block files are implementation
history and job definitions, not competing designs.

## Purpose

This document defines session renaming and recoverable deletion. It extends the
existing named-session workflow without changing session identity.

The design has two user-facing outcomes:

- A stored session can be renamed without changing its stable ID or URL.
- Deleting a stored session removes it from CHA but retains its database in a
  per-forum `deleted/` directory.

## Terms

- **Session ID** is the stable URL-safe identifier used in routes and database
  filenames.
- **Session label** is the user-facing name stored in the database.
- **Stored session** is a database visible to the ordinary session catalog.
- **Live session** is a stored session currently owned by a `LiveSession` actor
  and protected by its `SessionLease`.
- **Deleted session** is a database moved out of the ordinary catalog into the
  forum's session deletion directory. It is retained on disk but cannot be
  listed or opened through the normal API.

## Scope

This change includes:

- Rename and Delete actions on Recent sidebar rows.
- A keyboard- and touch-accessible action button in addition to right-click.
- Rename and delete APIs for stored sessions.
- One session-label policy, newly applied to session creation as well.
- Actor-aware mutation of a live session's label.
- Coordinated shutdown before deleting a live session.
- Recoverable database relocation instead of file removal.
- Browser, API, storage, actor, and end-to-end tests for these behaviors.

This change does not include:

- Permanent database erasure.
- Listing, opening, or restoring deleted sessions in the browser.
- Bulk rename or delete.

## Storage layout

Session storage remains owned by `SessionCatalog`. Deleted databases live below
the existing sessions directory so the session layer does not need to learn the
workspace's forum layout.

```text
forums/<forum>/
  sessions/
    <session-id>.sqlite3
    <session-id>.sqlite3.cha-lock
    deleted/
      <session-id>.sqlite3
```

The ordinary catalog enumerates only regular `.sqlite3` files directly inside
`sessions/`. It does not recurse into `deleted/`, so a successful move removes
the session from forum listings and Recent without a separate tombstone index.
Direct session lookup continues to derive only
`sessions/<session-id>.sqlite3`; therefore a deleted session returns not-found.

The deleted destination preserves the original session ID and filename. A move
must never overwrite an existing destination. A destination collision is a
conflict and leaves the active database untouched.

Session IDs are derived from local time to the second, and session creation
currently skips a candidate stem only when it is occupied inside `sessions/`.
Once `deleted/` exists, that is no longer sufficient: deleting a session and
creating another within the same second would mint an ID whose deleted
destination is already taken, and that session could then never be deleted,
because this design offers neither permanent erasure nor restoration through
the browser. Session creation therefore also skips a stem occupied in
`deleted/`. With that, a destination collision means external interference
rather than ordinary use, and reporting a conflict is the right answer.

The `.cha-lock` file is not session content and is not moved. A stale companion
file is already valid in the lease design and must not be unlinked: removing it
could race with another process that has acquired the same companion inode. A
deleted session therefore leaves its companion behind in `sessions/`. That is
harmless: it is empty, the catalog does not enumerate it, and its stem is no
longer reachable by creation.

After a clean live-session shutdown, SQLite should have no transactional
sidecars. CHA never enables WAL, so `<database>-journal` is the only sidecar
its own databases can produce, and only after a writer died mid-transaction. If
one exists, the deletion operation moves it to `deleted/` under the
corresponding deleted database name, preserving the material needed to inspect
or restore that database. `<database>-wal` and `<database>-shm` are moved the
same way if a foreign tool left them behind, but nothing in CHA creates them.

## Session-label rules

There is no shared label policy today. `parse_create_session_label()` accepts
any JSON string, and the only server-side constraint on a stored label is the
database's `CHECK (label <> '')`. This change introduces one policy and applies
it to both operations, so rename and creation stop diverging.

Add `validate_session_label()` and apply it to the create and rename request
bodies. A submitted label is rejected when it:

- is not valid UTF-8;
- contains control characters or line breaks;
- begins or ends with Unicode whitespace, rather than being silently stored as
  something other than what was submitted;
- exceeds 200 Unicode code points.

Labels do not have to be unique, within or across forums. The browser trims
surrounding whitespace before submission, so ordinary typing never reaches
these rejections.

The empty label keeps its current meaning on creation only: `POST` continues to
substitute the generated session ID, which the OpenAPI document already
describes and clients already rely on. `PATCH` has no equivalent fallback and
rejects an empty label as `bad_request`.

Because this tightens creation, the `createSession` description gains the new
`400` conditions. That is the intended outcome — the alternative is a rename
route that refuses labels the create route accepts.

Validation applies to submitted labels only. A label stored before this change
may violate the new rules; it is never revalidated on read, it keeps listing
and displaying as it does now, and renaming replaces it. Rename must not fail
because of the label it is replacing.

The stable session ID is never renamed. Existing deep links and history entries
therefore remain valid after a label change.

## Storage operations

`rename()` and `move_to_deleted()` are `const` members, like every other
repository operation. The repository caches nothing, holds only its immutable
forum map, and takes exclusion from session leases; routes hold it as
`shared_ptr<const SessionRepository>`, so a non-const operation would not be
callable there.

### Rename

Add a low-level database operation that updates the singleton session metadata
row in one transaction:

```sql
UPDATE session SET label = ?1 WHERE singleton = 1;
```

The operation validates the database application ID, schema version, and
embedded identity before changing the label. It fails if exactly one metadata
row was not updated.

For a session without a process-local live actor, `SessionRepository::rename()`
performs the following work:

1. Derive the safe database path through `SessionCatalog`.
2. Settle absence before creating or acquiring a companion lock.
3. Acquire `SessionLease` without waiting.
4. Validate identity and update the label transactionally.
5. Return a fresh `StoredSession` observation.

The database modification updates its file modification time, so a renamed
session moves to the front of Recent under the existing `updated_at` policy.
This is accepted rather than incidental: Recent and the Sessions screen order
by that timestamp, a rename is a deliberate touch of the session, and no
separate label-change timestamp is introduced to hide it.

### Recoverable deletion

Add `SessionRepository::move_to_deleted()`. Once live-session coordination has
reserved the identity, the repository:

1. Derives and validates the source and destination paths.
2. Creates `sessions/deleted/` if necessary.
3. Rejects an existing destination without overwriting it.
4. Settles source absence before acquiring the lease.
5. Acquires the source session's lease without waiting.
6. Renames the database into `deleted/`.
7. Moves any sidecar left beside the source under the destination name.

The database rename is the catalog-removal commit point, and it happens before
any sidecar move. The order matters because the two arrangements fail
differently. Database first leaves, at worst, an orphaned
`<session-id>.sqlite3-journal` in `sessions/`, which the catalog already
ignores because it enumerates only regular `.sqlite3` files and whose stem
creation no longer reuses. Sidecar first would leave a catalog-visible database
whose hot rollback journal had been moved away — openable, and silently
corrupt. The larger loss decides the order.

The deleted directory is below the same session storage root, so normal
deployments perform a same-filesystem rename. The database must not be
implemented as copy followed by removal.

Step 3 alone cannot satisfy "never overwrite": `std::filesystem::rename`
replaces an existing destination, so a destination appearing between the check
and the move would be clobbered. The move itself must refuse to replace —
`renameat2` with `RENAME_NOREPLACE` on Linux, `MoveFileExW` without
`MOVEFILE_REPLACE_EXISTING` on Windows — with the existence check kept as the
ordinary path that produces the documented conflict error. Holding the lease
makes this window reachable only by something writing into `deleted/` behind
CHA's back, which is precisely the case the guarantee is for.

Deletion intentionally does not require valid database metadata. A catalog row
for a corrupt or mismatched database must remain removable. Safe path
derivation, source existence, destination nonexistence, and the lease are the
required protections.

## Live-session coordination

Repository leases serialize storage access across processes, but they do not
by themselves coordinate a mutation with the process-local actor map. The live
manager remains the liveness authority.

### Live rename

Renaming a live session must execute on its owner thread. Add a
`RenameSessionCommand` to the existing bounded owner queue. The actor handles it
in this order:

1. Ask `SessionController`/`SessionJournal` to persist the new label.
2. Replace `descriptor_.session_label` only after persistence succeeds.
3. Publish a full session snapshot.
4. Complete the command reply with the effective ID and label.

This ensures the database, current chat snapshot, sidebar, and any other SSE
consumer observe the same label. A separate SQLite connection must not update a
live session behind its actor because that would leave the actor's copied
descriptor stale. Persistence goes through the journal's existing connection
rather than a second one, since the journal already holds the open database for
the lifetime of the actor.

`CommandSubmitResult` is a closed variant of `CommandResult`,
`SessionSnapshot`, `SseConnectResult`, and `ErrorCode`, none of which can carry
a rename outcome. Add `SessionLabelResult { SessionId id; std::string label; }`
as a fifth alternative and handle it at the route exactly as `SessionSnapshot`
is handled today.

If no process-local live actor exists, the route calls the repository rename
operation. A concurrent open in this or another process may win the lease; in
that case rename returns `session_busy` and can be retried.

A live rename crosses the bounded owner queue, so it inherits that queue's
failures alongside the storage ones: a full queue, an expired reply deadline,
an actor that stopped between the manager lookup and the submission, and
process shutdown. The HTTP table below lists all of them. An expired reply
deadline is not a rollback — the owner thread may persist the label after the
waiter has given up — which is why the failure rules below treat it separately.

### Live deletion

Deleting a live session requires an exclusive maintenance reservation in
`LiveSessionManager`. The reservation prevents a new actor for the identity
from being opened after the old actor releases its lease but before the
database move commits.

```mermaid
sequenceDiagram
    participant UI as Browser
    participant Route as Lobby route
    participant Manager as LiveSessionManager
    participant Actor as LiveSession
    participant Repo as SessionRepository

    UI->>Route: DELETE stored session
    Route->>Manager: reserve identity for deletion
    Note over Manager: Reservation is taken first.<br/>From here open and reattach refuse this identity.
    Manager->>Actor: request shutdown(session_deleted)
    Actor->>Actor: stop generation, publish final, tear down
    Actor-->>Manager: Finished; lease released
    Manager->>Manager: sweep and join the finished actor
    Manager-->>Route: reservation guard
    Route->>Repo: move_to_deleted(identity)
    Repo->>Repo: acquire lease, rename database, move sidecars
    Repo-->>Route: success
    Route->>Manager: guard releases the reservation
    Route-->>UI: 204 No Content
```

Waiting for actor shutdown is bounded by a new `WebSettings::delete_deadline`,
default 10000 ms, validated at startup to exceed `sse_drain_deadline`. Teardown
publishes a final snapshot, waits out the bounded SSE drain, and then shuts the
controller down, so a deadline at or below the drain deadline would make every
live delete fail. Deleting a session while it is generating is an ordinary
case, not an edge case: controller shutdown stops the generation as part of
teardown.

If shutdown does not finish by the deadline, the route returns
`session_stopping`, leaves the database in the ordinary catalog, and releases
the maintenance reservation. The already requested actor shutdown may still
finish. The browser keeps the confirmation dialog open, reports that the
session is still stopping, and re-enables the destructive button so the user
can retry. It does not retry automatically, because a background retry would
outlive the dialog that authorized it.

### The maintenance reservation

The reservation is an RAII guard. A repository throw between reserving and
releasing must not leave an identity permanently unopenable, so release belongs
to a scope guard rather than to a statement on the success path.

`open()` and `try_reattach()` both refuse a reserved identity and report the
stopping failure, which is what the browser already understands for a session
that is going away. `lookup()` needs no change. It may briefly return the actor
that is about to tear down, because the reservation is taken while that actor
is still running, and that is harmless: teardown completes the queued commands
with the not-live outcome, and deletion does not depend on input having gone
quiet. Once the actor publishes `Finished`, this path sweeps and joins it like
any other manager operation, so a retried delete does not meet a corpse in the
collection.

Process shutdown does not cancel an in-flight deletion. A delete that has not
yet taken its reservation when `begin_shutdown()` publishes the stopping flag
reports `server_stopping`; one that already holds a reservation runs to
completion and releases through its guard, because the database move is
bounded, already authorized, and cheaper to finish than to unwind.

The reservation is also used for a non-live delete so a concurrent open cannot
start between the manager lookup and repository lease acquisition. The manager
does not perform filesystem work and does not hold its collection mutex while
waiting or while the repository moves files.

### The shutdown reason

Add `session_deleted` to `ShutdownReason`, to its serialization, and to
`shutdown_reason_priority()` between `session_failed` and `server_stopping`. A
deliberate deletion supersedes an incidental failure in what the browser is
told, while a process stop still outranks everything.

`session_deleted` does not skip the final drain — only `server_stopping` does —
so a second viewer of the same session still receives the final snapshot. That
snapshot must not claim the conversation remains saved in the active catalog,
which means `endedMessage()` gains a `session_deleted` case. Its current
`default` branch says the conversation is saved, so omitting the case would
produce exactly the false statement this rule forbids.

## Built-in Welcome session

The process-local Welcome session is not persistent workspace data. It cannot
be renamed or deleted.

`SessionRepository::create()` already refuses the temporary forum by throwing
`ForumNotFoundError`; rename and delete gain the same guard in the same place.
Without it both would succeed against the process-local database: rename would
edit a database that disappears at exit, and delete would create a `deleted/`
directory inside the private temporary directory. The routes map that error to
`404` with `not_found`, exactly as creation does.

Welcome does reach Recent, because the built-in Entrance forum is a member of
the workspace forum list and the bootstrap Recent listing iterates that list.
The browser therefore needs a discriminator, and bootstrap's
`initial_forum_id` and `initial_session_id` are it — they keep identifying
Welcome after the user has navigated elsewhere. The browser omits Rename and
Delete for that row rather than offering actions known to fail.

Copy remains available while Welcome is open because it operates only on the
browser's current snapshot.

## HTTP API

### Rename a stored session

```text
PATCH /api/v1/forums/{forum_id}/sessions/{session_id}
Content-Type: application/json

{"label":"Architecture review"}
```

Success returns `200 OK`:

```json
{
  "id": "2026-08-11-09-30-00-session",
  "label": "Architecture review"
}
```

### Delete a stored session

```text
DELETE /api/v1/forums/{forum_id}/sessions/{session_id}
Content-Type: application/json

{}
```

Success returns `204 No Content`. The JSON empty object retains the existing
mutation convention and allows the same content-type, size, exact-shape, and
Origin validation as other browser mutations.

### Error mapping

Both routes validate forum and session route components before consulting the
live manager or repository.

| Condition | Route | Status | Code |
| --- | --- | ---: | --- |
| Unknown forum or session | Both | 404 | `not_found` |
| The built-in Welcome session | Both | 404 | `not_found` |
| Invalid JSON or label | Both | 400 | `bad_request` |
| Mutation Origin does not match Host | Both | 403 | `forbidden_origin` |
| Session leased by another process | Both | 409 | `session_busy` |
| Live actor has not completed shutdown | DELETE | 409 | `session_stopping` |
| Live actor stopped before the command was accepted | PATCH | 409 | `session_not_live` |
| Deleted destination already exists | DELETE | 409 | `session_delete_conflict` |
| Request body exceeds the configured limit | Both | 413 | `body_too_large` |
| Storage or database failure | Both | 500 | `internal_error` |
| Owner command queue is full | PATCH | 503 | `command_queue_full` |
| Rename reply deadline expired | PATCH | 503 | `command_timeout` |
| Server is stopping | Both | 503 | `server_stopping` |

The three PATCH-only rows are the existing owner-queue outcomes; rename reuses
their current statuses and messages rather than inventing parallel ones.
`command_timeout` keeps its established meaning that the outcome is unknown.

Changing together: `ErrorCode` and `ShutdownReason` in the protocol header, the
OpenAPI document — including its `shutdown_reason` enum and the tightened
`createSession` description — the generated TypeScript declarations, the
runtime error-code allowlist, and the hand-written `ChaClient`.

The browser UI contract in `docs/web-ui/` changes with them. `behavior.md`
currently states that Recent rows have no actions and that top-bar actions do
not exist, `flows.md` has no rename or delete flow, and `api-requirements.md`
does not describe these endpoints. The `session/` and `web/` module READMEs
list per-file responsibilities that this change extends.

Copy does not add an HTTP endpoint.

## Sidebar actions

Each Recent row provides two ways to open its session action menu:

- Right-click or the platform context-menu gesture on the row.
- A visible, focusable ellipsis button labeled `Actions for <session label>`.

The explicit button is required for keyboard and touch users. The row can no
longer be one button containing another button; it becomes a wrapper containing
separate Open and Actions buttons.

The menu contains:

- `Rename…`
- `Delete…`

The menu is rendered in a portal so the sidebar's scrolling container does not
clip it. It is positioned at the pointer for right-click and at the action
button for button activation, clamped to the viewport.

This menu and the two dialogs below are the browser's first portal and its
first modal `<dialog>`; neither pattern exists in the app today, so both arrive
with their own unit coverage rather than leaning on established helpers.

The menu supports:

- Enter and Space to activate an item.
- Up and Down Arrow to move between items.
- Home and End to move to the first or last item.
- Escape and outside pointer activation to close.
- Focus restoration to the invoker when it closes.
- `aria-haspopup="menu"`, `aria-expanded`, `role="menu"`, and
  `role="menuitem"` semantics.

### Rename dialog

Rename opens a modal native `<dialog>` with the current label selected. Like
New session, the form trims before submitting and leaves the rest of the label
policy to the server. Submission is disabled for an empty, unchanged, or
pending value.

On success:

- Close the dialog.
- Refresh bootstrap so Recent receives the new label and ordering.
- Invalidate/refetch an already-mounted forum Sessions list.
- Let the actor's authoritative snapshot update an active chat label.

On failure, keep the dialog and edited value open and show the public error in
the dialog.

### Delete dialog

Delete opens a modal confirmation naming the session:

> Delete “Architecture review”? It will be removed from CHA and cannot be
> reopened.

Retention is an operator-facing property of the storage layout, not a promise
to the reader of this dialog. The wording does not name a directory the browser
cannot list, open, or restore from.

The destructive button is disabled while the request is pending. A failure
keeps the dialog open and reports the public error.

On success:

- Close the menu and dialog.
- Refresh bootstrap.
- Invalidate/refetch the selected forum's Sessions list.
- If the deleted identity is active, detach its stream, clear retry state,
  navigate to Welcome, and replace the current browser history entry with `/`.
- If another session is active, preserve it and the current main view.

## Browser state and invalidation

The existing bootstrap refresh updates Recent but does not invalidate the local
state owned by `SessionsScreen`. Add a lightweight session-catalog revision in
`App` and pass it to screens that load catalog data. Increment it after a
successful rename or delete; `SessionsScreen` includes it in its listing effect
dependencies.

Do not optimistically update labels or remove Recent rows before the server
commits. The successful response triggers refresh, while a live rename also
arrives authoritatively through the session snapshot.

Pending session-management mutations are keyed by forum, session, and action so
double activation does not create duplicate requests. Rename and delete for
the same identity must not run concurrently in one browser page.

## Failure and concurrency behavior

- A session may disappear after its menu opens. Rename and delete then return
  not-found and refresh the catalog.
- A session may become live after a non-live route decision. The repository
  lease decides the race and returns busy rather than mutating behind the actor.
- A delete destination collision never overwrites retained data.
- A rename that fails before the actor accepts the command, or that the actor
  rejects, leaves both the database label and actor descriptor unchanged.
- A rename reported as `command_timeout` has an unknown outcome. The waiter
  gave up; the owner thread may still persist the label and publish it. The
  browser must not present this as "unchanged", and reconciles through the
  authoritative snapshot and the next bootstrap refresh.
- An unsuccessful delete leaves the source database catalog-visible unless the
  database rename already committed. Once that rename succeeds, the operation
  is successful even if the following sidecar move fails.
- An abandoned browser request does not cancel an actor command whose outcome
  may already have committed. A subsequent bootstrap refresh reconciles the UI.

## Logging

Session-scoped logs continue to contain forum and session IDs, never labels or
conversation content. Add events for:

- `rename_requested`, `rename_committed`, and `rename_failed`.
- `delete_requested`, `delete_shutdown_requested`, `delete_moved`, and
  `delete_failed`.


## Test plan

### Session storage tests

- Rename persists a new label without changing session identity or transcript.
- Rename rejects missing, leased, invalid, empty, multiline, and oversized
  labels.
- Concurrent rename/open is serialized by the lease.
- Rename accepts a session whose stored label predates the label policy.
- Creation applies the same label policy, and an empty label still yields the
  generated session ID.
- Deletion creates `deleted/` and moves the database without overwriting.
- Deleted databases disappear from list and inspect returns not-found.
- Corrupt databases can be moved to Deleted.
- A held lease prevents deletion.
- A stale `.cha-lock` remains harmless.
- A sidecar beside the source moves after the database, under the destination
  name.
- A destination collision preserves both source and destination.
- Creation does not mint an ID whose stem is occupied in `deleted/`, so a
  session deleted and recreated within one clock second stays deletable.

### Live actor and manager tests

- Live rename persists, updates the descriptor, and publishes a snapshot.
- Failed persistence does not update the descriptor.
- Live deletion requests shutdown, waits for lease release, and moves storage.
- Deleting a generating session stops the generation and still completes.
- The maintenance reservation rejects a concurrent open and reattach.
- A reservation released by a throwing repository call leaves the identity
  openable again.
- A shutdown timeout leaves the database catalog-visible and returns
  `session_stopping`, and a retry after the actor finishes succeeds.
- A deleted session's final snapshot carries `session_deleted` and does not
  claim the conversation is saved.
- Welcome cannot be renamed or deleted.

### Route and protocol tests

- PATCH and DELETE validate route components, JSON shape, content type, body
  size, and Origin.
- Each storage and lifecycle failure maps to its documented status and code,
  including the owner-queue outcomes reachable only through a live rename.
- Successful rename returns the effective label.
- Successful delete returns an empty `204` response.
- Bootstrap and forum listing exclude a deleted session.
- Open returns not-found for a deleted identity.

### Browser unit tests

- Right-click and the ellipsis button open the same menu for the correct row.
- Menu focus, arrow keys, Escape, click-away, and focus restoration work.
- Welcome has no Rename or Delete action.
- Rename preserves input and shows errors on failure.
- Delete requires confirmation and reports pending state.
- Successful mutations refresh Recent and invalidate forum sessions.
- Deleting the active session replaces history and returns to Welcome.

### End-to-end tests

Both Playwright projects run these specs against one shared server and one
copied workspace, so a destructive test creates the session it deletes instead
of consuming a fixture session. Verifying the retained database also needs the
workspace path, which the launcher copies into a temporary directory and
currently publishes nowhere; export it alongside the API target.

- Rename from Recent updates Recent, the forum Sessions screen, and an open
  chat without changing its URL.
- Delete a closed session and verify it can no longer be listed or opened while
  its database exists under `deleted/`.
- Delete the active session and verify navigation returns to Welcome.

## Implementation order

1. Add shared label validation, apply it to creation, and add low-level
   database rename support.
2. Add repository rename and move-to-deleted operations, teach session creation
   to avoid stems occupied in `deleted/`, and cover both with storage tests.
3. Add manager maintenance reservations, the delete deadline, the
   `session_deleted` shutdown reason, and live rename/delete coordination.
4. Add protocol values, routes, OpenAPI declarations, and client methods.
5. Add catalog invalidation and active-session deletion handling in `App`.
6. Add the reusable sidebar action menu and rename/delete dialogs.
7. Complete route, browser, and end-to-end coverage.
8. Update the `docs/web-ui/` contract and the `session/` and `web/` module
   READMEs to match what shipped.
