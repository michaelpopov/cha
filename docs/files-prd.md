# Note and folder references in Files

Status: PRD draft with UI design approved by the user on October 30, 2026,
after review of the interactive mockup in this chat. The user also confirmed
that captured names stay fixed: remove and add an entry again to refresh its
name. This document does not implement the feature.

## Purpose

Extend the existing **New file** panel for Forums and Characters with two ways
to add an entry:

1. A reference to a note in the active vault.
2. A reference to a notes folder that has been uploaded to an OpenAI or Google
   (Gemini) file store.

The user enters the item's CHA Ref ID. CHA finds the item and stores its current
name, reference type, and Ref ID under the selected Forum or Character. The name
appears in that owner's existing file list.

These entries contain reference metadata only. This phase does not read note
text into a prompt, attach folders to conversations, or grant models access to
file stores. How saved references are used will be specified separately.

## Existing behavior and terms

- **Upload file** and **Write text** already share a New file panel. Character
  uploads support text and pictures; Forum uploads currently support text.
  Preserve these existing capabilities and restrictions.
- Notes and real folders already have vault-local CHA Ref IDs, such as
  `##00000A`, available through **Copy Ref ID**. Use this existing format and
  its case-sensitive encoding. A provider's remote store ID is not a CHA Ref ID.
- Folder uploads already have a local store record, with service `openai` or
  `google` and state `incomplete` or `ready`. A folder has at most one store
  association today. Reuse this information.
- A **reference entry** is an item in a Forum's or Character's file list. Its
  displayed “filename” is a captured name, not a filesystem path.
- The **owner** is the Forum or Character whose New file panel is open. A
  Character reference belongs to that Character's definition, not to one
  membership in a Forum or to one conversation.

## Required behavior

### Add a note reference

1. Open New file for an eligible owner and select **Note reference**.
2. Enter one CHA Ref ID, normally pasted from the Notes menu.
3. Select **Add file**.
4. CHA validates the ID and finds the note in the active vault. It captures the
   note's current name and saves the reference under the selected owner.
5. Return to that owner's file list. The saved entry displays the captured name.

A note does not need to be uploaded or belong to an uploaded folder. An empty,
editable, or read-only note is eligible: this phase stores no content. Include
readable note items already supported by CHA's note-reference path, including
summary and annotation notes; do not introduce a separate note eligibility rule.
A folder ID entered in this mode is an error.

### Add a folder reference

Use the same steps with **Folder reference**. At save time, the ID must resolve
to a real folder with its own local `ready` store association for OpenAI or
Google (Gemini).

- A folder with no store or an `incomplete` upload is ineligible.
- An uploaded ancestor does not make a child eligible. An uploaded child does
  not make its parent eligible. Check the exact referenced folder.
- A note ID entered in this mode is an error.
- Read the existing local store state. Do not contact a provider, verify remote
  availability, require a model selection, or revalidate API credentials.
- Do not upload a folder as part of adding its reference. The user must first
  use the existing **Upload to store** action.

The selected owner's model provider does not restrict which ready folder can
be recorded. Provider compatibility belongs to the later usage phase.

### Capture once; keep independently

Save the name exactly as it exists when the add operation succeeds. Do not let
the user supply an alias or edit the saved name or Ref ID. To refresh or change
an entry, remove it and add the desired reference again.

| Later source change | Saved reference entry |
| --- | --- |
| Note or folder is renamed or moved | Keep the captured name, type, and Ref ID. |
| Note content or folder contents change | Make no change. No content is stored in the entry. |
| Note or folder is deleted | Keep the entry, including its name and Ref ID. |
| Folder store is removed, becomes incomplete, or is replaced | Keep the entry. Do not refresh or remove it. |
| Owner is renamed | Keep its entries attached to the same owner. |
| Owner is deleted | Remove its reference entries with its other owned configuration. |
| Reference entry is removed | Leave the source item and its remote store untouched. |

There is no referential constraint, source-delete hook, automatic rename,
background validation, stale badge, or automatic cleanup for these entries.
Eligibility is checked when adding, not when listing or opening a saved entry.
Showing an entry later must work even when its source no longer exists.

## Approved UI

The user approved the interactive UI proposal on October 30, 2026. The layout
and flow below record that approval. No further UI design approval is needed
to implement this design.

Use the existing panel styling, back navigation, and action buttons. Keep
**Upload file** and **Write text**, and add **Note reference** and
**Folder reference** in the same action group. Retain Upload file as the
initial mode. Preserve existing visibility restrictions on Write text.

Upload file remains an action that opens the file chooser. Clicking it does
not change the current mode or its input. Successfully reading a selected
picture activates upload mode; successfully reading selected text activates
Write text, as today. Cancelling the chooser leaves the current mode and input
unchanged, including when a reference mode is active. A failed selection or
read also leaves the mode unchanged and uses the existing upload error handling.

Place the action group before the mode-specific fields so reference modes can
replace Filename and Content with one Ref ID field. Arrange the actions in two
rows: Upload file and Write text above Note reference and Folder reference.
Allow labels to wrap at narrow widths. Match the approved mockup's active-mode
styling: a filled primary button with contrasting text for the active mode,
and plain outlined buttons for the other modes. Set `aria-pressed="true"` only
on the active mode's button and `false` on the others. Upload file no longer
keeps its permanent primary styling: it is highlighted only in upload mode.
After a text upload, Write text is highlighted. Opening and cancelling the
chooser does not change the highlight.

Reference mode, shown inside the existing application shell:

```text
  ‹ Files

  [ Upload file ] [ Write text ]
  [ Note reference ● ] [ Folder reference ]

  Ref ID
  [ ##00000A                                  ]

                         [ Cancel ] [ Add file ]
```

Folder reference mode uses the same layout with Folder reference selected.
Do not add a panel heading, legend, provider picker, filename field, text editor,
preview lookup button, or explanatory helper text to these modes. Resolve the
name during Add file; no live lookup or separate confirmation step is needed.

Upload and text modes retain their existing fields and behavior. Switching
modes alone saves nothing. Add file submits only the active mode. Keep unsaved
upload/text input while switching modes within the panel, following the current
form behavior; clear reference validation errors when the mode or Ref ID changes.
Both reference modes share one Ref ID value. Changing between them keeps that
value, so a user can correct a wrong-kind error without pasting again. Keep it
separate from Filename and retain it when visiting upload or text mode. Leaving
the panel discards its unsaved input.

### Save, cancel, and errors

- Disable Add file for an empty or whitespace-only Ref ID and while saving.
  Disable conflicting input and repeated submission while saving; use the
  existing **Adding…** state.
- Enter submits the active form. Cancel and back navigation discard unsaved
  input and return through the existing owner-specific navigation path.
- On validation or storage failure, keep the form open with the entered ID and
  an inline error. Do not create a partial entry or change an existing entry.
- On success, return to the owner's file list and show the saved entry. Do not
  open the source note or start a model request.

Example errors, using the existing error treatment:

| Condition | Message |
| --- | --- |
| Malformed ID, URL, remote store ID, or multiple references | `Enter one valid CHA Ref ID.` |
| No matching item in the active vault | `No item with this Ref ID exists in this vault.` |
| Folder supplied in Note reference mode | `This Ref ID belongs to a folder. Select Folder reference.` |
| Note supplied in Folder reference mode | `This Ref ID belongs to a note. Select Note reference.` |
| Folder has no store or its upload is incomplete | `Upload this folder to a store before adding its reference.` |
| Same reference already saved under this owner | `This reference is already in this file list.` |
| Save fails | `The reference could not be added. Try again.` |

### File list and saved entry

Show reference entries alongside existing files. Their primary text is the
captured name. Use a note-reference or folder-reference icon with an accessible
type name so users can distinguish references from actual files. Reuse existing
icons where possible. Keep New file first and actual files in their existing
order. Place references after actual files, sorted by captured name using the
existing filename comparison, with reference type and Ref ID as tie-breakers.
Do not add group headings or change the file-list row layout.

```text
  +  New file                                      ›
     background.md                                ›
  ↗  Chapter 1                                    ›
  ▣  Research                                     ›
```

Selecting a reference opens a small read-only entry detail using existing
navigation and action patterns:

```text
  ‹ Files

  Chapter 1

  Ref ID
  ##00000A

                                      [ Delete ]
```

The captured name and Ref ID must be selectable text. The detail must remain
readable when the source is deleted. No text editor, rename control, source
navigation, current provider status, or source-content preview is required.
Use the existing delete confirmation pattern, with text such as
`Delete the reference to “Chapter 1”?` Deletion removes only this entry.

Names need not be unique. Two distinct notes named Chapter 1, a note and folder
with the same name, or a reference and an actual file with the same displayed
name may coexist. Entry selection and deletion must use reference identity,
not the displayed name. The detail's Ref ID distinguishes same-name references.

The approved mockup demonstrates switching between the reference modes,
entering a Ref ID, adding an entry, opening it from the file list, viewing its
captured name and Ref ID, and deleting it with confirmation. Apply the same
design to Forums and Characters, using their existing navigation paths. Keep
the application's existing appearance, spacing, rounded controls, and sidebar;
do not add explanatory copy or panel headings.

The mockup opens in Note reference mode to demonstrate the feature. The actual
panel retains Upload file as its initial mode, as specified above. Mockup-only
controls for changing the owner or simulating store readiness are review aids,
not product UI.

## Validation, identity, and persistence

The native application owns validation and name capture. The client submits
the owner identity, requested reference type, and entered Ref ID. It does not
submit an authoritative display name or store-readiness assertion.

1. Bind the request to the active vault and selected owner using existing vault
   change guards. Reject a request that becomes stale; never save it into a
   newly selected vault.
2. Verify the owner exists and permits file-list metadata changes. Reuse
   Character settings writability and Forum writability. This permits reference
   metadata for the built-in Assistant, whose settings and pictures are already
   editable, without making its text files editable. Other read-only owners
   remain read-only.
3. Trim surrounding whitespace and parse exactly one complete CHA Ref ID using
   the existing format. Share the parser currently private to `text_input.cpp`
   with this path instead of writing a second parser. Preserve case. Do not
   accept bare numeric database IDs, URLs, embedded references in prose, or
   provider resource IDs.
4. Resolve the item in that vault, check its type and any required ready store,
   capture its current name, and save under the existing application lifecycle
   lock and epoch check. Reuse this operation boundary; add no separate locking
   or cross-store transaction mechanism. Later source changes retain the entry
   as specified above.
5. Reject an existing reference with the same type and trimmed Ref ID under
   that owner. Repeated submission must not create duplicates or silently
   refresh the captured name. Allow the same reference under another owner.
6. Commit the complete entry before reporting success. A failed save leaves
   the owner's previous entries intact. A refresh or restart shows the saved
   name, type, and Ref ID without resolving the source again.

Each reference needs only this logical data:

| Field | Meaning |
| --- | --- |
| Owner | Existing Forum or Character identity, including its type. May be implicit in the owning configuration. |
| Reference type | Note reference or folder reference. |
| Ref ID | Trimmed CHA Ref ID for identifying the source within its originating vault. |
| Captured name | Source name at the successful add operation. |

The owner, reference type, and trimmed Ref ID provide sufficient identity for
selection, duplicate checks, and deletion. A separate generated entry ID is not
a product requirement. Do not use the captured name as a unique key, normalize
it to a disk filename, append `.md`, or reject valid note names because they
contain path punctuation.

Store references with the owner's existing vault-backed configuration through
the current configuration-edit path. Commit through the configuration store
without session invalidation, as Character picture edits do; do not use the
Markdown-file edit path that resets affected sessions. Keep one authoritative
representation.
The technical design will choose the exact serialization and API fields; no
new attachment service, second metadata database, source-name cache, or general
file abstraction is required.

Persist no note text, folder contents, provider name, credential reference,
remote store ID, upload timestamp, or readiness snapshot in the reference entry.
The type and Ref ID retain what is needed for later resolution within the same
vault. They do not establish source identity across vaults. The later usage
design will decide how to handle missing sources, replacement stores, and IDs
carried across vaults before enabling reference use.

Existing vaults with no reference metadata load as having no references.
Validate saved entry fields and duplicate identities through normal owner
configuration validation. Invalid field types, unsupported reference types,
invalid captured names, and malformed Ref IDs use the normal configuration
validation errors. A well-formed Ref ID does not need to resolve, match an
existing item's type, or point to a ready store when loading saved metadata.
Unknown or obsolete fields follow the existing warning-log rule.

Preserve references through ordinary owner renames, settings edits, vault
backup, and configuration round trips. A configuration-only copy or merge
keeps copied entries unchanged. In the destination vault, a Ref ID can be
missing or can name a different item, including an item of the same type.
Do not label all copied IDs as unresolved or refresh their captured names.
Such an entry still participates in duplicate checks; the user can delete it
and add the destination vault's own reference to capture its name.

Merges follow the existing per-file rule: a source configuration file replaces
the destination file with the same path, including its reference entries.
Reference lists are not combined; entries in the replaced file are replaced
too. Preserve well-formed metadata without source lookup or automatic
rebinding. Cross-vault source mapping and origin tracking are outside this
phase. A full vault copy retains notes and reference metadata but clears store
ownership under the existing copy behavior; keep saved folder references as
with any other store removal.

## Scope boundaries

This phase includes adding, persisting, listing, inspecting, and deleting these
reference entries for Forums and Characters. It preserves current upload and
text-file behavior.

It excludes automatic use in prompts, conversation attachment, retrieval,
provider/model compatibility checks, source editing, reference pickers, search,
drag and drop, bulk add, manual aliasing, source synchronization, remote store
management, and cross-vault references. It does not change existing chat Ref ID
or `/detach` behavior.

Adding, viewing, and deleting reference metadata must work offline. They must
not trigger provider requests, remote uploads/deletions, session resets, or model
generation. Do not add background work, events for routine metadata edits,
timers, or new configuration options. Unused or obsolete unrelated settings must
not block a valid operation; follow the existing warning-log rule.

## Acceptance criteria

1. Add a note reference under a Forum and under a Character. In both cases the
   list shows the current source name, and detail shows the correct CHA Ref ID.
   Restart and reopen the vault: both entries retain their saved values.
2. Add ready OpenAI and Google (Gemini) folder references. A nonuploaded folder,
   an incomplete upload, and a child whose parent alone was uploaded are
   rejected without saving an entry. The owner's model provider does not affect
   acceptance.
3. Reject malformed IDs, wrong-kind IDs, missing items, and IDs with extra text.
   Trim surrounding whitespace without changing the case-sensitive ID.
4. Add an empty note and a read-only note without reading or copying their
   content. Existing readable summary and annotation notes remain eligible.
5. Rename, move, edit, and delete source items after saving. Saved entries keep
   their captured names and IDs and remain viewable and removable. Remove or
   replace a folder's store: its saved reference remains unchanged.
6. Remove and re-add a renamed source. The new entry captures the new name.
   Re-adding a folder still requires a ready store at that time.
7. Repeated submission of the same reference under one owner creates at most
   one entry and does not refresh its name. The same reference can be saved
   independently under a different Forum or Character.
8. Save distinct references with identical names and a reference whose name
   matches a real filename. Each opens and deletes independently. Names with
   punctuation or no extension display unchanged. List references after actual
   files in captured-name order, using type and Ref ID to break ties.
9. Cancel, change modes, fail validation, and fail persistence. No unintended
   entry is saved, and only the selected mode is submitted. Preserve current
   text upload, picture upload, text creation, and existing file actions. From a
   reference mode, open and cancel the file chooser: keep that mode, its Ref ID,
   and its selected styling. Successfully load text: activate Write text. Load
   a picture: activate upload mode. Exactly one source button is highlighted
   with `aria-pressed="true"` in each state. Correct a wrong-kind error by
   switching reference modes without changing the entered Ref ID.
10. Switch vaults during an add. Use the existing lifecycle lock and epoch check
    to reject stale-vault work without creating entries in the wrong vault.
    Saved entries have no source foreign key or later source-change dependency.
11. Allow reference metadata where owner permissions permit it, including the
    Assistant's editable settings. Reject writes to other read-only owners in
    the native layer as well as the UI. Owner deletion removes only its entries.
12. Exercise the reference flows offline and verify that no provider request,
    prompt change, conversation attachment, or live-session reset occurs.
    Loading dangling references does not prevent vault use or unrelated edits.
13. Rename an owner and edit its settings: its reference entries stay unchanged.
    Create a vault from configuration: copied entries load and display their
    captured values even when Ref IDs are missing or identify different items.
    For a same-type ID collision, adding the destination item is rejected as a
    duplicate until the copied entry is removed. Remove and add it: the entry
    captures the destination item's name. Merge a configuration file containing
    references over the same destination file: its entries are replaced, not
    combined. A full vault copy retains references despite cleared store
    ownership.
14. Load malformed reference metadata through the normal configuration import
    path: report normal validation errors for invalid known fields or duplicate
    identities. Load well-formed metadata whose source is missing, has a
    different type, or no longer has a ready store: keep it without a source
    lookup. Existing vaults without reference metadata still load normally.

## Review decisions

Confirmed by the user: captured names stay fixed; refreshing requires removal
and re-addition. The original request also requires independent saved metadata
and ready-store eligibility for folders.

Confirmed by the user on October 30, 2026: the interactive UI proposal, including
the two-row source controls, single Ref ID field in reference modes, return to
the file list after adding, and read-only reference detail with deletion. The
Approved UI section records this design for implementation; do not request the
same UI approval again.

Duplicate rejection per owner, same-name coexistence, inclusion of existing
readable note kinds, and use of settings writability for reference metadata
remain product recommendations in this draft. UI approval does not imply a
separate decision on those rules.
