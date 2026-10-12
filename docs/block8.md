# Block 8: Pictures, clipboard, links, and Find/Replace

Status: execution guide; this document does not establish implementation,
approval, or a passing test result.

Split block 8; plan steps 13–14. Next: [block W](notes-blockW.md) in
Windows-not-available mode, otherwise [block 9](notes-block9.md).

Sources: [implementation split](notes-split.md), [full plan](notes-plan.md),
[detailed design](notes-design.md), [PRD](notes-prd.md), and
[repository instructions](../AGENTS.md). The PRD defines acceptance; for
implementation follow the plan, then design, then architecture as background.
This guide expands the split and does not replace any source step/check.
Report conflicts that this precedence does not resolve.

## Outcome and scope

Implement plan **steps 13–14**, imports/links first, then search. Deliver the
planned editing feature set through the existing atomic acceptance and history
paths. Import limits are checked at entry; the transaction boundary continues
to check total document size only. Use no picture store or second search index.

## Prerequisites and reading

Complete blocks 1–7. Require approved picker, link actions/dialog, find bar,
errors, and keyboard behavior. Read plan steps 13–14; design sections 3.3–3.5,
7.2–7.4, and 9; PRD sections 4.5, 4.9–4.10, 5.2, and 9. Use block 1's pinned
extension results. Windows shell changes/checks run now only in available mode.

## Main code paths

Use `webapp/src/notes/pictures.ts`, `notes/editor.ts`, `NotesScreen.tsx`, bundled
styles, and each available host's navigation interception. Extend existing
editor/component/native tests, not a new import framework.

## Implementation sequence

### 1. Use one shared image importer

Accept PNG, JPEG (both filename extensions), WebP, and GIF. Read actual bytes,
check magic bytes and raw size, and allow exactly **1,048,576 bytes** before
encoding. Filename/MIME alone cannot prove the format. Preserve original bytes;
encode once as a matching `data:<mime>;base64,...`, using `image/jpeg` for JPEG.
Do not resize, recompress, fetch remote data, or browser-decode to validate.
A valid header with bad pixels may display as a broken, deletable image.

Capture originating editor/context and a ProseMirror selection bookmark before
an asynchronous file read. Map it through accepted edits while pending. Recheck
origin and editable/freeze state before resolving the bookmark and inserting.
Failure/disposal/replacement changes no note; release the reader. Run one image
insertion through total-size acceptance/history. Never follow focus to another
note. The approved picker takes one file; clipboard files use this same path.

For embedded data URLs, synchronously verify Base64 decoding, decoded size,
magic bytes, and claimed MIME agreement. Reject blob/file/remote references.
Display natural image size bounded by available editor width, preserving aspect
ratio. Ordinary image selection and Delete/Backspace remove it; no resize/crop/
caption/upload controls are in scope.

### 2. Implement the existing clipboard hook sequence

In `transformPastedHTML`, preprocess a detached HTML document, then let the
normal schema parser create the slice. Preserve supported markup/links and
internal `data-pm-slice` metadata. Remove scripts/styles/unsupported embedded
objects, font colors, and non-data images without fetching. Flatten table cells
in reading order with tabs between cells and line breaks between rows, keeping
supported text/marks where possible.

Convert `<mark>` elements from every source into the single highlight mark.
Convert Word highlight styles (`mso-highlight`, and inline span `background`
or `background-color`) into that mark only in Word HTML, which has an Office
namespace or generator marker. Do not convert other web page backgrounds into
highlighting. Reuse this preprocessing for external HTML drops,
which ProseMirror already routes through it.

Use one synchronous slice-image validator in both `handlePaste` and
`handleDrop`, checking every image including internal drag slices. Invalid,
malformed, mismatched, or oversized images reject the **entire** mixed action:
return true with no dispatch or alternate-format fallback. Keep file validation
in the importer and do not scan candidate-document images.

For nonempty valid paste, use normal schema slice insertion under one history/
size action. Retain block 4's `slice.size === 0` guard and extend it with supported
fallbacks: consume the event so a selected passage cannot be deleted. If a
supported clipboard image file exists, import it once; otherwise use a
plain-text alternative once with `view.pasteText(text)` without the original
clipboard event. That reuses the text parser without HTML/file fallback
recursion. No supported content means no dispatch. Never insert both an HTML
image and its clipboard-file alternative.

Consume **every file drop**, including ones also carrying HTML/text, before
validation/insertion, with no file-import fallback. Consume empty/unsupported
non-file drops too. Valid non-file drops share slice validation, exact size,
and action history. No unchecked external/internal drop can insert images.

### 3. Apply one HTTP/HTTPS link policy throughout

Use the same predicate for parse, typing/paste/prompt autolinks, edited URLs,
loaded-link rendering, and activation. Accept valid absolute HTTP/HTTPS URLs,
preserve explicit HTTP, and prefix HTTP only for recognized bare web domains.
Never repair an explicit unsupported scheme into a web address.

Primary activation reads the stored mark's destination, not resolved DOM
`a.href`. Rendering and click handling must block damaged loaded destinations
without making the whole note unreadable. Do not rewrite loaded content or
scan unrelated text for autolinks.

Secondary click opens approved Edit link / Remove link without launching.
The toolbar Link menu gives equivalent keyboard access. Capture the full
contiguous intended mark range. Cancel/invalid URL changes nothing; valid edit
changes only its destination; remove keeps text and other marks. Isolate each
history event and use unlink autolink-prevention metadata. Saved absence of a
mark is sufficient; no persistent removed-link registry is allowed. Reopening
and unrelated edits must not recreate a removed link.

Extend each available shell to intercept HTTP and HTTPS, including child/new
windows. Validate parsed schemes before OS launch, keep the embedded note open,
and retain edits. Unsupported schemes must never reach launch APIs. Link
opening itself neither saves immediately nor changes editor history.

### 4. Connect the approved CHA find bar

Use the installed headless extension with `injectCSS: false` and initial
`searchDebounceMs: 0`. Its transient matches/current index are authoritative.
Connect search/replacement terms, case, whole word, regex, previous/next,
Replace, and Replace All. Empty replacement deletes. `$1` stays literal in both
modes. Regex clears/disables Whole word; it remains false after regex turns off.
Use tested RE2 behavior for invalid/unsupported patterns; no JavaScript fallback.

Show current/total or No matches for a nonempty zero-result term. Disable only
inapplicable navigation/replacement; empty replacement is valid. Use real labels
and an accessible close control. Matches may cross mark boundaries inside a
textblock, not blocks or hard breaks.

Scope Mod+F to editor/toolbar, retaining prompt shortcuts and the visible Find
button. Find Enter advances, Shift+Enter reverses, Replace Enter replaces one,
and Escape closes and restores editor focus. Match selection is the editor's
actual insertion selection and is shown by the inactive marker.

Wrap replacement in existing atomic acceptance and isolated history. Replace
All uses the original match set in one action, not repeated Replace dispatches.
Search-only operations do not dirty/save or change history. Closing clears
search/decorations; another note resets controls. Retaining values on reopen
within the same mounted screen needs no new store.

## Verification

Use editor fixtures and relevant frontend/native suites for:

- All image formats, exact/over-limit files and embedded URLs, bad Base64,
  MIME/magic mismatch, broken image pixels, mapped async insertion, stale/frozen
  origins, and whole-document size rejection with unchanged selection/history.
- Word/web/internal HTML, table text, supported highlights, rejected colors/
  objects, remote-image omission, slice metadata, empty/unsupported-only paste
  over a selection, text/file fallback once, and no duplicate images.
- Invalid mixed paste and internal/non-file drop rejected wholly; every file
  drop unchanged even with text/HTML alternatives; whole Undo/Redo.
- Explicit HTTP/HTTPS, bare domains, named links, final prompt URLs, blocked
  schemes and loaded invalid URLs, saved href activation, both window paths,
  primary/secondary clicks, captured link range, unlink persistence, and history.
- Literal/case/word/regex searches, mark/hard-break boundaries, pending terms,
  invalid RE2, literal `$1`, empty replacement, original-set Replace All,
  isolated Undo/Redo, near-limit atomic rejection, keyboard scope, search cleanup,
  actual selected insertion target, and no search-triggered saving.

Run affected frontend/native checks and builds from plan section 5. Verify
hooks against pinned packages rather than upstream demo behavior.

## Operator checkpoint

Provide revision, host/browser version, fixtures, and numbered expected results.
The operator copies from actual Word, a web page, and CHA, and pastes supported
clipboard image files. Check unsupported-only content over selected text, mixed
oversized content, table flattening, highlights, and complete Undo.
Insert all four image formats, save/reopen offline after removing source files,
and verify display/ordinary removal. Test file-drop no-op and valid internal
content drag. Open HTTP/HTTPS/bare-domain/named links in the system browser,
verify secondary click does not launch, then edit/remove/save/reopen. Exercise
find modes, empty/literal replacement, shortcuts, and near-limit rejection in
each available packaged host. Report each case; synthetic fixtures alone do
not establish native clipboard or system-browser behavior.

## Completion and handoff

Imports, links, and search satisfy their focused tests and operator results;
accepted actions remain offline-capable, within limits, and reversible in one
step. Record byte rules, clipboard cases, native link paths, extension behavior,
and pending Windows work. Proceed to conditional Windows work if needed, then
final vault/release verification; do not publish this build.

## Execution and reporting rules

Apply the split's [execution rules](notes-split.md#execution-rules), including
its operator checkpoint, Windows mode, and end-of-block rules, and its
[handoff contents](notes-split.md#handoff-between-blocks). Apply the scope
rules in plan section 1 and the KISS rules in the repository instructions.
