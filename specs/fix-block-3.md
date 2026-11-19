# Fix block 3: Storage speed

**Fixes:** F11, F12, F13, F14 · **Written:** November 19, 2026

## Before you start

- This file is complete. Do not read `docs/fix-plan.md`, the architecture
  reviews in `docs/`, or `specs/p0-d1-design.md`.
- **Start condition:** none. Start from the latest `main`.
- Other work (P0-D1, a D1 sync rework) is in progress. It also changes
  `src/storage/note_storage.cpp`: it keeps the SQLite result code in the note
  apply path so that D1 can classify errors. Do not change D1 sync code or the
  D1 apply path. If your change meets P0-D1 changes in the same function, stop
  and tell the user.
- File paths and line numbers come from commit `11ca9039`. Lines can move.
  Find the code by the function name.

## Approvals

- **ChaWeb permission:** F11, F12, and F13 change `cha_lib` code that
  cha-daemon also runs. For F14, find out first whether cha-daemon runs
  `note_import_folder`; if it does, F14 also needs the permission. Before you
  change code, ask the user for explicit permission. If the user does not give
  it, stop. Run ChaWeb tests only with this permission.
- **UI approval:** none. No visible text changes.

## Rules

- Make the smallest fix. Do not add machinery.
- **Measure first** for F12 and F13. Show the numbers to the user and ask
  whether to continue. If the user says the cost is too small, close the item
  with no change.
- For measurements, use a test vault that you make, or ask the user for one.
- Add only the tests that this file names. Run the tests of the changed area.
- Do not change `docs/` or `specs/`. The document updates are a separate task.
- Do not commit unless the user tells you to.

## F11. Read transactions for `note_list` and `note_get`

**Problem:** `note_list` and `note_get` use the default `SqliteTransaction`
mode, which is `BEGIN IMMEDIATE`. Each read takes the write lock and waits
behind D1 apply and session writes.

**Fix:** Use `SqliteTransaction::Mode::read` in both functions
(`src/storage/note_storage.cpp:1406-1418, 1511-1546`). Keep
`synchronous = FULL`.

**Test:** No new test. Run the notes storage tests. Run the ChaWeb notes tests
only with ChaWeb permission.

## F12. One keyed connection for the audio lookup (measure first)

**Problem:** `SessionRepository::cached_audio_entries` opens a new database
connection for each full snapshot
(`src/storage/session_repository.cpp:813-828`). In a protected vault, each
open runs the SQLCipher key derivation (PBKDF2, 256,000 iterations). A turn
makes about 2 to 4 snapshots. Nobody has measured the cost.

**Fix:**

1. Measure the snapshot time on a protected vault.
2. If the user agrees to continue, run the same query on the journal's open
   connection. That connection belongs to the same thread.
3. Keep fresh audio state and the epoch-safe teardown.

**Test:** No new test.

## F13. Skip the annotation rebuild when nothing changed (measure first)

**Problem:** In a folder with an ANNOTATIONS note, each `note_save` calls
`rebuild_folder_annotations`. It decompresses and parses every note of the
folder inside the write transaction. Autosave runs every 5 seconds while you
type.

**Fix:**

1. Measure with an annotated folder that has many notes.
2. If the user agrees to continue, compare the old and new annotations of the
   saved note. Skip the rebuild when they are equal
   (`src/storage/note_storage.cpp:619-684, 1589`).

**Test:** One test: a save that changes no annotation does not rebuild.

## F14. Shorter write transactions in folder import

**Problem:** A journal write that waits more than 5 s for the write lock
throws, and the live session stops. The reply in progress is lost. The chat
can be opened again. The clear cause is `note_import_folder`. It copies all
notes and all audio in one write transaction.

**Fix:**

1. Copy the notes in one transaction, as today.
2. After that, copy the audio in separate short transactions
   (`src/storage/note_storage.cpp:2235-2345`). If an audio copy fails, the
   notes stay. Audio is a cache that CHA can make again.
3. Keep the current fail-closed session behavior.

**Test:** No new test. Run the folder import tests.

## Not in this block

- Do not keep a session alive after a failed journal write. That needs
  reconciliation logic.
- Keep `synchronous = FULL`. Do not change it to `NORMAL`.
- Double validation at session open: no change.
- File and mirror I/O on the runtime thread: no change.
- Audio in the vault database (a sidecar file), R2 upload preparation cost:
  no change.
- EPUB unzip cost and the 16 MiB limit: no change.

## Done when

- F11 and F14 are in the code.
- F12 and F13 are measured. Each one is in the code, or closed with no change
  by the user's decision.
- The named tests pass, and the tests of the changed areas pass.
- Your final report says, for each fix, what changed, the measured numbers
  for F12 and F13, and which tests ran.
