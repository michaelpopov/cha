# Character picture panel

Status: proposed design. This document does not describe an implemented feature.

## Purpose

Show the active character's picture beside the conversation in the standalone
application. The layout has three areas: the existing sidebar on the left,
the chat in the center, and an optional picture panel on the right.

The user opens and closes the picture panel with an icon, independently of
the sidebar. A predefined picture filename avoids another character setting.

## Picture files

A picture belongs to the character's definition folder, beside
`character.toml` and `CHARACTER.md`. For example:

```text
characters/seneca/
  character.toml
  CHARACTER.md
  PICTURE.png
```

Use the existing character directory lookup. Character definitions can be
nested; do not construct their paths from `characters/<id>` alone. The
built-in assistant is not in that lookup. Its definition folder is
`system/assistant/`; use the same special case as the existing character
settings code.

Recognize these exact filenames, in this order:

| Priority | Filename | MIME type |
| --- | --- | --- |
| 1 | `PICTURE.png` | `image/png` |
| 2 | `PICTURE.webp` | `image/webp` |
| 3 | `PICTURE.jpg` | `image/jpeg` |
| 4 | `PICTURE.jpeg` | `image/jpeg` |
| 5 | `PICTURE.gif` | `image/gif` |

The first existing supported file wins. Do not depend on directory iteration
order. If that file cannot be displayed, report the failure; do not silently
select another file. Unsupported files do not prevent a character or forum
from loading.

GIF and animated WebP use the webview's normal animation behavior. Render the
original image without conversion, resizing on disk, or thumbnail generation.
Pictures are optional. They are UI assets and are not added to prompts,
Markdown descriptions, or conversation history.

Use pictures of normal portrait size, for example up to about 2 MB. The first
version does not enforce a size limit. Picture rows are configuration rows, so
each settings save, merge, and assistant configuration listing reads them. The
panel also transfers the whole picture as base64 each time it loads it, and
the webview decodes the full image before it scales it. Very large files make
these operations slow and use much memory.

The first version uses the character's own folder. It does not add picture
overrides in forum member folders.

## Panel behavior

- Put a picture icon at the right end of the chat status row under the
  composer, which already holds the sidebar toggle. Its accessible label and
  tooltip are `Show picture` or `Hide picture`. Set `aria-expanded` to match
  the panel's visibility.
- Start with the picture panel open and a preferred width of 280 CSS pixels.
  Keep its open/closed preference and preferred width in the application UI
  state. Preserve both across closing and reopening the panel, navigation,
  and character, session, and vault changes during the current application
  run. Do not persist them to the vault or disk.
- Show the panel only on the chat screen. Other screens do not change its
  open/closed preference.
- Opening or closing either panel does not change the other. Changing the
  active character never opens a picture panel the user has closed.
- Drag the divider on the picture panel's left edge to resize it. Dragging
  left widens the picture panel; dragging right narrows it. Allow a preferred
  width of at least 128 CSS pixels, up to the space available after reserving
  320 CSS pixels for chat. Start each drag from the displayed width to avoid
  a jump after automatic shrinking. Resizing the picture does not change the
  sidebar's width or either panel's open/closed preference.
- The displayed picture width is the smaller of its preferred width and the
  available space beyond chat's 320-pixel reservation, never less than zero.
  When the window narrows or the sidebar widens, the picture panel shrinks
  first, including below 128 pixels and down to zero. The image scales with
  it. These automatic adjustments do not overwrite the preferred width, so
  the panel returns to that width when space permits.
- Reuse the sidebar divider's pointer capture, resize cursor, and focus
  treatment. Give the divider the accessible label `Resize picture`, a
  vertical separator role, and width values that reflect the current bounds.
  Left and Right arrow keys move the divider by 16 pixels in the same
  direction as dragging. Disable resizing when at most 128 pixels are
  available for the picture, and hide the divider when the panel is closed or
  has zero width. End a drag on pointer release, cancellation, or lost capture.
- Keep the sidebar and chat side by side on narrow windows. Below 512 CSS
  pixels, an open sidebar uses its existing 192-pixel minimum width, the
  picture panel has zero width, and chat fills the remaining width even when
  that is less than 320 pixels. With the sidebar closed, chat uses the full
  available width and the picture panel gets only space beyond chat's
  320-pixel reservation. Below 320 pixels, chat itself shrinks to fit.
- Remove the existing 430-pixel breakpoint's sidebar width and main-area
  offset overrides, which leave only `3.5rem` for chat. Let the status row wrap
  onto additional lines, keep its icon buttons at their normal size, and let
  context text shrink or wrap. Both panel toggles must remain reachable with
  mouse and keyboard. Resizing never changes either panel's open/closed
  preference; a panel reduced to zero width returns when space permits.
- Keep the picture fixed while the transcript scrolls. Center it in the
  available panel space and use `object-fit: contain` to preserve the whole
  image and its proportions. A subtle divider separates it from the chat.
- Add no panel heading, legend, biography, or explanatory helper text. Use
  the character's display name as the image's alternative text.
- A character without a picture leaves the open panel empty. Keep the toggle
  available and clear the previous character's picture immediately.
- A load or decode failure shows the concise error `Picture unavailable` in
  the panel. It does not interrupt the conversation.

## Active character

Use the existing session recipient, generation state, and transcript. Do not
introduce a separate backend concept of an active character. Apply these rules
in order; an active generation takes precedence over the other selections.

1. A session or vault change discards the previous character selection. When
   a session opens (`conversation-opened`), show its selected character.
   If its target is everyone (`*`), use the latest character reply in its
   existing transcript. If neither identifies a current forum member, show no
   picture.
2. For later snapshots of the same session, compare the old and new transcripts
   by entry ID. If newly observed entries identify a generation turn, show the
   character from the latest such turn in transcript order. Use
   `participant_id` for character replies and errors with a `request_id`, and
   `addressed_to` for human entries with a `request_id`. The human entry covers
   a generation stopped before it produced a reply. Self-notes and notices do
   not identify generation turns. Text or status changes to an existing entry,
   deletion, and covering or uncovering history do not count as a new turn.
3. When `default_character_id` changes between two snapshots of the same
   session, show the new recipient. The user or the Jev classifier can make
   this change. This overrides a turn recovered by rule 2 in the same snapshot.
   Selecting everyone does not replace the picture selected by the preceding
   rules. Selecting the recording-only target (`-`) clears it.
4. When a snapshot has an active generation with a character ID, show that
   character immediately, including during preparation or reasoning. During
   multicast, follow the character identified by the existing foreground
   generation state. Do not infer activity from audio playback or transcript
   scrolling.
5. Keep the responding character's picture after generation finishes or
   stops, until another character becomes active or the user changes the
   recipient.
6. Validate the resulting selection against the current forum members on every
   snapshot; an unknown or removed member clears the picture.

The runtime can combine generation updates, and reconnection can miss
intermediate snapshots. For example, one multicast event batch can finish A,
start B, and finish B, leaving an idle generation state in the next snapshot.
Rule 2 recovers B from the existing transcript even though the UI never saw B
as active. Compare the reducer's existing snapshot with the incoming snapshot
before replacing it. Unchanged history does not replace a later recipient
selection.

Each `session-snapshot` sets `currentDefaultCharacterId`. Apply rule 1 only
when the session opens and rule 3 only when the recipient changes. If a later
snapshot applies them again, a finished explicit `@handle` reply loses its
picture.

One temporary UI character ID is sufficient to retain the last responding
character after generation ends and to honor later recipient selections.
This state is needed because the idle generation state clears its character
ID, and the selected recipient can differ from the last speaker: an explicit
`@handle` reply does not change the recipient. Do not add a persistent
last-speaker field or a second transcript store.

## Vault storage

Character folders are also a logical structure inside the vault. The current
configuration store imports only Markdown and TOML into SQLite, and the
`config.content` column is strict `TEXT`. Reading a filesystem picture alone
would therefore lose the feature after import or when moving a vault.

Extend the existing configuration file path to preserve supported picture
files:

- The storage name check has no workspace data. It accepts a picture name
  only when its last component is one of the predefined picture names.
  Directory import accepts a picture only in a character definition folder:
  a folder under `characters/` that contains `character.toml`, or
  `system/assistant/`. Import ignores a file with a supported picture
  extension in any other name or folder, for example `picture.png`,
  `Seneca.jpg`, or a picture in a forum member folder, and writes a warning
  for each. Do not turn this change into a general attachment store.
- Store picture content as base64 in the existing `config` rows. Keep Markdown
  and TOML content as ordinary text. The recognized picture path determines
  the encoding; no extra table or encoding setting is needed. A BLOB would
  need a new or rebuilt table, because the strict `TEXT` column rejects BLOB
  values. That means a schema version change and picture handling in each
  configuration operation. Base64 also matches the bridge, which carries JSON
  text.
- Encode picture bytes at directory import and decode them at directory
  export. Keep filesystem images as normal binary files. Extend the existing
  name checks and file collection rules consistently.
- Database copies, configuration copies, merge, and character deletion already
  handle all configuration rows. They need only the extended name check.
  Character deletion removes the whole definition folder, including its
  picture. Pictures never enter assistant undo records, because character
  deletion clears undo and the assistant cannot write pictures.
- Merge copies picture files one by one, as it copies other configuration
  files. If the two vaults use different picture formats for one character,
  both files remain and the priority order selects one, so the destination's
  older picture can stay visible. Accept this rare case; the user can remove
  the unwanted file through export and import.
- Use the normal workspace reload/publication path after changes. Reuse the
  existing workspace file data to locate and select a picture; do not keep
  another authoritative asset index or persistent image cache.
- The character file editor accepts only Markdown names, so it already
  excludes pictures. Template includes can read any file inside their
  containment root, so prompt expansion must reject picture files. Exclude
  picture paths from the assistant configuration tools: `list_config`,
  `read_config`, and the write policy of `apply_config`. These paths must not
  expose base64 as a character document or accept text as a picture.

Old vaults with no pictures continue to work. The proposed encoding uses the
existing table and does not require a schema migration. Add focused byte
round-trip checks before relying on this path.

Older builds cannot open a vault that contains pictures, because their name
check accepts only Markdown and TOML. Vaults move between the desktop hosts
and the ChaWeb daemon through R2 upload, R2 download, and parent merge. Update
all hosts, including the ChaWeb daemon, before importing pictures.

Users add pictures through the vault's modify folder: export the vault, add
the file to the character folder, then import the folder. Export first clears
the modify folder. Merge copies pictures that already exist in the source
vault. An upload control, image editor, and filesystem watcher are outside
this plan.

## Native bridge and rendering

Add a narrow character-picture read operation to the application and native
bridge. Its input is a character ID in the current vault. Resolve the folder
and filename in the backend; the frontend does not pass arbitrary file paths.

Return either no picture or the selected filename, MIME type, and base64
content. The stored row is already base64, so return it unchanged; the
backend does not decode or validate it. A bad row shows `Picture unavailable`.
Request the image only when the panel is open and has an active character.
Keep picture content out of bootstrap data, session snapshots, and live
transcript events.

Render the response as an image data URL. Both native hosts already permit
`data:` images in their content security policy, so this does not need a new
resource server, temporary files, or resource-release protocol. The existing
`/media/` resource path sends raw bytes, but its in-memory handles need
release calls and cleanup rules. Check PNG, JPEG, GIF, and WebP rendering in
both native hosts.

Use the existing asynchronous loading and context invalidation patterns.
Clear the displayed image when the character, session, or vault changes,
and ignore results for a context that is no longer current. If the panel is
closed during loading, a late response must not reopen it. Closing the panel
removes its image element, which also ends its animation. Keep the image
content in the panel component, not in the application UI state. The panel
then loads the picture again when the chat screen opens, for example after a
merge or import into the same vault.

## Implementation order

1. Extend picture file recognition and configuration storage in
   `src/workspace/workspace_config_store.cpp` and
   `src/storage/workspace_session_database.cpp`. Exclude picture paths from
   template includes and the assistant configuration tools. Verify import,
   export, and merge preserve the original bytes.
2. Add character-picture lookup using the existing workspace definitions and
   file source in `src/workspace/`, then expose it through `src/app/` and
   `src/bridge/` with matching DTO and native-client types. Regenerate API
   types when the DTO changes.
3. Add the picture toggle, preferred width, and temporary character selection
   to the existing UI state and chat event paths, including recovery from
   newly observed transcript turns when generation snapshots are missed.
   Implement the panel and draggable divider in
   `webapp/src/components/ChatScreen.tsx` and its styles in
   `webapp/src/styles/app.css`. Reuse the sidebar's toggle and resize patterns
   from `webapp/src/components/App.tsx`; no shared panel framework is needed.
   Update the narrow-window sidebar rules and status-row wrapping as part of
   this step; shrinking the picture panel alone cannot keep controls usable.
4. Check the complete feature in the standalone macOS and Windows hosts.
   The separate ChaWeb interface is outside this UI change, but the ChaWeb
   daemon needs the storage change because it opens the same vaults.

## Acceptance checks

- Each supported format displays, preserves its proportions, and uses the
  documented priority when multiple files exist. Animated files animate.
- Imported and exported images retain their exact bytes, including zero and
  non-UTF-8 bytes. Database/configuration copies and merge retain pictures;
  character deletion removes them.
- Import writes a warning for each image file that it ignores.
- Existing vaults without images load normally. Missing or invalid pictures
  cannot prevent an otherwise valid conversation.
- The two panel toggles work independently with mouse and keyboard. Closing
  the picture panel remains effective across replies and navigation.
- Dragging the picture divider and using its arrow keys resize the picture
  without changing the sidebar or closing either panel. Resizing stops at the
  documented bounds and leaves at least 320 pixels for chat when space permits.
  Releasing or cancelling a drag ends resizing. Closing and reopening the
  panel, navigation, and session or vault changes preserve its preferred width.
  Narrowing the window or widening the sidebar temporarily reduces the
  displayed width; restoring space restores the preferred width. Starting a
  drag after automatic shrinking does not jump to the preferred width. The
  divider is unavailable at the documented narrow widths and hidden when the
  picture panel is closed or has zero width.
- Selected recipients, explicit addressed replies, and multicast foreground
  generation update the portrait according to the active-character rules.
  Finishing a reply retains its portrait. A snapshot with no new generation
  turn, recipient change, active character change, or member removal does not
  change the portrait within the same session.
- A multicast batch that finishes A and starts and finishes B in one snapshot
  leaves B's portrait visible. Reconnection after a missed explicit addressed
  reply also selects that reply's character. Include missed failed generations
  and generations stopped before any reply. Repeated snapshots, text or status
  updates, and transcript deletion or cover changes do not undo a later
  recipient selection. A changed recipient other than everyone wins over
  recovered history; an active generation wins over both.
- Switching sessions or vaults during image loading cannot display a stale
  picture. A character without a picture clears the previous portrait.
- Pictures stay out of prompts, prompt includes, assistant configuration
  tools, and transcript event payloads.
- Manually inspect wide and narrow windows in both native hosts: the status
  row remains usable, chat scrolls independently, and the panel has no heading
  or unnecessary copy. At widths of at least 512 CSS pixels, the sidebar at its
  maximum width leaves 320 pixels for chat and the picture panel shrinks.
  Check 512, 430, 400, and 320 pixels with both panels open: below 512, the
  sidebar stays at 192 pixels, chat shrinks, the picture has zero width, and
  the status controls wrap without clipping either toggle. Close and reopen
  each panel, then widen the window and verify that its open/closed preference
  is preserved. Also check a narrow window with the sidebar closed.

Use focused storage, bridge, and UI tests for these behaviors and run the
repository checks relevant to the implementation. Do not add timers,
background refresh, a general asset framework, or animation controls.
