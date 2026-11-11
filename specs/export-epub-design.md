# Export a notes folder to EPUB

Technical design for the native CHA Notes screen. The proposed menu and status
are shown below for review.

## 1. Export behavior

**Export EPUB…** saves a selected folder as one reflowable EPUB 3 book. Use the
folder name as the book title and the basis for the filename. In L1 or L2
mode, append ` (L1)` or ` (L2)` to the book title, for example `History (L1)`
and `History (L1).epub`. Export the notes
in that folder and its descendants. Each note becomes one chapter. Subfolders
appear as groups in the reader’s table of contents.

| Source | Export behavior |
| --- | --- |
| Ordinary notes and read-only notes | Include them as chapters. This includes chapters imported from an EPUB. |
| Saved session summaries and generated folder Summary notes | Include them as chapters. A folder’s own generated Summary note comes first in that folder. |
| Note text mode | When the selected mode is L1 or L2, use each note’s L1 or L2 text. A note without that text uses its full document, as the editor shows it. In Full text and Compressed modes, use the full document. |
| Notes marked **Exclude from summaries** | Include them. The flag controls summary generation. |
| Generated `ANNOTATIONS` items | Omit the aggregate document. Inline comments in the source note remain in its chapter. |
| Blank notes | Include a chapter with the note title. |
| Empty subfolders | Omit groups with no exportable descendants. |
| Folder with zero exportable notes | Show an error and produce no file. |

A folder containing only summary notes is a valid source. Export works offline
using the documents CHA already has. The book contains formatted text, embedded
raster pictures, and web links. The scope is the macOS and Windows Notes screen
and the content the reader can see on the page. The SQLite folder export is the
command for transferring a folder between CHA vaults.

## 2. Notes menu and feedback

Place **Export EPUB…** directly below **Export folder…** in the existing menu.
This is an excerpt of the menu for a regular folder.

```text
┌─────────────────────┐
│ Import EPUB…        │
│ Import folder…      │
│ Export folder…      │
│ Export EPUB…        │
│ Rename              │
└─────────────────────┘
```

Show the command on real folder rows, including managed folders under
**Forums** and the **Forums** folder itself. Use the same folder eligibility as
**Export folder…**, including its folder busy rules. Render the EPUB command
independently of `noteFolderTransfer`, which supplies the SQLite transfer.
Reuse the current menu admission rules, keyboard handling, and focus restoration.

On activation, close the menu and show **Exporting EPUB…** in the existing
Notes tree status position with `role="status"`. Reuse the existing `busy`
guard and add an EPUB export value to `folderTransfer`. Clear the status on
completion of preparation and download handoff, or on failure. The system Save
dialog supplies the filename, destination, overwrite confirmation, and Cancel.
Cancel closes the dialog without an app error. Use the existing Notes tree
alert for failures before handoff and the existing host error handling for
download failures.

## 3. Source selection and reads

Use `NoteItem`, `NotesClient.listNoteItems()`, `NotesClient.getNote()`, and
`NotesClient.contextEpoch()` from the current client. The TypeScript export
flow uses `notes.list` and `notes.get` through the current bridge. It builds
the EPUB entirely in the webapp.

1. In `NotesTree.tsx`, capture the folder ID, the menu’s epoch, and the
   selected note text mode. Pass `state.noteTextMode` from `App.tsx` to
   `NotesTree` as a new prop. Verify the epoch before starting. Use the same mounted-tree check used for other tree
   actions. Save the open note as described below before the first read.
2. Read the current item list once. Find the selected folder by ID in that
   list and use its current name. If the folder was deleted, end the export
   with an error. Use `buildNoteIndex()` in `webapp/src/notes/tree.ts` for
   children and ordering. Filter out `annotation` rows. Walk folders depth
   first and use the tree’s sibling order, except that the direct child marked
   `folder_summary` comes first at each level. Use the flag because an
   ordinary note may be named `Summary` and a generated one may be named
   `Summary (2)`. A saved session summary follows the normal sibling order.
3. Read notes in this order with `getNote(id, mode)`. The mode is `l1` or
   `l2` when that mode is selected, and `full_text` in all other cases.
   `notes.get` already returns the full document when a note has no text at
   the selected level. Use the same ordered traversal to produce chapter entries and table-of-contents
   groups. Assign chapter numbers in traversal order. The spine and the
   table of contents therefore agree, including when note titles repeat.
4. Check the epoch and mounted tree after the save, before and after each
   asynchronous read, and immediately before download handoff. Stop on a
   context change, connection loss, or read failure. Restore the tree’s busy
   state in `finally` and allow any deferred tree refresh to run. Ignore late
   updates to an unmounted tree.

The open editor can contain changes that autosave has yet to store. Before the
reads, call the existing `onBeforeCreateNote` save, which runs before
**New note** and keeps the note editable. **Compress**, **Summarize**, and
**Make read-only** also save the open note first. If the save fails, the
existing Retry, Stay, and Discard choice appears. When the save returns
`false`, end the export with no message. All chapters use stored documents.

Folder membership and titles come from the single list read. Each stored body
comes from its own note read. A later edit can appear if it arrives before that
read. A listed note that disappears before its read causes the export to fail
with the note name. This uses the existing read semantics without holding a
vault transaction open across requests.

### Example reading order

```text
History                       Book title
├── Summary (2)               Chapter 1, generated summary of History
├── Rome                      Table-of-contents group
│   ├── Summary               Chapter 2, generated summary of Rome
│   └── Republic              Chapter 3
├── Introduction              Chapter 4
└── Summary                   Chapter 5, ordinary note
```

## 4. Chapter XHTML and pictures

Move `renderNoteDocument()` from `webapp/src/chaweb/noteDocument.ts` to
`webapp/src/notes/noteDocument.ts` and update the ChaWeb caller and tests.
Keep its existing schema validation, `notesExtensions()` and Tiptap static
rendering, HTML sanitization, web link policy, heading handling, and embedded
picture checks. Keep the renderer’s annotation ordering for its ChaWeb caller.
The EPUB writer takes the sanitized result and performs EPUB-specific steps
in an inert `template`.

- Put the note name in the chapter’s `<title>` and `<h1>`. The shared renderer
  already suppresses a matching first heading and shifts body headings under
  the title. Preserve the remaining content, including paragraphs, lists and
  their starting numbers, code, emphasis, underlining, highlights, and alt
  text. A renderer result of `null` means the document failed validation.
  Fail the export and name the note in the error. An empty but valid document
  still produces its chapter.
- Keep explicit HTTP and HTTPS links accepted by `storedWebDestination()`.
  Keep the text of other links. Preserve the existing static formatting of
  inline comments. The renderer keeps `data-color` only on comment
  highlights. Use a small local stylesheet that styles `mark[data-color]` as
  ChaWeb does, fits pictures to the viewport, and wraps code. Use the
  reader’s default body typography.
- For each surviving `<img>`, decode its embedded data URL. In
  `webapp/src/notes/pictures.ts`, share the existing data URL decoder and
  validation between `embeddedImageError()` and the EPUB writer. Keep the
  current image signature, MIME match, and 1 MiB checks. Package the actual
  PNG, JPEG, GIF, or WebP bytes as a local resource. Keep the `alt` text and
  supply `alt=""` when the note has none. Use the detected MIME type for the
  manifest and extension. Invalid or remote images use the shared renderer’s
  alt-text fallback, or `Image unavailable` if there is no alt text. All image
  resources in the finished book are local archive entries.
- Create an XML document with an XHTML namespace root, head, title, stylesheet
  link, and body. Import the processed nodes into it and use `XMLSerializer`.
  Encode UTF-8. Before serialization, replace any remaining characters
  disallowed by XML 1.0 in text and attribute values with U+FFFD in the export
  copy. Use text nodes and attribute setters for user content. Use the same
  XML character handling for titles in the package metadata and table of
  contents. This preserves literal markup in text and code and produces XML
  syntax for void elements such as `br` and `img`.

The EPUB writer only needs the standard raster formats that CHA already
accepts. They are EPUB 3.3 core media types. Keep one archive resource per image
occurrence for the initial implementation. [EPUB 3.3 core media types](https://www.w3.org/TR/epub-33/#sec-core-media-types)

## 5. EPUB package

Implement archive construction in `webapp/src/notes/epubExport.ts`. Pass the
selected folder, item list, and checked chapter reader into the builder. Keep
menu state and download code outside it. Read and render one document at a
time. Retain the resulting XHTML and image bytes as archive entries. Keep
generated paths independent of vault IDs and user-entered titles.

```text
mimetype
META-INF/container.xml
EPUB/book.opf
EPUB/nav.xhtml
EPUB/book.css
EPUB/chapter-0001.xhtml
EPUB/chapter-0002.xhtml
EPUB/images/image-0001.png
```

| Entry | Contents |
| --- | --- |
| `mimetype` | First local ZIP entry. Store the exact ASCII bytes of `application/epub+zip` with no BOM, newline, compression, or ZIP extra field. |
| `META-INF/container.xml` | OCF container namespace `urn:oasis:names:tc:opendocument:xmlns:container` and `version="1.0"`. One `rootfiles` element containing a `rootfile` with `full-path="EPUB/book.opf"` and `media-type="application/oebps-package+xml"`. |
| `EPUB/book.opf` | OPF namespace `http://www.idpf.org/2007/opf`, `version="3.0"`, and `unique-identifier="book-id"`. Include a fresh `urn:uuid:…` in `dc:identifier`, the book title from section 1 in `dc:title`, `und` in `dc:language`, and a UTC `dcterms:modified` timestamp with seconds precision. Use the Dublin Core namespace `http://purl.org/dc/elements/1.1/`. |
| Manifest in `book.opf` | One item per chapter, image, stylesheet, and navigation document. Use unique generated IDs and relative paths. Give `nav.xhtml` `properties="nav"` and use `application/xhtml+xml` for navigation and chapters. |
| Spine in `book.opf` | One `itemref` per chapter in reading order, pointing to its manifest ID. The reader opens on the first chapter. |
| `EPUB/nav.xhtml` | XHTML with one `<nav epub:type="toc">` using the EPUB namespace `http://www.idpf.org/2007/ops`. Set the document language to `und`. Add no heading. Each chapter is a relative link in an ordered list. Represent each nonempty subfolder as a list item with a label in `span` followed by its nested ordered list. |
| Chapter XHTML | The chapter title and content from section 4. All links to the stylesheet and images are relative to the chapter. Set `lang` and `xml:lang` to `und` because CHA stores no language for a note or folder. |

Create a random UUID per export with `crypto.randomUUID()`, as
`webapp/src/xaiVoiceInput.ts` already does on the same `cha://app` origin. The
identifier does not expose the vault or folder ID. [EPUB 3.3 package and navigation requirements](https://www.w3.org/TR/epub-33/)

Use the installed `fflate` `zipSync()`. Insert `mimetype` first with
`level: 0` and use the default Deflate level for the remaining files.
`zipSync()` writes no extra field and no data descriptor, so its output meets
the OCF rules without changes. Reject sizes or entry counts that need ZIP64,
which `zipSync()` does not write. Use the existing per-note limits and keep the
EPUB import size limit specific to import. [EPUB 3.3 OCF ZIP requirements](https://www.w3.org/TR/epub-33/#sec-zip-container)

## 6. Save and failure handling

Build the complete archive before offering it for download. Use a Blob with
type `application/epub+zip` and an anchor with a `download` filename, following
the browser-download pattern in `webapp/src/download.ts`. Apply the filename
rule of **Export folder…** to the book title from section 1: replace the
characters that file systems reject with hyphens, and append `.epub`. The book
metadata keeps the title before this replacement.

Use the existing `WKDownloadDelegate` in `packaging/macos/main.swift` and
WebView2 `DownloadStarting` handler in `packaging/windows/main.cpp` to choose
the destination and save. Check the Blob download on both packaged hosts early
in implementation. The test must start the download after asynchronous note
reads, under the production `cha://app` origin and CSP. Use a real binary test
file. Verify that the Save dialog appears, the suggested filename is correct,
and the saved bytes match. Browser-only tests cannot establish this behavior.
If a packaged host cannot download the Blob, resolve that transport before
shipping.

The WebView may take the Blob after `click()` returns, so do not release the
URL right after the click as `download.ts` does. Keep the last export URL in
one module variable. When the next export creates its URL, release the
previous one. Include consecutive exports in the host test to verify that this
release cannot interrupt a transfer. A vault change before handoff cancels
preparation. A file already handed to the host follows the normal system
download lifecycle.

| Condition | Result |
| --- | --- |
| No exportable notes | `This folder has no notes to export.` |
| Folder gone | `This folder is no longer available.` |
| Note unavailable or document invalid | `Could not export the note “{name}”.` End the export before opening Save. |
| Active vault changes | Use the current vault-change error. Start a new export from the new vault. |
| Archive requires ZIP64 | `This folder is too large to export as one EPUB.` |
| Other preparation failure | `The EPUB could not be exported.` |
| User cancels Save | Close the system dialog. No file is created. |

Preparing the complete file in memory keeps failure handling in the Notes
tree until handoff. The existing hosts then control destination, cancellation,
and transfer errors.

## 7. Validation

- **Archive structure.** Unzip a fixture and parse the container, OPF, chapter,
  and navigation files as XML. Check that all local references resolve, the
  manifest covers the resources, and the spine and navigation list the same
  chapters in the same order. Check that `mimetype` is the first entry and is
  stored. Run [EPUBCheck](https://www.w3.org/publishing/epubcheck/)
  against the produced file and resolve conformance errors before release.
- **Contents and ordering.** Cover nested folders, empty branches, a blank
  note, duplicate note names in different folders, a generated `Summary (2)`
  beside an ordinary `Summary`, and a saved session summary. Check that the
  generated `ANNOTATIONS` item is absent and that a summary-only folder
  exports successfully. Test that Full text and Compressed modes export full
  documents, and that L1 and L2 modes export the L1 or L2 text, with the full
  document for a note that has no text at that level. Check that only L1 and
  L2 books have ` (L1)` or ` (L2)` in the title and the filename. Test that
  unsaved edits in the open note are saved before the reads and appear in
  the book, and that Stay in the save-failure choice ends the export without
  a download.
- **Formatting and safety.** Include Unicode and XML-sensitive names and
  text, an XML-invalid control character, code containing markup, a list
  starting at a number other than 1, a comment, a web link, an unsafe link,
  and valid raster pictures. Check picture bytes and MIME types. Confirm the
  bad-image fallback and that exported pages contain no remote resources or
  executable content. Re-run ChaWeb’s note-rendering tests after moving the
  shared renderer.
- **Workflow and failures.** Test menu availability and focus, the busy
  status, repeat activation, a missing or corrupt note, and a vault change
  while a read is pending. Verify that these failures never start a download.
  Confirm an empty folder has the specified error and that the next export
  works after a failure.
- **Packaged desktop and reader checks.** Export on macOS and Windows. Try
  Save, Cancel, and overwrite with the existing Save dialog. Open the saved
  book in Apple Books and a second EPUB reader such as Calibre. Check the
  opening Summary, nested navigation, images, links, and title. Include an
  image-heavy folder in the desktop check to assess preparation time and
  memory use. Import one exported book with CHA’s existing **Import EPUB…**
  command and check readable chapters, order, and pictures. CHA’s importer
  creates a flat folder of chapters, so this check covers reading
  compatibility and content.
