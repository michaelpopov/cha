# Storing and showing a timestamp for each transcript entry

## What this is

Every response CHA receives from a model is journaled into the session's
SQLite database and rendered in the browser transcript, but nothing records
*when* it arrived. This change gives each journaled transcript entry a
wall-clock creation timestamp, persists it in the session database, and shows
it on each message in the web UI.

The two requirements, restated:

1. **Store** a timestamp with each model response in the session database, so
   the record of a conversation includes when each answer was produced.
2. **Present** a timestamp for each response in the web UI, so the reader can
   see when a given answer arrived without leaving the conversation.

## What exists today

There is no per-entry time anywhere in the pipeline:

- The `entries` table (`src/session/session_database.cpp`, `create_schema()`)
  stores `entry_id, epoch, request_id, kind, participant_id, display_name,
  addressed_to, addressed_to_name, text, status` — no time column. The
  `turns` table has none either.
- `TranscriptEntry` (`src/chat/transcript.h`) has no time field, and the
  entry factories in `src/chat/transcript.cpp` set none.
- `transcript_entry_json` (`src/web/protocol.cpp`) serializes no time, and
  the `TranscriptEntry` schema in `resources/cha.yaml` declares none.
- The transcript renderer in `webapp/src/components/Screens.tsx` shows the
  speaker and the text and nothing else.

The only time in the system is **per session**: `StoredSession.updated_at` is
the database file's modification time, shown in the Sessions and Recent lists
through the relative `formatSessionTime()` helper ("Now", "5m", "2d"). It
answers "when was this conversation last touched", not "when was this message
written".

One constraint shapes the storage work: `load_session_database()` rejects any
database whose `user_version` is not exactly `session_database_version`
(currently 2). Adding a column is therefore a schema-version event, and
existing session databases need a migration story — rejecting them would
silently strand every stored conversation, which is not acceptable.

## Decisions

### Scope: timestamp every journaled entry, not only responses

The requirement names model responses, but the column goes on **every**
journaled entry: the human prompt at `start_turn()`, the character response
or error entry at its terminal write, and any stored notice.

This costs nothing over timestamping responses alone — the schema change, the
journal write, and the restore read are identical in size either way — and a
chat view in which answers carry times but the prompts they answer do not
reads as broken rather than focused. Prompt and response are two halves of
one exchange; the reader's question "when did this happen" applies to both.
It also keeps the storage rule stateless: `insert_entry()` never inspects the
entry kind to decide whether the timestamp matters.

Off-record marker entries are never journaled, so they carry a timestamp in
memory only. They show their time like any other entry — the storage rule is
stateless, and so is the render rule: no kind check decides whether a time
appears, on either side.

### Moment: entry creation time, stamped by the factories

`TranscriptEntry` gains `created_at`, a `std::int64_t` of Unix seconds, and
the existing factories (`make_human_entry`, `make_character_entry`,
`make_notice_entry`, `make_error_entry`) stamp it from
`std::chrono::system_clock` at construction. `0` means "unknown" and is what
pre-migration rows read.

The alternative — stamping inside `insert_entry()` at journal commit — was
rejected. It would make a response's timestamp mean "when the answer finished
streaming" rather than "when it started appearing", and it would force the
live in-memory entry to be re-stamped at completion so that memory and disk
agree, or else leave the streaming entry with no time until it finishes.
Creation time keeps one value, set once, that is the same in the live
transcript, in the database, and after a restore. For ordinary exchanges the
two moments differ by the streaming duration, and "when this message
appeared" is the meaning a chat reader expects.

**One entry, two constructions.** Keeping that value single takes one extra
step, because a character response is built by
`SessionController::response_entry()` *twice*: once when the first answer
chunk opens the live streaming entry, and once at completion or cancellation
to produce the record handed to the journal. Those are separate objects — the
live one is never journaled, and `finish_entry()` only flips its status — so
a factory stamp alone would give the live entry the time the answer started
appearing and the stored entry the time it finished. The displayed time would
then change silently on the next restore, which is precisely the failure the
paragraph above rejects `insert_entry()` stamping for.

So the controller captures the stamp when it opens the response entry and
reuses it afterwards: the active-response state gains `response_created_at`,
set from the entry the factory just stamped, and `response_entry()` overwrites
its own fresh stamp with that value on every later call. Human prompts and
error entries need nothing — each is journaled from the very object the
transcript holds, so it can only carry one stamp.

A useful consequence: a streaming response already carries its timestamp
before it is ever journaled, so the UI can show the time of an answer that is
still arriving.

### Storage: one column, one schema version, a one-shot migration

`entries` gains:

```sql
created_at INTEGER NOT NULL DEFAULT 0
```

Unix seconds match the convention `StoredSession.updated_at` already uses
end to end (the browser multiplies by 1000 for `Date`). `STRICT` tables
permit `ALTER TABLE … ADD COLUMN` with a constant non-null default, so
migration needs no table rebuild.

`session_database_version` becomes 3, and opening an old database migrates it
exactly once:

- `validate_database_identity()` accepts versions **2 and 3**. The read-only
  paths — catalog listing, `inspect()`, route-level `validate()` — keep
  working against an unmigrated database without writing to it.
- `SessionRepository::prepare()` runs a new `migrate_session_database()`
  step after acquiring the lease and before `load_session_database()`: open
  the database read-write, and if its `user_version` is 2, run the `ALTER`
  and set 3 **in one transaction**. The lease is already the required
  exclusion, and opening is already a write-capable moment, so migration
  adds no new locking or lifecycle. A no-op on version 3 keeps it cheap on
  every later open.

  The transaction is not decoration. `ALTER TABLE` and `PRAGMA user_version`
  are two statements, and a crash between them would leave a database that
  has the column but still reads version 2. The next open would re-run the
  `ALTER`, fail with `duplicate column name`, and strand that session
  permanently. `create_session_database()` already wraps its schema creation
  the same way.
- `load_session_database()` keeps its read-only single connection and now
  reads `created_at` in `build_restore()`. Its contract gains one line: it
  requires a migrated database, and `prepare()` is what guarantees that.
  `rename_session_database()` needs no migration of its own — it updates only
  the `session` row, which exists unchanged in both versions.

New databases are created directly at version 3, including the process-local
Welcome session, so nothing on the creation path changes shape.

The rejected alternatives:

- **Dual-version readers** (accept 2 and 3 forever, select the column
  conditionally) avoid migration writes but keep two schema shapes in the
  code permanently — more machinery than a personal application needs.
- **Rejecting old databases** is the smallest change and strands every
  existing conversation. Dismissed.

Timestamps on the `turns` table instead of `entries` were also considered and
rejected: a turn pairs a prompt with its response, but the UI shows time per
message, and one row per exchange cannot say when each half appeared.

### Protocol: one nullable field, snapshots only

`transcript_entry_json` emits `created_at` on every entry, as an explicit
`null` when the value is 0 — an `nlohmann::json(nullptr)`, the same spelling
the character settings feature uses for absent `provider`/`style`. Not
`put_optional`, which sits in the same function serializing `request_id` and
does something different: it omits the key entirely.

The `TranscriptEntry` schema in `resources/cha.yaml` gains `created_at` as
`type: [integer, "null"]` and lists it in `required`. The file is OpenAPI
3.1, which dropped `nullable: true`, and the schema is
`additionalProperties: false`; required-and-nullable is what `provider` and
`style` already do, and it is what makes the generated browser type
`number | null` rather than an optional. The browser's types are regenerated
from it.

The append path needs nothing. `created_at` never changes after an entry
exists, and every new entry — prompt, opened response, finished response —
already reaches the browser through `SnapshotRequired`, which carries full
entries. Only pure text growth rides the append events, and text growth does
not touch the timestamp.

### Presentation: absolute local time on every message

The transcript renderer in `Screens.tsx` adds a subdued `<time>` element to
each `cha-message` article, following the pattern the Sessions screen already
uses (`<time dateTime={…}>` with a formatted label). Entries whose
`created_at` is `null` render no element at all — pre-migration history
simply has no times, and the UI does not apologize for it.

The label is **absolute**, in the browser's local timezone: the date and
the time on every message, carrying the year only outside the current one,
with the full timestamp in the element's `title`. The existing relative `formatSessionTime()` is
deliberately not reused: it is built for lists that reload, and on a
transcript that sits open its "5m" labels silently go stale because nothing
re-renders them. An absolute time is correct from the moment it is painted.

Placement must work for human entries, which have no speaker line (the
speaker row renders only for non-human kinds), so the timestamp goes on its
own subdued line under the message text — the same slot the existing
`Stopped` / `Failed` status lines use — rather than beside the speaker name.

### What the timestamp never touches

- **Model context.** `project_model_context()` is unchanged. Timestamps are
  presentation and record-keeping data; a provider never sees them, and the
  shared-history JSONL encoding keeps its current shape.
- **Copy conversation.** `conversationText()` keeps its deterministic
  plain-text format. Whether copied transcripts should include times is a
  separate decision, not bundled into this one.
- **Update classification.** Timestamps introduce no new controller mutation
  at finish time, so `ControllerUpdate` classification is untouched.
- **Recent / Sessions lists.** They keep ordering by `updated_at` exactly as
  today.

## What we are deliberately not doing

- **Not recording completion time separately.** One timestamp per entry,
  meaning creation. Duration or finish time is derivable from nothing we
  store and is not needed for the stated requirements.
- **Not backfilling old entries.** Pre-migration rows keep `0` and show no
  time. Inferring times from entry order or file mtimes would manufacture
  data we do not have.
- **Not timestamping turns.** See "Storage".
- **Not changing the copy format or model context.** See above.

## Known limitations

- Entries written before the migration show no timestamp in the UI. Their
  stored `0` is honest — the information was never recorded.
- The timestamp is the server wall clock at entry creation. It is not
  monotonic, not timezone-labelled in storage, and not a guarantee about
  provider-side timing; it records when CHA saw the message.
- Tests that compare whole `TranscriptEntry` values now compare `created_at`
  too. Factory-stamped values at second resolution make same-second
  comparisons pass, but any test building an expected entry literal must set
  the field deliberately rather than relying on a default. A test that
  factory-builds an expected entry and compares it against one built earlier
  in the same test can also straddle a second boundary; those should compare
  the fields they care about, or zero `created_at` on both sides, rather than
  relying on the clock.
