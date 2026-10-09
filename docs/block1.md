# Block 1: Picture storage

Status: implementation plan; this stage is not implemented.

Read [picture.md](picture.md) and the repository instructions before starting.
This is the first of three sequential stages. Finish its checks before
continuing with [block 2](block2.md). Keep the implementation small and use
the existing configuration store.

## Outcome

Supported character pictures survive import, export, configuration saves,
database copies, merge, and deletion. Pictures remain outside prompts and
assistant configuration tools. There is no visible panel in this stage.

## Implementation

1. Define one small shared filename recognition helper for the exact names
   and MIME types in `picture.md`: `PICTURE.png`, `PICTURE.webp`, `PICTURE.jpg`,
   `PICTURE.jpeg`, and `PICTURE.gif`, in that priority order. Share the rule
   between storage, import, template rejection, and later picture lookup.
   Do not introduce an asset registry or configurable format list.
2. Extend `validate_stored_config_name` in
   `src/storage/workspace_session_database.cpp`. Retain its relative-path and
   containment rules. Accept a picture only when the last path component is
   an exact supported filename. This low-level check does not decide whether
   the parent directory defines a character.
3. Extend collection in `src/workspace/workspace_config_store.cpp`. Import
   pictures only from `system/assistant/` or a definition directory beneath
   `characters/` containing a regular `character.toml`. Support nested
   definition directories. Preserve the existing symlink and containment
   protections. For files with a supported image extension but an unsupported
   name or location, skip the file and write a warning naming it. Do not
   import forum-member overrides.
4. Encode imported image bytes as standard base64 in the existing
   `ConfigFile::content` strings. Leave Markdown and TOML unchanged. Decode
   picture rows when `materialize_config_files` exports them to binary files.
   Runtime workspace loading must retain the stored base64 representation;
   it must not depend on files in the modify directory. Use a small byte
   codec; the existing PCM decoder has audio-specific restrictions and must
   not be reused unchanged for arbitrary image bytes.
5. Keep the strict `TEXT` table and current schema version. Reuse the current
   row operations for saves, copies, and merge. Check that ordinary settings
   edits preserve picture rows without re-encoding them. Keep the documented
   merge behavior when different formats coexist. Character deletion must
   remove its picture rows through the existing directory deletion path.
6. Reject picture files before template expansion reads their contents in
   `src/util/text_template.cpp`, for both disk and stored-file sources. Use
   the existing include error path. An unused or undecodable image alone must
   not make a workspace invalid; an explicit attempt to include one is an
   invalid prompt include.
7. Filter picture paths from `list_config`, deny their contents in
   `read_config`, and reject all picture writes through `apply_config` in
   `src/workspace/workspace_config_store.cpp`. Keep ordinary text operations
   and credential protections unchanged. Check the public tool behavior in
   `src/app/assistant_service.cpp`; do not create a bypass for known paths.

Import must not decode images or enforce a new image size limit. Invalid image
bytes are stored normally and will produce a panel error in block 3. If export
encounters malformed stored base64, report an export failure through the
existing error path; do not silently replace or discard the row.

## Focused verification

Extend the existing suites instead of creating a separate test framework:

- `tests/workspace/unit_workspace_config_store.cpp`: import/export byte
  equality for all five filenames, including zero bytes and non-UTF-8 bytes;
  nested definitions and Assistant; warnings for ignored names and locations;
  ordinary settings saves; configuration merge; character deletion.
- `tests/storage/unit_workspace_session_database.cpp`: accepted picture names,
  rejected unsafe paths, database/configuration copies, and unchanged schema
  and text-row behavior.
- `tests/util/unit_text_template.cpp`: picture includes are rejected for disk
  and stored sources while ordinary Markdown includes continue to work.
- `tests/app/unit_assistant_service.cpp`: pictures are absent from listings,
  explicit reads expose no base64, and create/replace/set attempts cannot
  write picture paths. Include the Assistant directory.

Test a workspace with no pictures and one with a valid filename containing
non-image bytes. Both must still load. Verify failed import preserves the
previous committed configuration and export never changes the database.

Run from the repository root with the normal build prerequisites installed:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure -R '^(WorkspaceConfigStore.*|RuntimeWorkspaceConfigStoreTest|WorkspaceSessionDatabase|Workspace|TextTemplate|AssistantService)\.'
```

The filter includes both fixture-based configuration-store suites. Run any
additional affected suite if implementation changes extend beyond these
paths. Do not rerun unrelated suites repeatedly after successful checks.

## Completion and handoff

Report the changed files, shared recognition/codec helpers, and test results.
Block 2 must be able to read the original base64 rows from the configuration
store. Do not add a second persistent image store, cache, or workspace asset
index to make that handoff possible.

Older builds reject vaults containing picture rows. The eventual rollout must
update both desktop hosts and the ChaWeb daemon before users import pictures.
This stage adds shared storage support, not a ChaWeb picture interface.
