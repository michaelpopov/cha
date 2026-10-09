# Block 2: Picture lookup and native bridge

Status: implementation plan; this stage is not implemented.

Prerequisite: [block 1](block1.md) and its storage checks are complete.
Read [picture.md](picture.md) and the repository instructions. This stage
adds the read interface used by [block 3](block3.md), with no visible UI change.

## Outcome and interface

Add `character.picture.get` with params `{ "character_id": "..." }` and the
normal context epoch in the bridge envelope. Expose it to components as
`ChaClient.getCharacterPicture(characterId)`.

Return JSON `null` for a known character without a picture. Otherwise return:

```json
{
  "filename": "PICTURE.png",
  "mime_type": "image/png",
  "content_base64": "..."
}
```

An unknown character uses the existing not-found error. Invalid arguments and
stale vault contexts use existing application/bridge errors. Empty or corrupt
image content is still a picture result; rendering failure belongs to the UI.

## Implementation

1. Resolve a character's definition directory from the existing workspace
   lookup in `src/workspace/workspace.h` and `.cpp`. The retained
   `character_config_paths_` map already identifies nested definitions. Add
   only the narrow accessor needed to obtain its parent directory. Resolve
   Assistant to `system/assistant/`, as character settings already do. Do not
   reconstruct `characters/<id>` or search forum-member directories.
2. Read picture data through `WorkspaceConfigStore` using the published
   workspace and committed configuration rows under its existing locking
   rules. `Workspace` retains resolved definitions, not the complete raw file
   map passed to `Workspace::load`; do not retain a second full map or a
   borrowed `TextSource` to implement this read. Reuse the configuration-row
   reader and select exact candidate paths in the documented priority order.
3. Return the first existing supported row, with its basename, MIME type,
   and stored base64 unchanged. Do not decode it, inspect image dimensions,
   fall back after a corrupt row, or read from the exported modify folder.
   Use a small result value rather than adding image fields to character
   metadata or session state.
4. Add the application operation in `src/app/application.h` and `.cpp`, with
   any necessary mapping in `src/app/workspace_operations.h` and `.cpp`.
   Follow `Application::get_character`: take the lifecycle lock and check
   admission with the request's context epoch before reading the store.
5. Define the response DTO and serialization beside the existing character
   DTOs in `src/runtime/protocol.h` and `.cpp`, and in `resources/dto.yaml`.
   Keep the workspace-layer result independent of the native bridge. Add the
   method enum/name in `src/bridge/bridge_protocol.h` and `.cpp`, then dispatch
   through `src/bridge/workspace_dispatch.cpp`. Accept only `character_id`;
   reject extra keys and arbitrary path parameters. Keep the operation scoped
   to the current vault, with no context change or media-release lifecycle.
6. Add the DTO alias, response guard, and method to
   `webapp/src/api/client.ts`. Map the method in
   `webapp/src/api/nativeClient.ts`. Validate the response shape and supported
   filename/MIME pairing, including `null`; leave image decoding to the
   renderer. Regenerate `webapp/src/api/schema.d.ts` from the schema.
7. Update `webapp/src/test/fixtures.ts` with a default no-picture response and
   update affected client mocks and wire fixtures. The separate ChaWeb client
   does not need a picture endpoint. Both native hosts use the shared bridge;
   add host-specific code only if the existing generic transport needs it.

Do not add picture data to bootstrap, `CharacterDetail`, session snapshots,
transcript entries, or live events. Do not add a resource server, temporary
image files, a media handle, or a cache.

## Focused verification

- Extend workspace/store tests for nested definitions, Assistant, no picture,
  unknown character, and every filename priority. When the preferred file
  contains bad image data and another format is valid, return the preferred
  file unchanged. Test reading from the stored vault after removing or
  changing the external import directory.
- Extend `tests/app/unit_application.cpp` and relevant vault tests for current
  vault reads and stale-epoch rejection. Include two vaults with the same
  character ID and different picture bytes.
- Extend `tests/bridge/unit_bridge_protocol.cpp` and
  `tests/bridge/unit_bridge_router.cpp` for method registration, exact params,
  null and picture serialization, invalid arguments, and context checks.
- Extend `webapp/src/api/nativeClient.test.ts` for request mapping, valid and
  malformed responses, null, and propagated errors. Keep fixtures and generated
  DTO types consistent. Verify existing bootstrap and snapshot fixtures gain
  no picture payload.

Run from the repository root:

```sh
npm --prefix webapp run api-types
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja --output-on-failure -R '^(WorkspaceConfigStore.*|RuntimeWorkspaceConfigStoreTest|Workspace|Application|ApplicationVault|BridgeProtocol|BridgeRouterTest)\.'
npm --prefix webapp run api-types:check
npm --prefix webapp run typecheck
npm --prefix webapp test -- src/api/nativeClient.test.ts src/api/wireFixtures.test.ts
```

## Completion and handoff

Confirm the interface above is implemented and the checks pass. Summarize any
necessary naming adjustment so block 3 uses the actual method and DTO. The
native client must be able to request a portrait without loading a session or
using any filesystem path. Image content still has no UI consumer until block 3.
