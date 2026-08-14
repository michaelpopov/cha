# Implementation plan — transcript entry timestamps

Read `docs/design.md` first. It explains what is being built and why each
choice was made; this file is only the sequence.

The work is split into four blocks. Each block ends with a green build and
leaves the tree in a committable state. Run them in order — B depends on A,
C depends on B, D depends on all three.

Verification commands, from the repository root:

```bash
make test
```

```bash
make web-check
```

---

## Block A — core model, persistence, and migration

**Goal:** every journaled entry carries `created_at`, old databases migrate
on open, and nothing outside `chat/` and `session/` knows yet.

### Steps

1. **Add the field.** `TranscriptEntry` in `src/chat/transcript.h` gains
   `std::int64_t created_at{};` — `0` means unknown. The four factories in
   `src/chat/transcript.cpp` stamp it from `std::chrono::system_clock` at
   construction; `<chrono>` is a standard-library header, not a new project
   dependency, so `chat/` stays as self-contained as it is today.
   `require_storable_transcript_entry()` does not validate the field: `0` is
   a legitimate stored value for pre-migration rows.

2. **Keep the response's stamp stable across its two constructions.**
   `SessionController::response_entry()` in
   `src/session/session_controller.cpp` builds a character response twice —
   once to open the live streaming entry, once to build the record the
   journal stores — so a bare factory stamp would put the start time in
   memory and the finish time on disk, and the displayed time would jump on
   restore. Add `std::int64_t response_created_at{}` to the active-response
   state, set it from the opened entry at the `begin_entry` call site, and
   have `response_entry()` overwrite its own fresh factory stamp with that
   value whenever it is non-zero. Human prompts and error entries need
   nothing: each is journaled from the same object the transcript holds.

3. **Schema version 3.** In `src/session/session_database.cpp`:
   - `create_schema()` adds `created_at INTEGER NOT NULL DEFAULT 0` to
     `entries`.
   - `session_database_version` becomes 3.
   - `insert_entry()` writes the column from the entry.
   - `read_entry()` reads it at index 9, and `build_restore()` appends it
     **last** in its `SELECT` column list — `read_entry` indexes columns by
     number, so appending leaves every existing index untouched.
   - `validate_database_identity()` accepts `user_version` 2 **or** 3, so
     read-only paths (catalog listing, `inspect`, route `validate`) keep
     working on unmigrated databases.

4. **The migration.** Add `migrate_session_database(path)` to
   `session_database.*`: open read-write, read `user_version`, and if it is
   2 run — inside one `Transaction` — `ALTER TABLE entries ADD COLUMN
   created_at INTEGER NOT NULL DEFAULT 0` and `PRAGMA user_version = 3`. The
   transaction is what makes a crash mid-migration survivable; the design
   note explains why a half-migrated database would otherwise be
   unopenable forever. No-op on 3; throw on anything else, exactly
   as the identity validation does today. Call it from
   `SessionRepository::prepare()` (`src/session/session_repository.cpp`)
   after the lease is acquired and before `load_session_database()` — the
   lease is the exclusion, and this keeps `load_session_database()` itself
   read-only. Document on `load_session_database()` that it requires a
   migrated database and that `prepare()` is the guarantee. Do not add
   migration to `rename_session_database()`: it touches only the `session`
   row and works on both versions.

5. **Tests.**
   - `tests/chat/unit_transcript.cpp`: journal round-trip preserves
     `created_at`; a factory-stamped entry survives store and restore.
   - **The step-2 regression test**, and the one that matters most: stream a
     response, complete it, and assert the stored entry's `created_at`
     equals the live transcript entry's. Repeat for the cancellation path,
     which builds its journal record the same way.
   - Migration test: build a version-2 database in the test (run the old
     `entries` shape without the column and set `user_version = 2`), migrate
     it, and assert the column exists, pre-existing rows read `created_at`
     0, new writes store real values, and `user_version` is now 3. Assert a
     second migrate is a harmless no-op. Note that `load_session_state()`
     also goes through `build_restore()` and so now needs a migrated
     database too — this test must not read its version-2 fixture through
     that helper, which `tests/support/test_session_database.h` and much of
     `unit_transcript.cpp` rely on.
   - `tests/session/unit_session_repository.cpp`: `prepare()` on a
     version-2 fixture migrates and opens successfully.
   - Audit existing tests that compare whole `TranscriptEntry` values.
     Same-second factory stamps make most equality checks pass unchanged;
     any test building an expected entry literal must set `created_at`
     explicitly rather than relying on a zero default. A test that
     factory-builds its expected value can straddle a second boundary — have
     those compare the fields they care about, or zero `created_at` on both
     sides.

### Done when

`make test` passes, a fresh session database is version 3 with the column, a
hand-built version-2 database migrates on `prepare()` and restores its old
entries with `created_at` 0, and a completed response carries the same
`created_at` in memory and in the database.

---

## Block B — protocol

**Goal:** the timestamp crosses to the browser.

### Steps

1. **Serialize.** `transcript_entry_json` in `src/web/protocol.cpp` emits
   `created_at` on every entry, as `null` when the value is 0. Write it as an
   explicit `nlohmann::json(nullptr)`, the same spelling absent
   `provider`/`style` already use — **not** `put_optional`, which sits in the
   same function serializing `request_id` and omits the key instead. No
   protocol struct changes: the snapshot owns `TranscriptEntry` copies, so
   the field rides along for free.

2. **API spec.** Add `created_at` to the `TranscriptEntry` schema in
   `resources/cha.yaml` as `type: [integer, "null"]` (Unix seconds), and add
   it to that schema's `required` list. The file is OpenAPI 3.1, which
   dropped `nullable: true`, and the schema is `additionalProperties: false`;
   required-and-nullable matches `provider`/`style` and generates
   `number | null` rather than an optional. Do this in the same block as the
   C++ that serves it — never in Block C.

3. **Regenerate types.** `cd webapp && npm run api-types`.

4. **Tests.** `tests/web/unit_protocol.cpp` for the JSON shape, including
   the `0` → `null` case. Update `webapp/src/test/fixtures.ts` so fixture
   entries carry the field; `make web-check` should pass before Block C
   starts.

### Done when

`make test` and `make web-check` pass, and a session snapshot carries
`created_at` on every entry.

---

## Block C — web UI

**Goal:** each message shows when it appeared.

### Steps

1. **Formatting helper.** Add `formatEntryTime(seconds, now)` beside
   `formatSessionTime` in `Screens.tsx` (or its own module if that file's
   size argues for it): the date and the time on every message, carrying the
   year only outside the current one, using `toLocaleTimeString` /
   `toLocaleDateString`. Absolute, browser-local — do not reuse the relative
   helper; the design note explains why relative labels go stale on an open
   transcript.

2. **Render.** In the transcript `entry.kind` article in `Screens.tsx`, add
   a subdued `<time className="cha-message-time">` line **under** the message
   text — the same slot the `Stopped` / `Failed` status lines use — because
   human entries have no speaker line to put it beside. Set `dateTime` to
   the ISO instant and `title` to the full local timestamp. Render nothing
   when `created_at` is `null`. One new CSS class in
   `webapp/src/styles/app.css`, styled like `cha-entry-status`.

3. **Tests.** A `formatEntryTime` unit test next to `SessionTime.test.ts`
   (date-and-time labels, year only outside the current one); a
   `LiveChat.test.tsx` case asserting a timestamped entry renders its time
   and a `null` entry renders none.
   Extend the App-level transcript assertions if they snapshot message
   structure.

### Done when

`make web-check` passes, and a manual `make run` shows a time under each
message of a conversation, including one still streaming.

---

## Block D — documentation and final pass

### Steps

1. `src/session/README.md` — the schema diagram gains `created_at` on
   `entries`, and the persistence section states the version-3 migration
   contract: read paths accept 2 and 3, `prepare()` migrates, restore reads
   require 3.
2. `src/chat/README.md` — the entry-model field table gains the timestamp
   and its "creation time, 0 means unknown" meaning.
3. `docs/web-ui/behavior.md` — the chat presentation contract mentions the
   per-message timestamp and its absence on pre-migration entries.
4. Re-read `docs/design.md` against what was built and correct any drift,
   particularly "Known limitations".
5. Full verification: `make test`, `make web-check`.

### Done when

Both verification commands pass and no document still describes the
transcript or the session schema as timeless.

---

## Notes for whoever picks up a block

- `CLAUDE.md` governs: prefer the smallest change. This feature is one
  column, one field, one JSON key, one element — resist adding clocks,
  duration tracking, or turn-level timestamps.
- The migration is the only step that can damage data. Keep it to exactly
  the `ALTER` and the version bump, in one transaction, behind the lease,
  and never rebuild the table.
- `resources/cha.yaml` is the source of truth for the browser's types.
  Change it in Block B with the C++ that serves it, never in Block C.
- Do not send timestamps to providers. `project_model_context()` and the
  shared-history encoding stay exactly as they are; if a diff touches them,
  it is wrong.
