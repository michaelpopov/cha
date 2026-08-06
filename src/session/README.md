# Session layer

`session/` owns the reusable operations: it finds the workspace, manages session
files, persists the transcript, and coordinates one live chat. Every operation
it exposes is a typed C++ call — no command syntax, no widgets, no HTTP — so the
terminal front end, a future HTTP front end, and the tests all drive the same
code.

## Contents

| Source | Responsibility |
| --- | --- |
| `workspace.*` | Resolve the workspace layout, load persona context for agent definitions, list and validate forums and sessions, create stored sessions, load agent definitions, and build controllers. Persona selection and author resolution remain outside live sessions. |
| `session_catalog.*` | List, create, and safely resolve the SQLite session files of one forum. |
| `session_lease.*` | Acquire and own the cross-process companion-file lock for one live session. |
| `session_database.*` | Create and validate a session database, restore a transcript, and journal turn transitions through `SessionJournal`. |
| `forum_characters.*` | The ordered character identities in a forum, including validation, lookup, and handle resolution. |
| `session_identity.h` / `opened_session.h` | Stable `SessionIdentity`, presentation-safe `SessionDescriptor`, and the owner-thread-only `OpenedSession` result. |
| `session_state.*` | Owning `SessionState` snapshots and transport-neutral append continuity proof. |
| `session_change.h` | Semantic controller outcomes (`SessionChange`) and bounded event-drain results. |
| `session_controller.*` | Own one live session: commands, the in-flight response batch, agent events, default agent, notices, and shutdown. |
| `generation_status.h` | `GenerationStatus`, `ResponsePhase`, and the shared generation-in-progress notice. |

## Workspace layout

```mermaid
flowchart TD
    root --> forums["forums/"]
    root --> personas["personas/<id>/persona.toml + optional PERSONA.md"]
    root --> env[".env — optional"]
    root --> characters["characters/<id>/"]
    characters --> character["character.toml + CHARACTER.md<br/>definition + includes"]
    forums --> forum["forum-name/"]
    forum --> config["config.toml — required display_name + optional description/default_agent"]
    forum --> members["members/"]
    members --> base["character_defaults.toml<br/>optional forum defaults + [prompt]"]
    members --> member["<id>/character.toml + CHARACTER.md<br/>optional overrides"]
    forum --> forum_prompt["FORUM.md — template-expanded forum prompt extension"]
    forum --> sessions["sessions/&lt;id&gt;.sqlite3<br/>created on demand"]
```

`Workspace` refuses to construct unless `forums/`, `characters/`, and a valid
`personas/` directory exist. The directory may contain no custom personas;
the terminal application's built-in Guest persona makes its effective roster
non-empty.
The `forums/` directory may be temporarily empty; its valid forum names are
sorted before presentation. Forum IDs and session database stems may contain
only RFC 3986 unreserved ASCII characters, excluding the complete names `.` and
`..`; invalid entries are ignored during discovery and rejected on direct use.
Each forum's `members/` directory must contain at least one member subdirectory,
also sorted before loading. Member and definition directory names are character
IDs and are validated with `validate_character_id()`.
Each forum's `config.toml` must provide a non-empty string `display_name` and
may provide an optional single-line `description` for application inventory.
persona-facing selection and listings; its directory name remains the stable ID.
Each definition directory likewise supplies its stable ID, while its
`character.toml` provides the required persona-facing `display_name`. The loader
rejects the removed character-level `id` and `name` fields. `display_name` and
optional tags are definition-only: they are errors in forum defaults and member
overrides. Tags are trimmed, non-empty, control-free, and unique under ASCII
case folding; they are metadata only and never determine membership. Definitions
with no member directory in any forum are valid.

Workspace construction validates the whole directory graph before it serves a
forum: every member resolves to one definition, each configured `default_agent`
names a member, and character/persona IDs and case-folded display names do not
collide. An invalid workspace fails as a whole. `default_agent` leaves member
and agent-definition ordering unchanged; when it is absent, the first
lexicographic member ID is used.

Template containment follows the file's layer: a definition `CHARACTER.md` is
contained to workspace `characters/`; a member `CHARACTER.md` and `FORUM.md`
are contained to their forum directory.

`Workspace` supplies validated storage-facing metadata and forum definitions.
Each controller receives the effective application-wide persona roster and
resolves submitted stable author IDs against it. The roster also contributes
static model context, but it is not forum or session membership. The agent layer applies the shared provider
layer, definition, forum-default, and member override policy,
deriving the definition containment root from the definition directory's parent
and otherwise receiving resolved workspace paths explicitly.

`Workspace::check_forum()` follows the same loading path without creating or
opening a session. It also constructs `ForumCharacters` to validate character IDs,
display names, and uniqueness, giving the console's `--check` mode the same
static validation a real session receives before provider initialization. It
does not inspect the optional `sessions/` directory or resolve provider
credentials and models.

`Workspace::session_summary()` reads one selected stored session's identity,
label, and metadata directly, without scanning the other session databases or
acquiring its lease. `Workspace::check_session()` is the validation-only form.
Both distinguish an absent session from invalid or unreadable storage so front
ends can map only absence to not-found. Web lobby routes skip this disk
validation when their separate live-session registry can reattach directly,
and otherwise use it before asking the registry to open a session.

## Forum characters

`ForumCharacters` is the identity-only view of the characters participating in one
forum. It is ordered, non-empty, and rejects duplicate IDs and
ASCII-case-insensitive names. The workspace passes the validated initial default
character ID separately: it is `config.toml`'s `default_agent` when supplied,
otherwise the first lexicographic member ID. It never reorders this view.

`resolve_handle()` tries an exact case-insensitive name, retries after removing
trailing `,.;:!?`, and finally accepts a unique case-insensitive prefix. It
returns resolved, unknown, or ambiguous; `SessionController` owns the wording
of the corresponding persona notices.

Model, API, and streaming details do not belong to `ForumCharacters`.
`AgentRegistry` exposes those separately as `AgentRuntimeInfo`, and
`SessionController` combines the two only for `/agents` and `/info`.

## Session operations

```mermaid
sequenceDiagram
    autonumber
    participant UI as Caller
    participant WS as Workspace
    participant AG as agents/
    participant SC as SessionCatalog
    participant SL as SessionLease
    participant DB as Session database
    participant CC as SessionController

    Note over UI,DB: Creating a stored session
    UI->>WS: create_stored_session forum, label
    WS->>WS: load_forum, enumerate members/ directories
    WS->>WS: load_personas
    WS->>AG: load_agent_definitions(definition/member pairs, forum, roster)
    WS->>SC: create label
    SC->>SC: timestamp id, numeric suffix on collision
    SC->>DB: build hidden temporary sibling, then link into place
    WS-->>UI: SessionSummary with assigned ID and effective label

    Note over UI,CC: Terminal create and open convenience
    UI->>WS: create_session forum, label
    WS->>WS: load forum, roster, and definitions once
    WS->>SC: create label
    WS->>SC: open_database_path assigned ID
    WS->>SL: acquire lease
    WS->>DB: load_session_state
    WS->>CC: from_definitions; no persona membership roster
    WS-->>UI: OpenedSession (descriptor + controller)

    Note over UI,CC: Opening a session
    UI->>WS: open_session forum, id
    WS->>SC: open_database_path id
    SC->>DB: read metadata, check id and forum match
    WS->>SL: acquire `<database>.cha-lock` without waiting
    WS->>DB: load_session_state
    DB-->>WS: SessionRestore
    WS->>WS: load_personas, unless the caller supplies an effective roster
    WS->>AG: load_agent_definitions(definition/member pairs, forum, roster)
    WS->>CC: from_definitions with restore; supplied effective rosters retain Guest for browser-authored input, while terminal callers use workspace personas as before
    CC->>CC: repair interrupted turns, then install entries
    WS-->>UI: OpenedSession (descriptor + controller)
```

`Workspace::create_stored_session()` is the creation primitive. It first
performs the same complete forum validation as `check_forum()`, then delegates
publication to `SessionCatalog`. Publication uses `link(2)`, which fails rather
than overwriting, so a half-written database is never visible under a real
session name and a collision simply retries with the next numeric suffix. The
operation returns only a `SessionSummary`: it neither acquires a session lease
nor constructs a controller or provider. Web callers create and open in
separate operations, so an open failure leaves the successfully stored session
available for a later ordinary open.

`Workspace::create_session()` is the terminal convenience operation. It loads
and validates the forum, roster, and definitions once, publishes the stored
session, revalidates that database's identity, and constructs the controller
from the values already loaded. The returned `OpenedSession` couples its
controller to a descriptor derived from the same validated forum and stored
session metadata. Persona data used to assemble static prompts is not retained
by the controller as membership. Terminal creation logs `Session stored`
followed by `Session opened`.

Listing is tolerant: a file that fails validation still appears, with its error
attached, so the selector can show it instead of hiding a broken session.

Session paths are resolved only by `SessionCatalog`. Its `database_path()` and
`open_database_path()` require the session ID to be one safe path component
before appending `.sqlite3` beneath the forum's `sessions/` directory, so an
absolute path, `..`, or an ID containing a directory separator cannot escape
that directory.

Every live controller holds a `SessionLease` on `<database>.cha-lock`. The
companion file may remain after a run; only its non-blocking exclusive operating
system lock means the session is active. `Workspace` acquires the lease after
resolving a database but before restore, then moves it into `SessionController`.
The controller keeps it through explicit shutdown and journal destruction, so
`cha`, `chacon`, and future frontends fail immediately with `SessionBusyError`
when another process owns that stored session. Test-only controller factories
use an explicitly inactive lease instead of locking fixture databases.
On Windows, the companion handle intentionally omits `FILE_SHARE_DELETE`, so
the lock file cannot be deleted or renamed while its controller is alive.

## Persistence

One session is one self-contained SQLite file. Its `application_id` and
`user_version` are checked before anything else is read.

Opening a database initializes the SQLite library once, under `std::call_once`,
before the first connection is created. SQLite performs that setup lazily on
first use and writes its configuration globals and the unix VFS's process-id
record without full synchronization, so concurrent first opens from several
session owner threads would otherwise race on them.

```mermaid
erDiagram
    session {
        int singleton PK
        text id
        text forum
        text label
    }
    state {
        int singleton PK
        int history_epoch
        int next_entry_id
        int next_request_id
    }
    turns {
        int request_id PK
        int epoch
        int state "0 started, 1 completed, 2 cancelled, 3 failed"
    }
    entries {
        int entry_id PK
        int epoch
        int request_id FK
        int kind "0 human, 1 agent, 2 notice, 3 error"
        text participant_id
        text display_name
        text addressed_to
        text addressed_to_name
        text text
        int status
    }
    turns ||--o{ entries : "prompt and response"
```

What the schema enforces on its own, independently of the C++ code:

- `session` and `state` are single-row tables, guarded by a `CHECK` on the
  primary key.
- A partial unique index on `turns` allows **one** `started` turn at a time.
- A partial unique index on `entries` allows **one** prompt per turn.
- `CHECK` constraints restate the entry model: participant identity where it is
  required, addressing only on human entries, `failed` status only for error
  entries, `complete` status only for human and notice entries, non-empty text
  for completed agent entries, and `streaming` status excluded entirely.
- `entries.request_id` references `turns.request_id`, with foreign keys on.

### Epochs

`/clear` does not delete anything. It increments `state.history_epoch`, and
restore only reads entries of the current epoch. Older rows stay in the file.

### Transactions

Each of `start_turn`, `complete_turn`, `cancel_turn`, `fail_turn`, and `clear`
is one transaction. `start_turn` writes the turn row, the prompt entry, and the
advanced `next_request_id` together; a terminal call writes the state change
and the response or error entry together. So the durable turn state and the
entry that explains it can never disagree.

### Turn states and recovery

```mermaid
stateDiagram-v2
    [*] --> started: start_turn
    started --> completed: complete_turn, response required
    started --> cancelled: cancel_turn, response optional
    started --> failed: fail_turn, error entry required
    completed --> [*]
    cancelled --> [*]
    failed --> [*]
    started --> restored: session reopened while started
    restored --> failed: fail_turn during restore
```

A turn still in `started` when a session is opened is reported as an
`InterruptedTurn`. `SessionController::initialize()` must finish every one of
them through `fail_turn()` before any other journal write; only then does the
transcript become live. That is why `SessionRestore` documents the requirement
as part of its contract.

### What is never stored

Streaming status never reaches SQL: `require_storable_transcript_entry()` and
the schema constraints reject it. Reasoning is even further outside
persistence—it lives only in the active response and never enters a
`TranscriptEntry`. A cancelled agent response is stored only if it has answer
text. The off-record span and its marker entries are also run-time only:
reopening a session presents the complete durable conversation with no span and
no markers.

### Reviewed multicast durability tradeoff

Concurrent multicast deliberately has a narrower durability boundary than its
execution boundary. The database continues to permit one `started` turn: the
initial foreground child is journaled before the shared gate opens, while later
children may already be running or finished in memory before they become
foreground and receive their own durable turn records.

Consequently, a process crash can discard background child turn records,
addressed prompts, output, and completed answers that have not yet become
foreground. Recovery reports only the durably started foreground turn as
interrupted. This is a reviewed and accepted tradeoff. Multicast concurrency
and ordered transcript commits are requirements, but crash recovery of
background work is not.

The design therefore does not add a durable batch manifest, background-result
spooling, or out-of-order turn persistence solely to recover that work. When
choosing between additional crash durability and a simpler persistence and
controller model for multicast, simplicity wins. This decision concerns
process-crash recovery; normal event handling still commits every foreground
terminal outcome before making it visible.

## Session control

`SessionController` is the owner-thread-only engine behind one live session. It
offers borrowed views for direct frontends, `state()` for an owning
transport-neutral `SessionState`, and commands that return `SessionChange`.
`SessionChange` reports observable state changes, accepted input, controller
termination, and a presentation-safe notice; it never tells a widget to clear
itself or to navigate away.

| Command | Behavior | Semantic result |
| --- | --- | --- |
| `submit_prompt(author_id, text, handle)` | Resolves the stable author ID against the controller's application-wide roster, then resolves the character handle or falls back to the default agent and starts a turn. | On success, state changes and submitted input is consumed. Unknown authors or character handles and an empty prompt retain the draft; the roster is not forum membership. |
| `clear_transcript()` | Bumps the durable epoch, then clears the live transcript. | State changes; the text layer decides how a submitted command affects an editor. |
| `open_offrecord()` | Opens an off-record span at the current turn boundary. | On success state changes with no notice — the appended marker is the acknowledgement; on a precondition failure only a notice. |
| `extend_offrecord()` | Sets or moves the span's end to the current turn boundary. | As above. |
| `restore_offrecord()` | Cancels the span, returning its entries to model context. | As above. |
| `start_multicast(author_id, text, handles)` / `start_multicast_by_ids(author_id, text, ids)` | Resolves the stable author ID and textual character handles or target IDs once, then captures one immutable pre-multicast history, stages every distinct target concurrently, and commits foreground turns in target order. | Author or target validation failures start no batch; terminal notices are retained until multicast completion or abort cleanup. |
| `session_information()` | Entry count plus the forum characters and their runtime details. | A notice and consumed submitted input; state is unchanged. |
| `agent_information()` | Forum characters and runtime details, marking the default. | A notice and consumed submitted input; state is unchanged. |
| `set_default_agent(handle)` | Changes the default for this run only. | A successful change is observable state; a text command may consume its submitted input. |
| `request_stop()` | Cancels every live execution and starts non-blocking cleanup while retaining the foreground event channel, or says there is no active generation. | Immediate stopping notice, followed by the final stop notice after cleanup. |
| `receive()` | Drains the foreground channel, advances to already-buffered children in the same controller turn, and polls abort cleanup. | Merged semantic changes; after shutdown drains its batch, `controller_ended` is true. |
| `shutdown()` | Cancels executions, commits the retained foreground terminal state, and joins the session pool. | — |

Every command except `request_stop()` and `receive()` is refused while a turn or
multicast is active, with the shared in-progress notice. The controller and its
forum-character helpers format session notices — handle errors, forum-character
text, `/info` — because their wording belongs to the session, not to a UI.
For read-only activity checks, `is_generating()` avoids constructing a
`GenerationStatus` and copying its potentially growing reasoning text.
Frontends request the full status only when they are about to render it.

### Owning state and append continuity

`state()` builds the owning `SessionState` used when a consumer cannot borrow
the controller-owned transcript. A consumer may retain a compact
`SessionStateCursor` beside its published presentation state and ask
`text_append_since()` for a `SessionTextAppend`. The cursor stores only
revision, epoch, entry/open-entry identity, default agent, active request and
phase, plus published text lengths; it never stores transcript or reasoning
text. The proof relies on transcript revisions changing for every structural
or answer-text mutation, a single open streaming entry, and the controller's
owner-thread-only access. Any changed epoch, metadata, request, phase, shape,
or non-growing text is ambiguous and returns no append, requiring a full state
replacement. Transport sequence numbers, event names, and mailbox coalescing
remain frontend policy. `SessionState`, its cursor, and `SessionTextAppend`
contain no HTTP, JSON, SSE, or mailbox concepts.

The three off-record commands are the only ones that add a transcript entry
without a journal write. Each passes the current `next_entry_id_` to the
matching `Transcript` mutation *without* incrementing it and advances the
counter only after a `true` result, so a refused command burns no ID. The
marker entries are never journaled; a later durable write simply leaves their
IDs as gaps, which entry IDs already permit.

The controller does **not** parse `/commands`, mentions, HTTP routes, or JSON.
Front ends translate those into these calls.

## The in-flight turn

`SessionController` holds the mechanics and state of one in-flight response
batch. An ordinary prompt is represented as a one-child batch.

Starting a batch, in order:

1. Resolve and deduplicate every target, then capture one immutable pre-batch
   history and build every complete `CompletionInput`, including its moved run
   specification.
2. Stage all pool tasks behind a closed gate. A staging failure opens no gate,
   calls no backend, waits for any already-submitted task, and creates no
   durable turn.
   Expected runtime refusals are reported as
   `Request could not be dispatched` and preserve the persona's draft; invalid
   inputs are controller bugs and propagate. The one live registry batch keeps
   each run at its stable controller index.
3. Persist `start_turn()`, add the human entry, and install the active response.
   If in-memory activation fails after the journal write, the controller
   compensates with `fail_turn()` and discards the gated batch.
4. Open the gate once. Every staged backend may now begin concurrently, while
   only the foreground child's events are applied to the transcript. The
   background work remains intentionally volatile until each child becomes
   foreground, as described in the reviewed durability tradeoff above.

After a foreground terminal event is committed, the controller selects the next
child, activates its turn, and drains its already-buffered events in the same
controller turn. It clears the whole batch after its final terminal event.

Applying events:

| Event | Effect |
| --- | --- |
| Reasoning `AgentDelta` | Appends to ephemeral active-response state; the first sets phase to `reasoning`. |
| Answer `AgentDelta`, first one | Opens the streaming transcript entry, appends the answer, and sets phase to `answering`. |
| Answer `AgentDelta`, later | Appends answer text to the open transcript entry. |
| `AgentCompleted` while answering | `complete_turn()`, then finish the entry as `complete`. |
| `AgentCompleted` before any answer | Treated as failure: "completed without answer content". |
| `AgentCancelled` while answering | `cancel_turn()` with the partial answer, entry finished as `cancelled`. |
| `AgentCancelled` earlier | `cancel_turn()` with no response; ephemeral reasoning is cleared. |
| `AgentFailed` | `fail_turn()` with an error entry, the open streaming entry is discarded, the error is added to the transcript. |

Events whose request ID does not match the active turn are ignored, which is
what makes a cancelled turn's late fragments harmless.

`ResponsePhase` is monotonic during normal generation:
`waiting` → `reasoning` → `answering`. The separate `stopping` phase is an
abort-cleanup overlay. It keeps the foreground agent name visible while the
registry commits that child's terminal event and waits for all remaining
executions. A foreground completion already queued when `/stop` is processed wins
the race and is committed normally.

## Failure policy

Persistence failures are not recoverable at this level. Every journal call is
wrapped with the operation it was attempting — "Failed to persist completion of
request 7 for @Name" — and rethrown. The session ends rather than continuing
with a transcript the database does not agree with. Provider failures, by
contrast, are ordinary events: they become error entries and notices, and the
session continues.

## Dependencies

- **Depends on:** `transcript/` for transcript values, `agents/` for
  definitions, runtime information, execution, and events, `util/` for path and text
  helpers, and SQLite for storage.
- **Must not depend on:** `ui/` or `apps/`.

## Tests

| Test | Covers |
| --- | --- |
| `tests/session/unit_workspace.cpp` | Layout resolution, forum loading and checking, session create/open. |
| `tests/session/unit_session_catalog.cpp` | Listing, identity validation, collision handling, publish semantics. |
| `tests/session/unit_session_controller.cpp` | Command behavior, concurrent staging, ordered foreground drain, persistence ordering, stop races, activation-failure teardown, large buffered background output, restore, and repair. |
| `tests/session/unit_concurrent_controllers.cpp` | Independent owner-thread controllers, concurrent workspace/catalog access, and atomic catalog publication while listing. |
| `tests/transcript/unit_transcript.cpp` | `SessionJournal` and the session database, checked against the in-memory model they mirror: turn transitions, rollback, constraint violations, interrupted-turn recovery, and version rejection. |

Those database tests link `cha_sqlite3` directly, so they can assert on the
stored schema and rows rather than only on what the C++ API reports.
