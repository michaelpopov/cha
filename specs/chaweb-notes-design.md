# ChaWeb read-only notes

Date: November 6, 2026

Status: Technical design for implementation. The folder-picker navigation is
agreed. This document does not implement the feature. Safari Reader behavior
must pass the device validation below before the feature is complete.

## 1. Purpose and decisions

Add access to existing notes in ChaWeb. Use the same navigation pattern as the
existing forum picker and session list:

1. Choose **Notes** in the bottom navigation.
2. Choose a folder from a flat, ordered picker.
3. Choose a note from that folder's list.
4. Read the document. Use the browser's Back and Forward controls to navigate.

The folder picker includes every folder that directly contains readable notes,
including folders that also contain subfolders. Show each folder's full path.
Do not reproduce a tree, indentation, expandable folders, or a folder drill-down
interface. Include **Top level** when readable notes exist outside folders.

Keep two root screens: **Chats** for forums and sessions, and **Notes** for
folders and notes. A root button opens its browsing screen, not the last open
session or note. Keep the last selected forum and folder during the current
application lifetime.

The note reading screen must work with Safari's **Show Reader** on iPhone for
article-like notes. It must also provide a complete, readable normal view for
all notes, including short notes for which Safari does not offer Reader.

## 2. Scope

Implement browsing and reading only. Include ordinary notes, saved session
summary notes, and the existing generated `ANNOTATIONS` documents. Treat both
`note` and `annotation` items as readable documents. The native `read_only`
property does not restrict visibility: every note is read-only through ChaWeb.

Preserve supported document content: paragraphs, headings, lists, inline
formatting, code, links, highlights, comments represented in the document, and
embedded pictures. Read the original document, not `compressed_text`.

Do not add note or folder creation, editing, rename, deletion, moving, imports,
exports, compression, provider operations, stores, audio generation, or audio
playback. Do not save reading positions to the vault. Do not add a search box,
sorting controls, reader settings, an application Back button, or a custom
replacement for Safari Reader.

The existing chat and vault operations retain their behavior. The work does
not change the native Notes UI, the storage format, or the database schema.

## 3. Existing code to use

| Area | Relevant implementation |
| --- | --- |
| ChaWeb composition | `webapp/src/chaweb/App.tsx` selects the session list or conversation and owns the voice hooks. |
| Folder-picker precedent | `webapp/src/chaweb/sessions.tsx` implements the forum picker with `details`, `summary`, and selectable rows. |
| Browser navigation | `webapp/src/chaweb/route.ts` parses hashes. `useChaweb.ts` handles `pushState`, `popstate`, and `hashchange`. |
| HTTP client | `webapp/src/chaweb/client.ts` implements validated JSON requests and standard errors. |
| HTTP adapter | `src/daemon/chaweb_adapter.cpp` dispatches `/api/cha/v1/` requests to `Application`. It currently has no notes routes. |
| Note reads | `Application::list_note_items(epoch)` and `Application::get_note(id, epoch)` already provide metadata and document reads. They delegate to `NotesService` and the existing store. |
| Data types | `NoteItem`, `NoteDocument`, and `NoteContent` are defined in `resources/dto.yaml`. Metadata includes `id`, `parent_id`, `kind`, and `name`. |
| Paths and ordering | `webapp/src/notes/tree.ts` supplies `noteItemPath` and `compareNoteSiblings`. These helpers do not require displaying a tree. |
| Document schema | `notesExtensions()` in `webapp/src/notes/editor.ts` defines the existing Tiptap schema. `@tiptap/core` provides static HTML generation. |
| Content safety | `webapp/src/notes/pictures.ts` validates embedded raster pictures. `editor.ts` supplies existing web-link validation. DOMPurify is already a dependency. |

The current ChaWeb CSS fixes the application to the viewport and disables
document scrolling. That layout must not contain the note article. The reading
screen needs a separate layout mode within the same application.

Do not reuse `NotesScreen` or `NotesTree` as the web screen. They include native
editing, imports, generation, context menus, and save workflows. Reuse the
small data and document-schema functions instead.

## 4. Screens and controls

### Notes root

Match the forum/session screen's spacing, typography, colors, selection mark,
and touch target sizes. The folder picker occupies the position of the forum
picker. It displays the selected path without a panel heading or subtitle.

```text
┌──────────────────────────────────────┐
│ Projects / CHA                     ▾ │
├──────────────────────────────────────┤
│ Browser reading                      │
│ Navigation ideas                     │
│ Release checklist                    │
│ ANNOTATIONS                          │
│                                      │
├──────────────────────────────────────┤
│        Chats           Notes         │
└──────────────────────────────────────┘
```

An open picker contains full paths in a flat list, for example:

```text
Top level
Projects
Projects / CHA                         ✓
Reading / History
Travel
```

`Projects` appears only if it has its own readable documents. It does not stand
for all documents below `Projects`. Selecting `Projects / CHA` shows only
documents whose `parent_id` is that folder's ID.

Use the existing picker interaction, including outside-click dismissal,
Escape, and focus restoration. Give the control an accessible name of
`Folder`. Expose the full selected path to assistive technology. Allow long
paths to wrap in the selected value and menu rows; do not depend on hover to
reveal them. Keep a picker even when it has only one eligible option so the
current folder remains visible in the same place.

Note rows show the name only. There are no audio, lock, summary, compression,
store, or management controls. Do not invent update timestamps: `NoteItem`
does not contain them. Use real links to note routes so normal browser actions,
including opening a link in another tab, remain available.

### Reading screen

```text
┌──────────────────────────────────────┐
│ Browser reading                      │
│                                      │
│ Notes remain available as ordinary   │
│ formatted documents.                 │
│                                      │
│ A section heading                    │
│                                      │
│ The document continues here, with    │
│ its paragraphs, lists and pictures.  │
│                                      │
├──────────────────────────────────────┤
│        Chats           Notes         │
└──────────────────────────────────────┘
```

The title is the document's article heading, not a panel title. Do not add a
toolbar, folder breadcrumb, Back button, read-only badge, or instructions for
using the browser. Browser Back returns to the previous history entry.

Use a single reading column, about `68ch` maximum width, centered on wide
screens. On phones, use approximately 16px side padding, 18px body text, and
1.6 line height. Keep browser zoom enabled. Images fit the column; code blocks
may scroll horizontally without making the whole page scroll horizontally.
Use the application's light/dark colors and normal selectable text.

### Bottom navigation

Show **Chats** and **Notes** as two equally sized, visibly labeled links styled
as buttons, with at least 44px touch targets. Mark the active section with the
existing selection treatment and `aria-current`. Do not use tab semantics for
links that navigate to URLs.

The navigation is available on both root screens, conversations, and note
reading screens. In the chat layout it is a final non-scrolling flex row below
the existing controls. Account for its height in the existing viewport and
keyboard layout. Preserve existing chat controls; this work does not remove
the conversation's existing session-list action.

Chats root:

```text
┌──────────────────────────────────────┐
│ Forum name  Character · Character  ▾ │
├──────────────────────────────────────┤
│ Session title                  Nov 6 │
│ Session title                  Nov 5 │
│                                      │
│ New Session                          │
│ Vault action icons                   │
├──────────────────────────────────────┤
│        Chats           Notes         │
└──────────────────────────────────────┘
```

Conversation, keyboard closed:

```text
┌──────────────────────────────────────┐
│ Transcript entries                   │
│                                      │
├──────────────────────────────────────┤
│ Message                              │
│ Existing composer controls           │
├──────────────────────────────────────┤
│        Chats           Notes         │
└──────────────────────────────────────┘
```

Conversation, message editor focused on a touch device:

```text
┌──────────────────────────────────────┐
│ Transcript entries                   │
├──────────────────────────────────────┤
│ Message                              │
│ Existing composer controls           │
├──────────────────────────────────────┤
│ Keyboard                             │
└──────────────────────────────────────┘
```

On touch devices (`pointer: coarse`), hide the navigation row while the
message editor has focus, so the composer stays directly above the keyboard.
Use a CSS rule on that focus. Do not add keyboard-detection state. On other
devices the row stays visible.

In the reading layout, keep the navigation at the bottom of the viewport,
outside the article. Reserve its height plus the safe-area inset below the
article so it cannot cover the last paragraph. There is no animation or
hide-on-scroll behavior. Safari Reader is expected to exclude this navigation.

## 5. Folder options and document lists

Fetch one complete metadata list through the new notes-list endpoint. Derive
the picker and selected folder's rows from that list. No folder-list endpoint,
server-side navigation index, recursive requests, or persisted derived data is
needed.

Use the following algorithm:

1. Identify readable items: `kind === 'note' || kind === 'annotation'`.
2. Collect their distinct immediate `parent_id` values.
3. For each non-null parent, resolve the folder and its ancestor names from
   the same metadata with `noteItemPath`. Use the full path joined with ` / `
   as its label.
4. If a readable item has `parent_id === null`, add the synthetic option
   `Top level`. This is a UI option, not a stored folder. Give it a separate
   `root` key; never send zero as a note or folder ID.
5. Put `Top level` first, when present. Sort real folders by full path using
   the existing locale comparison and deterministic string fallback. Use the
   numeric folder ID as the final tie-breaker for equal labels.
6. Filter the note list by exact `parent_id`. Use `compareNoteSiblings` for
   alphabetical note ordering and the existing convention that annotations
   follow ordinary notes. Use ID as the final tie-breaker.

Keep selection keyed by ID, never by label or array position. Equal display
paths remain distinct options. Path separators in names are display text;
never parse a displayed path to obtain identity.

A temporary map from ID to metadata is sufficient to resolve paths efficiently.
Do not introduce a second long-lived copy of the hierarchy. Recompute options
when the metadata response changes, using memoization only if useful.

Use `noteItemPath` as it is. The store prevents a missing parent, a
non-folder parent, and a cycle: foreign keys are on, item creation checks that
the parent is a folder, and there is no move operation. Do not add a separate
hierarchy error state.

### Initial and restored selection

- An explicit valid folder route wins over remembered selection.
- On entry through the Notes root button, restore the last eligible folder.
- Otherwise choose the first sorted option and canonicalize the URL with
  `replaceState`.
- If an explicitly requested folder is absent or no longer eligible, stay in
  Notes and show `That folder is no longer available.` Do not silently display
  another folder under the old URL. The picker can select another valid folder.
- If there are no readable items, show `No notes.` Hide the empty picker.

An ordinary empty note remains a list item. Opening it shows its title and the
state `This note is empty.`

## 6. URLs and browser history

Extend the existing hash router. Keep the existing chat URLs compatible.

| URL fragment | Meaning |
| --- | --- |
| empty or `#/` | Chats root, with existing forum selection behavior. |
| `#/forums/{forum}/sessions/{session}` | Existing conversation route. |
| `#/notes` | Notes root; resolve remembered/default folder after metadata loads. Also the empty-notes screen. |
| `#/notes/folders/root` | Notes root with Top level selected. |
| `#/notes/folders/{folderId}` | Notes root with the specified folder selected. |
| `#/notes/items/{noteId}` | One readable document. |

Use positive safe-integer decimal IDs, matching `NoteId`. Reject zero, signs,
fractions, overflow, malformed encoding, and extra path segments. Generate
canonical decimal URLs. Names and paths do not appear in route identity, so
renaming a folder or note does not break a link. IDs refer to the active vault;
the URL is not a cross-vault or public sharing identifier.

| User action | History behavior |
| --- | --- |
| Select another root | Push its root route and apply it once. |
| Select another folder | Push that folder's list route. |
| Open a note | Push its item route. |
| Select Notes while reading a note | Push the last folder list route, or the note's containing folder on a direct entry. |
| Select the root/folder already displayed | No-op; do not add duplicate entries. |
| Resolve a default folder from `#/notes` | Replace the current entry; do not create a synthetic Back step. |
| Browser Back or Forward | Apply the destination route without pushing or replacing it, except necessary default canonicalization. |
| Reload or open a direct note link | Load that document without requiring a previous visit to the list. |

A normal sequence is:

```text
Chats → Notes: Reading → Notes: Projects / CHA → Browser reading
                                                   ← browser Back
```

Back from the note restores the last folder list. A second Back restores the
previous folder selection. Do not implement a separate application history
stack or intercept the iPhone edge-swipe gesture.

Two existing chat screens have no history entry of their own. This behavior is
accepted; this work does not change chat navigation:

- A new session that is not sent yet uses the Chats root URL. New Session →
  Notes → Back shows the Chats list. The unsent text is kept, and New Session
  in the same forum shows it again.
- The conversation's Sessions button shows the list but keeps the
  conversation URL. Sessions → Notes → Back opens the conversation again.

Extend the existing history owner in `useChaweb.ts`; do not add another
independent `popstate` handler with competing routing decisions. Update
`sameChawebPlace` to compare note and folder identities. A `pushState` call
does not emit `popstate`, so explicit navigation must still apply the route.
The existing `hashchange`/`popstate` deduplication must handle both events.

Preserve note-list scroll position in the current history entry, under a small
namespaced `history.state` field. Merge with any existing state. Write it once,
when an application action leaves the list: opening a note, selecting another
folder, or selecting a root button. Do not write it on scroll events. Safari
throws a `SecurityError` when a page calls `pushState` and `replaceState` more
than 100 times in 30 seconds, and later navigation then fails. When browser
Back or Forward leaves a list, its offset is not saved. Restore the offset
after the destination list is rendered, clamped to its current size. Do not
keep a separate folder-scroll cache or serialize note contents into history.

Notes start at the top, including on Back and Forward. ChaWeb does not keep
the reading position: each entry loads the document again, so the browser's
own scroll restoration occurs before the article exists. Do not read or write
the native `scroll_top` value.

## 7. HTTP contract

Add only two GET routes under `/api/cha/v1`:

| Method and path | Result |
| --- | --- |
| `GET /notes` | `{ "items": NoteItem[] }`, using the existing `NoteItemList` shape. |
| `GET /notes/{id}` | `{ "item": NoteItem, "document": NoteDocument }` for a note or annotation. |

The detail response includes metadata so a direct URL can supply the title and
parent folder without first downloading the whole hierarchy. An `annotation`
document also needs the metadata list to order its groups; see section 9. Define the web
response as a small composition of the existing TypeScript DTO types; no
storage type or database change is needed. Do not include compressed text,
native scroll position, audio bytes, credentials, or provider configuration.

For the list, capture `application.context_epoch()` and call
`application.list_note_items(epoch)`. For a detail request, capture one epoch,
find the item's metadata with `list_note_items(epoch)`, check its kind, and call
`get_note(id, epoch)` with the same epoch. The existing metadata list is an
adequate source for this small application; do not add an index or new lookup
service solely for this endpoint. If the vault changes between calls, the
existing admission checks reject the stale epoch.

Use the existing JSON response writer and error envelope. Missing IDs and
folder IDs requested as documents return `404`. Reject malformed IDs during
route parsing. Preserve existing mappings for unavailable application (`503`)
and changed vault context (`409`). Unsupported methods and paths return `404`,
as elsewhere in the adapter. In particular, POST, PUT, PATCH, and DELETE must
not expose note mutations.

Do not gate either read on compression, audio, cloud-store, or provider settings.
Unused configuration must not prevent reading. Use existing warning logging
where relevant; do not add provider validation to these routes.

Add `listNoteItems(signal?)` and `getNote(id, signal?)` to `ChaWebClient`. Reuse
`exchange` for timeout, cancellation, JSON content-type checks, and errors.
Validate the wrapper, IDs, kinds, metadata, and document object. Validate that
the detail response ID equals the requested ID. Share the existing metadata
guard with the native client by moving/exporting the small guard if needed;
do not import `nativeClient.ts` into the web client.

The nginx API location already routes this prefix and applies `no-store`.
The Vite proxy also covers it. No extra service, HTML endpoint, rewrite, or
deployment route is part of this design. The existing request-body limit does
not limit a GET response; large saved documents must remain readable.

## 8. Client ownership and request lifecycle

Extend the screen discriminant to cover `list`, `conversation`, `notes`, and
`note`. Keep one route owner and the existing chat state alive across all four
screens. In particular, do not remount the whole application or discard unsent
chat drafts when Notes opens.

Keep note loading in a small hook, such as `useChawebNotes.ts`, whose input is
the current notes route. It owns only:

- The latest metadata list and its loading/error state.
- The currently requested document and its loading/error state.
- Request cancellation/sequence markers needed to reject late responses.
- The last selected folder ID or `root` during this application lifetime.

Folder options and visible rows are derived data. The URL is authoritative for
the visible route. There is no all-notes content cache, service worker,
IndexedDB, expiry timer, background refresh loop, or second history store.

Fetch metadata on entry to a notes-list route, including browser history entry
and return from a note. Also fetch it on entry to an `annotation` document
route when no metadata list is loaded. Existing same-vault metadata may remain
visible while that read runs. Fetch the document on entry to its route, including direct
load, Forward, and Retry. Do not keep the previous document visible under the
new document's URL while loading.

Abort obsolete reads when the destination changes. Also check the request
sequence before applying a result, since abort alone does not prevent a
response that has already completed. A late success
or failure must not replace the current screen or its error state.

Do not poll notes, refresh an article on a timer, or replace it when the browser
regains focus. The displayed document is a snapshot until it is reopened or
the page is reloaded. This avoids disturbing reading, selection, and Safari
Reader. Returning to a list gets current metadata without a new refresh
control.

`refreshVault` in `useChaweb.ts` is the existing reset path for a local vault
download or merge. The vault actions are only on the Chats root, and leaving
Notes already aborts note reads. The notes hook exposes one `reset()` function,
and `refreshVault` calls it. It clears the metadata and document, forgets the
remembered folder, and advances the same request sequence that route changes
use. Do not add a separate vault generation. Do not carry note content into a
new vault even when numeric IDs coincide. A stale
read rejected with `409` uses the existing bootstrap/context recovery path;
never label old content with a new response's metadata.

### Interaction with chat activity

Equality checks such as `screen === 'conversation'` already exclude the new
screens. Change the places that assume only two screens:

- `App.tsx` renders `Conversation` for every screen that is not `list`.
- `buildCommand` in `useChaweb.ts` types the screen as `'list' | 'conversation'`.
- `deletedOk` in `useChaweb.ts` calls `showList()` without a screen check. Make
  it switch to the list only while the deleted conversation is on screen. A
  delete that completes after the user selects Notes must not show Chats under
  a Notes URL.

Notes navigation must invalidate obsolete conversation reads and pause
conversation polling, as leaving a conversation already does. Visibility
events and late session responses must not switch back to Chats.

Stop local microphone capture and playback when leaving the conversation;
disable automatic audio for the departed screen. A dictation completion must
not send a prompt after Notes is selected. Preserve the current draft. Do not
send a server Stop command or cancel an already accepted generation merely
because the user is reading a note. On returning to a conversation, use the
existing snapshot/recovery path.

## 9. Static document rendering

Render ordinary HTML from the stored Tiptap JSON. Do not instantiate an Editor,
render a `contenteditable` surface, or attach editing, saving, import, audio,
or generation hooks.

Use the existing `notesExtensions()` schema with Tiptap's static serialization
facilities (`getSchema`/ProseMirror validation and `generateHTML`). Validate the
document before serialization, including its root type and schema content.
Use the schema as the single definition of supported nodes and marks. Do not
add a second handwritten JSON-to-HTML renderer. The current nonvisual editor
extensions do not require mounting an Editor; extract a shared schema function
only if a concrete integration problem requires it.

After serialization, sanitize the output with a dedicated DOMPurify allowlist
for notes. Do not use the chat Markdown renderer: it removes most links and
images, and the note is already structured JSON. Sanitation must retain the
supported semantic elements while dropping executable attributes and markup.

Rendering rules:

- Escape the note name through React text rendering. Set `document.title` to
  `<note name> — CHA` while the note is visible and restore the app title when
  it is left.
- Render the name as the article's main `h1`. If the document's first block is
  a heading with the same normalized plain text, omit that duplicate from the
  displayed body only. Place remaining document headings below the article title in the
  heading hierarchy, capping levels at `h6`. Do not change saved content.
- Preserve paragraphs, line breaks, lists and ordered-list start values,
  emphasis, bold, underline, inline code, code blocks, and highlights supported
  by the existing schema.
- Strip all inline styles. Keep the highlight `data-color` attribute only for
  the saved-comment color. Style ordinary and saved-comment highlights with
  theme-aware `mark` and `mark[data-color]` selectors, as the native editor CSS
  does. Text remains readable without the colors.
- Validate link destinations with the existing stored-web-link rules. Keep
  allowed HTTP/HTTPS links as ordinary anchors; remove a disallowed destination
  while keeping its visible text. Use normal browser navigation. Do not allow
  scripts, native bridge URLs, or arbitrary custom protocols.
- Validate embedded picture data with `embeddedImageError`. Preserve valid
  PNG, JPEG, WebP, and GIF images, including their alt text. Use real `img`
  elements, not background images or canvas. A failed picture leaves an alt
  text or `Image unavailable` placeholder without hiding the article.
- Show a picture only when `embeddedImageError` accepts its `src`. Imports
  embed pictures. Any other `src`, including an external URL, uses the
  unavailable placeholder. Do not load external pictures or add a daemon proxy.
- The native `annotationSource` node serializes to an empty span. Omit that
  non-text source-jump control in ChaWeb, retaining annotation headings and
  all comment/highlight text. No precise highlight-jump workflow is added.
- Before serialization, order the groups of an `annotation` document with the
  existing `sortAnnotationGroups(document, items)` from `annotationSource.ts`,
  as the native Notes screen does. Use the loaded metadata list as `items`.
- Invalid or unsupported document structure produces `This note cannot be
  displayed.` rather than silently dropping unknown content or modifying it.

Only sanitized, derived HTML reaches `dangerouslySetInnerHTML`. Keep loaded
article HTML stable across unrelated state changes; root navigation state,
chat acknowledgments, and image events must not regenerate the article body.

## 10. Safari Reader compatibility

Apple documents Show Reader for supported websites and says Reader displays
the relevant text and images. It does not promise that every document qualifies
or provide a website control to enable it. Safari decides whether a page is
eligible. See [Apple's iPhone Reader guide](https://support.apple.com/guide/iphone/hide-distractions-when-reading-iphdc30e3b86/ios)
and [Apple's explanation of article availability on Mac](https://support.apple.com/guide/safari/hide-distractions-when-reading-sfri32632/mac).

The following are design measures to make the actual note page suitable for
article extraction, not a claim that Apple guarantees acceptance of this HTML:

```html
<main class="chaweb-note-main">
  <article aria-labelledby="chaweb-note-title">
    <header><h1 id="chaweb-note-title">Note name</h1></header>
    <div class="chaweb-note-body">
      <!-- The complete sanitized document: headings, paragraphs, images, etc. -->
    </div>
  </article>
</main>
<nav aria-label="Main navigation">
  <!-- Chats and Notes root links, outside the article. -->
</nav>
```

Requirements:

1. Mount one visible article for the current note in the main document DOM.
   Unmount the list and transcript while reading. Do not put the article in an
   iframe, shadow root, dialog, textarea, or editor widget.
2. Render the entire document after its single read completes. Do not paginate,
   virtualize, collapse sections, or load paragraphs as the user scrolls. Do not
   mark the article with `content-visibility: auto` or hide offscreen text.
3. Give the reading route natural document height and window scrolling. In
   this mode, remove the current global `overflow: hidden`, fixed application
   positioning, visual-viewport height, and nested vertical scroll container.
   Scope the mode and restore the existing chat layout on exit.
4. Keep the full prose and actual image elements visible in the normal page.
   Do not create a hidden duplicate article intended only for Reader.
5. Update the article and document title together when the route resolves.
   Clear the old article immediately on navigation so Safari cannot extract
   the preceding note while the new note loads.
6. Do not inject artificial paragraphs, padding text, dates, or author metadata
   to influence eligibility. Do not guess Reader's word-count threshold.
7. Keep navigation and loading/error messages outside the article. There is
   no application Show Reader button or simulated Reader-mode indicator.

### Delivery choice and required early check

Start with the existing same-document hash navigation and static HTML rendering
above. This preserves chat drafts and uses the current deployment. Static DOM
rendering here means rendered HTML without an editor; it does **not** mean the
initial server response already contains the article.

Before completing the rest of the feature, validate a thin vertical slice on
an actual iPhone with Safari: a long representative note, the actual layout,
entry from a list, direct URL load, and a second note opened through Back.
This explicitly tests the uncertainty around Reader reevaluation after
asynchronous content and same-document navigation.

If Show Reader is unavailable for the representative article, extracts stale
content, or requires a manual reload after list navigation, this delivery
choice has failed. Do not declare compatibility from semantic tags, a desktop
test, or a WebKit automation run. Resolve the observed navigation/rendering
problem and update this design before proceeding with a different delivery
mechanism. In particular, a separate server-rendered article route is a
possible remedy only after evidence requires it; it would need its own review
of browser history, deployment, rendering reuse, and unsent chat drafts.

This is a release gate, not permission to ship a Reader-incompatible screen.
Do not build parallel reader endpoints or persistence machinery in advance.

## 11. Loading and errors

| State | Display and behavior |
| --- | --- |
| Metadata is loading with no existing list | `Loading` in the list area. Root navigation stays available. |
| No readable notes | `No notes.` No picker or management action. |
| Metadata request fails | A concise error and Retry. Existing same-vault rows may remain, with the error visible. |
| Requested folder no longer exists or contains no readable items | `That folder is no longer available.` Keep the other folder options usable. |
| Note is loading | `Loading` outside an article. Never show another note's body. |
| Note was deleted or the ID names a folder | `This note is no longer available.` Root navigation and browser Back remain usable. |
| Transient detail request failure | A concise error and Retry for the same route. |
| Invalid document | `This note cannot be displayed.` Preserve navigation; do not offer editing or repair. |
| Empty document | The title and `This note is empty.` |
| Image fails | Preserve all other content and use the image fallback. |

Use existing error styles, `role="alert"` for errors, and a polite status region
for loading. A route failure must not redirect to Chats, silently open a
different note, or repeatedly retry in the background.

## 12. Implementation boundaries

| File or area | Planned change |
| --- | --- |
| `src/daemon/chaweb_adapter.cpp` | Parse and serve the two GET routes using existing note application reads. |
| `webapp/src/chaweb/client.ts` | Add web note response types, guarded reads, and optional cancellation. |
| Shared API guards and `nativeClient.ts` | Share only the small existing note metadata validation if needed. |
| `webapp/src/chaweb/route.ts` | Add Notes list/folder/item parsing, URL builders, and place comparisons. |
| `webapp/src/chaweb/useChaweb.ts` | Extend the route/screen owner; coordinate chat suspension and vault resets. |
| `webapp/src/chaweb/useChawebNotes.ts` | Load current metadata/document and reject stale results. |
| `webapp/src/chaweb/notes.tsx` | Folder picker and read-only document list. |
| `webapp/src/chaweb/note.tsx` | Article view and its loading/error states. |
| `webapp/src/chaweb/noteDocument.ts` | Schema-based static serialization, URL/image checks, and sanitized presentation. |
| `webapp/src/chaweb/App.tsx` | Compose four screens and the shared bottom navigation without remounting chat state. |
| `webapp/src/chaweb/styles.css` | Picker/list styling, bottom navigation, and scoped document-scrolling reader layout. |

These are small module boundaries, not a request for a generic navigation
framework or reusable document platform. A short navigation component may
stay in `App.tsx`. Reuse the forum picker's interaction pattern without
refactoring unrelated chat functionality.

Do not change files in `docs/` as part of implementation unless separately
requested. This specification is the design record.

## 13. Validation and completion criteria

### Automated coverage

Use focused tests for behavior that can fail:

- **Metadata derivation:** parent folders with their own notes; nested paths;
  Top level; grouping-only folders excluded; empty notes retained; annotations
  and summaries; ordering; equal labels; long and non-Latin names.
- **Routes/history:** all new routes; invalid numeric IDs; direct reload;
  Back/Forward through folders and notes; no duplicate active-root entries;
  default-folder replacement; restored list scroll after a note is opened and
  closed; no history writes during list scrolling; existing session URLs.
- **API:** successful list/detail; detail metadata and document belong to the
  requested ID; missing item; folder-as-note; invalid IDs; unsupported mutation
  methods; vault-context errors; valid large note responses.
- **Read-only behavior:** opening, reading, navigating, and retrying issue no
  note mutation, scroll-save, compression, store, audio, or provider requests.
  Unused provider settings do not block reads.
- **Rendering:** supported rich content; embedded pictures; unsafe links,
  image data, and external picture URLs; escaped titles; unsupported nodes;
  empty notes; annotations in native group order; no editor or editable DOM;
  complete content in one semantic article.
- **Request races:** open A then B; leave while a read is pending; Retry after
  failure; return to the list; reset the vault with a request in flight. Late
  results must not change the active document, title, or section.
- **Chat regression:** unsent drafts survive notes navigation; late chat
  responses do not steal navigation, including a session delete that
  completes after Notes is selected; dictation cannot submit after leaving;
  local audio stops; server generation continues; returning refreshes the
  conversation; existing Welcome and vault flows still work.

Extend the existing ChaWeb unit/component suites and
`tests/daemon/unit_chaweb_adapter.cpp`. Cover the new HTTP reads and no-write
behavior in `tests/integration/daemon_integration_test.py`, using a temporary
vault and the existing test deployment. Do not use real providers for this.

Relevant validation commands from the repository root:

```sh
npm --prefix webapp run check
npm --prefix webapp run build:chaweb
make test-daemon
make itest-daemon
```

Run the focused tests during development, then the applicable checks above for
the completed change. A documentation-only change does not require these builds.

### iPhone Safari acceptance

Record the tested device, iOS/Safari version, and results. Use the deployed
HTTPS ChaWeb interface with representative saved notes. Test at least:

1. Open a long prose note from a folder list. Show Reader becomes available
   without manually reloading the page.
2. Show Reader displays the correct title and complete prose in order. It
   includes supported images and excludes folder controls, root navigation,
   chat content, and loading/error text.
3. Hide Reader, use browser Back, and confirm the selected folder and list
   position. Use Forward and confirm the correct note.
4. Open note A, return, then open note B. Reader displays B, including after
   asynchronous loading; it never retains A's title or body.
5. Open a note URL directly and reload it. Test Safari's automatic Reader
   setting as well as manual Show Reader. For eligible articles, automatic
   Reader must not show an empty shell or an earlier document.
6. Test a long note with headings, lists, links, code, highlights, and embedded
   pictures. Normal mode preserves supported content. Reader may restyle it;
   verify that the main reading content remains complete.
7. Test a short note, an empty note, and an image-heavy note. The normal reader
   remains usable even if Safari does not offer Show Reader. Do not add fake
   content to make these qualify.
8. Verify portrait/landscape layout, zoom, text selection, browser edge-swipe
   navigation, bottom safe areas, and the last paragraph above navigation.
9. Switch between Chats and Notes, including a conversation with an unsent
   draft. Confirm draft retention and correct chat keyboard/composer layout.
   Expect the accepted chat Back behavior from section 6.

Automated browser tests can verify DOM, history, requests, and layout, but do
not substitute for Safari's actual Show Reader UI. If a device check cannot be
performed, report that check as unverified and do not mark Reader compatibility
complete.

## 14. Delivery order

1. Add the minimal GET detail path and semantic reading slice. Run the early
   Safari compatibility check before expanding the interface.
2. Add metadata loading, folder-option derivation, and the note list.
3. Add the two root buttons and finish route/history integration, including
   chat draft retention and vault invalidation.
4. Complete document sanitization, supported content, and error behavior.
5. Run automated regression checks and the full iPhone Safari acceptance set.

The feature is complete when the agreed folder/list navigation works, browser
Back and Forward work, note access is read-only, existing chat behavior is
preserved, and representative article notes pass Safari Reader validation.
