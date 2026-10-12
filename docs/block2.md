# Block 2: Notes storage and native API

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 2; plan steps 4–5. Next: [block 3](notes-block3.md).

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

Implement plan **steps 4–5**, storage before native exposure. All seven Notes
operations work through the production trusted bridge, using the existing
vault database/locks. Older vaults gain the additive table without a new schema
version. This block introduces no visible Notes interface or HTTP API.

## Prerequisites and reading

Read block 1's baseline/platform handoff and inspect its commit/results.
Complete its technical baseline; unresolved visible approval does not prevent
this nonvisible work. Read plan steps 4–5, design sections 5–6 and 9.3/14, and
PRD sections 3, 5.1–5.2, and 9. Use disposable plain/protected vaults.

## Main code paths

- New `src/storage/note_storage.{h,cpp}`: SQL, item structures, schema creation.
- `workspace_session_database.cpp`, `WorkspaceConfigStore` open/forwarders and
  maintenance reopen, `session_label.{h,cpp}`, and existing CMake targets.
- `Application`, bridge protocol/operation dispatch, new `notes_dispatch.cpp`.
- `resources/dto.yaml`, generated `webapp/src/api/schema.d.ts`, client/native
  client/guards, test mocks, wire fixtures, and method-policy fixtures.

## Implementation sequence

### 1. Add the shared schema on the existing connection

Implement design section 5.1's strict `note_items` table: positive safe
`INTEGER PRIMARY KEY AUTOINCREMENT` ID bounded by 9,007,199,254,740,991; nullable
parent foreign key with `ON DELETE CASCADE`; immutable note/folder kind; binary
name; folder null content and note non-null content. The sibling unique index
uses `IFNULL(parent_id, 0), name`; add the parent index for child/cascade access.
The root is not a row. Notes/folders share one exact sibling namespace.

Call shared `create_note_items_schema(Database&)` from new-database creation,
store open, and maintenance reopen after identity checks. Each caller owns its
transaction; nest none. Preserve schema-2 identity, `user_version`, schema-1
import behavior, integrity checks, and database protection. Do not scan all note
JSON at startup/transfer, add migrations, open another connection, or publish
configuration snapshots/revisions for Notes mutations.

Keep normal lock order: Application lifecycle mutex → store mutex → transaction.
Add small store forwarding methods under existing ownership/maintenance rules.

### 2. Reuse label validation and implement atomic operations

Share session-label validation with narrow Notes-specific field/error wording,
leaving session errors unchanged. Require nonempty valid UTF-8, at most 200
Unicode code points, prohibited control/line breaks absent, and no leading/
trailing whitespace. Preserve spelling; do not trim/normalize or impose filename
rules. Compare siblings case-sensitively across kinds, including Top level.

| Operation | Storage behavior |
| --- | --- |
| List | Select metadata only: id, parent, kind, name. |
| Create | Validate kind/name/folder parent/duplicates; insert folder null or valid empty note; commit and return metadata. |
| Get | Require an existing note, parse content as an object, enforce byte limit, return it; never substitute empty content. |
| Save | Compact-serialize object, enforce limit, update existing note only, require one affected row and commit. |
| Rename | Validate sibling uniqueness excluding self; update only name and return metadata. |
| Move | Validate destination folder/uniqueness/cycles; update only parent and return metadata. |
| Delete | Require existing item; cascade subtree deletion and commit. |

New notes use `{"type":"doc","content":[{"type":"paragraph"}]}`.
Use SQLite ID allocation, never MAX+1 or INSERT OR REPLACE. Deleted IDs are not
reused. Unchanged rename/move succeeds as a no-op. Rename/move preserve bodies.

Put source/parent/uniqueness checks and mutations in one locked transaction.
Use the design's recursive destination-ancestor CTE with `UNION` to terminate
on damaged ancestry; root moves need no ancestry query. Reject self/descendant
moves without overwrite/merge/automatic rename. Let the index enforce final
uniqueness and map constraint failures to specific public errors.

### 3. Enforce the opaque document contract

Core accepts JSON objects and measures compact UTF-8 `nlohmann::json` output.
Allow exactly **12,582,912 bytes**; reject one byte over. Include keys, marks,
attributes, escapes, and encoded images, excluding bridge envelope bytes.
Do not validate Tiptap schema, URL policy, image headers, or picture limits in
C++; those belong to frontend entry points. Bad stored JSON is an opening
failure. Saves never create deleted IDs and report success only after commit.

Add byte-limit fixtures for the core with multilingual text, escaping, actual
editor attributes, exactly 12,582,912 bytes, and one byte over. Generate the
large documents in the tests from a small description, for example a base
document and a padding length; do not commit multi-megabyte files. Keep that
description where the C++ and Vitest tests can both read it, so block 4 can
reuse it for frontend parity. Marked-node splitting is an editor case; block 4
adds it. An early full-copy round trip must preserve embedded image documents
and SQLite's non-reuse sequence.

### 4. Expose ordinary native operations

Define transport-neutral `NoteItem`, `NoteItemList`, object `NoteDocument`, and
`NoteContent` DTOs; regenerate TypeScript using the existing generator.

| Method | Params | Result |
| --- | --- | --- |
| `notes.list` | `{}` | `{items: NoteItem[]}` |
| `notes.create` | `{parent_id, kind, name}` | `NoteItem` |
| `notes.get` | `{id}` | `{id, document}` |
| `notes.save` | `{id, document}` | `{}` after commit |
| `notes.rename` | `{id, name}` | `NoteItem` |
| `notes.move` | `{id, parent_id}` | `NoteItem` |
| `notes.delete` | `{id}` | `{}` after commit |

Use positive JavaScript-safe integers and null root parents, and send documents
as objects, not escaped JSON strings. Register method names in shared bridge
protocol; the thin dispatcher validates exact shapes/IDs/kind/nullable parent.
Application admits the supplied epoch under `lifecycle_mutex` before store
access. Keep trusted connection, nonzero epoch, protocol version, 16 MiB request
limit, and 16 ordinary in-flight request behavior. Notes is ordinary traffic,
not control/context-changing traffic. No routes, event stream, cancel method,
revision field, or custom queue is required.

Expose `listNoteItems`, `createNoteItem`, `getNote`, `saveNote`, `renameNoteItem`,
`moveNoteItem`, and `deleteNoteItem` in the native client with shape/safe-ID
response guards. Update all affected mocks/wire/method fixtures. ChaWeb gets
no Notes endpoint/editor. Existing generic hosts need no Notes-only transport.

Map invalid arguments, missing rows, document size, stale vault, maintenance,
and stopping through existing codes with specific Notes public wording.
Unexpected SQL/corrupt content uses safe opening/saving errors. Log technical
failures without document content. Queue admission is never save success; a
lost/rejected reply remains unacknowledged and retry sends the current full
object. Preserve existing fatal queue-overflow recovery rather than inventing
another request scheduler.

## Verification

Extend existing storage/store/application/bridge suites and native client/wire
fixtures. Register new source/test files with existing targets. Cover:

- New/older schema-2 plain/protected vaults, schema-1 compatibility, maintenance
  reopen, unchanged identity/integrity rules, full copy and ID sequence.
- Root/nested cross-kind duplicate create/rename/move, case distinction, same
  names in different parents, UTF-8/code-point/whitespace boundaries, rollback,
  missing/non-folder parents, cycles, no-ops, cascade delete, and deleted-ID
  save failure/non-reuse. Metadata listing must not load bodies.
- Object/non-object/corrupt JSON and exact byte fixtures, both get/save;
  committed update-only success, repeated full-document saves, opaque foreign
  content, and no configuration revision/undo/snapshot effect.
- All seven wire methods, strict params, safe/unsafe/null IDs, malformed replies,
  public errors, stale epochs between vaults with equal numeric IDs, and
  maintenance/stopping admission. Failed old-context writes leave both vaults
  unchanged.

Run relevant C++ suites, DTO generation/check, targeted frontend API suites,
and normal affected builds from plan section 5. Full-copy/protection checks use
disposable data. Record actual results rather than a written test matrix.

## Completion and handoff

Storage, compatibility, API, admission, and boundary tests pass; production
native calls can list/create/load/save before a screen exists. Hand off exact
contracts, schema entry points, error mapping, byte fixtures, copy/sequence
evidence, and pending platform gates. Block 3 can proceed independently of
visible Notes approval; block 4 consumes storage/API and boundary fixtures.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.

COMPLETED
