# F4: Correct the contract finding and fix the concrete gaps

Status: Implementation plan based on source inspection at `3eb183ff`.
The validation commands below have not been run for this plan.

## Corrected finding

Shared native/frontend wire fixtures already exist. C++ serialization tests in
`tests/runtime/unit_wire_fixtures.cpp`, bridge tests in
`tests/bridge/unit_bridge_protocol.cpp` and `unit_bridge_router.cpp`, and frontend
tests in `webapp/src/api/` consume files from `tests/fixtures/wire/`. A field rename
in a covered native serializer fails its fixture comparison.

Coverage is incomplete, not one-sided. Notes, events, background audio,
compression, and session summaries have separate native and frontend tests, but
lack shared fixtures for some result variants. The folder-store contract already
covers a folder item with a store. Absence of a shared fixture does not by itself
establish a broken operation.

This task fixes two demonstrated guard inconsistencies and adds one notes
fixture. Defer shared fixtures for the other families until a concrete change or
failure needs them. Do not add a bridge-wide inventory or deliberate-breakage
exercise.

## Changes

### 1. Align two guards with the existing schema

In `webapp/src/api/nativeClient.ts`:

- In `isNoteItem`, require `name.length > 0` after the string check, matching
  `NoteItem.name.minLength: 1` in `resources/dto.yaml`.
- In `noteAudio.info`, require exactly the `count` key using the existing
  `hasExactKeys` helper. Keep the safe nonnegative-integer check. This matches
  `NoteAudioInfo.additionalProperties: false`.

Add one malformed-reply case per change in `nativeClient.test.ts`: a notes-list
item with `name: ''`, and audio info with `{count: 0, extra: true}`. Exercise the
public client methods and expect `ChaProtocolError`; use the existing audio
bootstrap helper. Keep existing valid and invalid cases.

`isNoteItemList` uses `every`, so one invalid item rejects the entire list. This
matches its existing behavior; do not silently filter items. Supported note
writes validate names through `require_name` in `src/storage/note_storage.cpp`,
and summary naming also uses it. The change does not require a vault migration.

### 2. Add one shared notes fixture

Add `tests/fixtures/wire/notes-contract.json` with representative `NoteItem`
values for a folder, ordinary note, annotation, summary, and forum-owned folder,
plus `NoteContent` with and without compressed text and with a saved scroll
position. Reuse the existing folder-store fixture for its store-specific variant.

In `tests/runtime/unit_wire_fixtures.cpp`, construct the corresponding native
`NoteItem` and `NoteContent` values with fixed IDs and compare their actual
`to_json` output against this fixture. These serializers live in
`src/storage/note_storage.cpp`. Fixed DTO inputs avoid database setup, generated
IDs, and normalization machinery.

In `webapp/src/api/nativeClient.test.ts`, feed the same fixture through
`createFakeNativeBridge` and the public `listNoteItems` / `getNote` methods, so
production guards check it. Do not export private guards or cast JSON as a
substitute for validation. Replace equivalent handwritten positive payloads
where practical rather than duplicating assertions.

Tests read the fixture independently. They do not generate files, require a
particular suite order, or introduce a schema/code generator. No schema change
is required for either guard correction.

## Validation and closure

Run from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja --target cha_app_tests
./build/ninja/cha_app_tests --gtest_filter='WebWireFixtures.*'
npm --prefix webapp run check
```

Keep the new native fixture test in the `WebWireFixtures` suite. Close the scoped
work when both malformed replies are rejected, the notes fixture passes native
serialization and frontend validation, and the existing frontend checks pass.
This does not claim complete coverage of every bridge method.

Report changes and test results briefly in the final chat response or commit
message. Do not produce a coverage report or additional `docs/` files during
implementation unless the user explicitly requests them.
