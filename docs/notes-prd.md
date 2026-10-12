# Notes: product requirements

Status: draft for review. This document describes proposed user behavior for
the macOS and Windows standalone applications. It does not approve a screen
design. Section 9 records the agreed picture storage choice and size limits.

## 1. Purpose

Notes lets a user write, organize, and keep documents inside CHA. A user can
maintain personal reflections, study notes, plans, or drafts without putting
that material into a conversation.

A note is a separate rich-text document. It is not a message, a saved session,
or a Self-note in a conversation. The user can create and edit a note without
starting a session or asking an AI character to respond.

## 2. User goals

With Notes, a user can:

- Capture an idea and return to it later.
- Develop a document over several editing sessions.
- Make a document easier to read with formatting and highlights.
- Include pictures or screenshots that remain available with the note.
- Keep clickable web references and open them in the default browser.
- Organize related documents in folders and subfolders.
- Find and correct text in the document being edited.
- Work with a note editor and a separate prompt box in the Notes panel.
- Prepare text by typing, voice input, or transliteration in the Notes prompt
  box and insert it into a chosen place in a note.
- Keep notes with the other material in the appropriate vault.

For example, a user can discuss a topic in a forum, copy useful text, then
open a note under `Study / Philosophy`. In the Notes panel, the user can paste
that text into its prompt box, edit it into a summary, and insert it into the
note. Text moves between the workflows only through an explicit user action
such as copy and paste. Another user can write a private journal directly in
Notes without using chat.

## 3. Organization and navigation

The existing sidebar has two tabs:

- **Chat** access to the existing forums and sessions.
- **Notes** access to the user's documents and folders.

These tab names are proposed labels for the design stage.

CHA has two separate main panels for two separate workflows:

| Panel | Contents and purpose |
| --- | --- |
| Chat | The selected session's chat history and its own prompt box, used to communicate with models through the existing chat workflow. |
| Notes | The selected note's rich-text editor and its own prompt box, used to prepare text and insert it into that note. |

Only one of these main panels is displayed at a time. The Notes panel shows
the note editor and its prompt box together. Session chat history and the
session picture panel are not part of the Notes panel. Pictures embedded in
the note remain part of the note editor.

The prompt boxes provide the same text-entry capabilities, but have separate
drafts and submission behavior. The Chat draft is retained when the user
switches panels and returns to the same conversation in the same vault.
Opening a different conversation starts an empty Chat draft and replaces the
previous one; CHA does not keep a draft for every conversation. Notes has one
draft that stays with the Notes panel, including when another note opens.
Drafts do not move between panels automatically. Submitting or clearing one
panel's prompt does not change the other panel's draft.

The Notes tab shows a tree. A note can be at the top level or inside a folder.
A folder can contain notes and other folders. The user can expand or collapse
folders and open a note by selecting it.

Each note and folder has a name that appears in the tree. Names follow the
same validation rules as session labels:

- A name must not be empty.
- A name can contain at most 200 Unicode code points.
- A name cannot contain control characters or line breaks.
- A name cannot start or end with whitespace.

CHA preserves the entered spelling; it does not trim whitespace or normalize
names automatically. These names are display labels, so operating-system
filename restrictions do not apply.

Within each folder and at the top level, no two items can have exactly the
same name. This rule applies across notes and folders: a note and a folder
cannot share a name in the same location. Comparison is case-sensitive, so
`Plan` and `plan` are different names. The same name is allowed in different
folders.

These rules apply when creating, renaming, or moving an item. CHA rejects an
invalid name or a duplicate name and explains what the user must change.
Rejected operations leave existing items unchanged. CHA does not overwrite
an item, merge folders, or rename an item automatically to resolve a duplicate.
The user must choose another name or destination.

At every level, folders appear first, followed by notes. Each group is sorted
alphabetically by name. The order updates after creation, renaming, or moving
an item. Users do not set a manual order.

Renaming or moving a note does not change its contents. Notes do not have to
belong to a forum or session. Removing a session does not remove a separate
note that the user wrote from it.

Switching sidebar tabs changes what the user can browse. It does not itself
switch the main panel, discard edits or prompt drafts, or change where the
visible panel's prompt submits. Opening a note or a session is a separate
action from switching sidebar tabs.

Opening a note displays the Notes panel in place of the Chat panel. To return
to chat, the user opens or creates a session from the Chat sidebar. CHA then
closes the note editor and displays the Chat panel, after finishing the note
save or resolving a failure under section 4.7. No separate Close note action
is required for this workflow. Opening another note also follows the save
before leaving rule.

The Notes prompt always inserts into the note shown in its panel, as described
in section 4.6. The Chat prompt keeps the existing chat submission behavior.
Opening a note does not change how the Chat prompt works.

Notes belong to the selected vault. Switching vaults shows the destination
vault's notes and folders. Notes from the previous vault remain in that vault.

## 4. Main workflows

### 4.1. Create a note

**Goal:** capture new material as a document that can be opened later.

1. The user opens the Notes tab.
2. The user chooses the top level or a folder as the destination.
3. The user chooses to create a note and gives it a name.
4. CHA opens the new note for editing and shows it in the tree.
5. The user writes or pastes content, inserts pictures, and applies formatting
   as needed.
6. CHA saves the note automatically. The user does not need to choose Save.

**Result:** the named note is available in the chosen location. The user can
close CHA, return to the same vault, and open the saved note again.

The first note must be possible to create without first creating a folder.

Paste keeps text, supported formatting, web links, and supported pictures
when their image data is available. Paragraphs, headings, lists, bold,
italic, underline, and highlighting are retained where the clipboard
supplies them in a supported form. Pasted font colors are removed; supported
highlighting is retained.

Unsupported fonts, font sizes, styling, and other embedded objects are
omitted. Text inside tables is retained as ordinary text in reading order,
with readable separation between cells and rows; table layout is not kept.
Pictures follow section 4.9. Links with HTTP or HTTPS destinations retain
their visible text and destination as described in section 4.10. Links with
any other scheme retain their visible text without a clickable link. If the
clipboard contains no supported content, pasting leaves the note unchanged.
A paste that changes the note forms one Undo step. A paste that exceeds a
picture or note size limit is rejected in full under section 5.2.

### 4.2. Read and continue editing a note

**Goal:** continue work on an existing document.

1. The user opens Notes and expands the relevant folders.
2. The user selects a note.
3. CHA shows its saved text, formatting, pictures, and links.
4. The user reads it, places the cursor where needed, and makes changes.
5. CHA saves the changes automatically as the user works.

**Result:** reopening the note shows the updated document. The user does not
need to recreate its formatting or find the conversation where it originated.

### 4.3. Organize notes in folders

**Goal:** group documents so they can be found by topic or purpose.

1. The user chooses the top level or an existing folder.
2. The user creates and names a folder.
3. The user creates notes in it or moves existing notes into it.
4. When more structure is useful, the user creates subfolders.
5. The user expands or collapses folders while browsing.

**Result:** the tree reflects the user's organization and retains it when CHA
is reopened.

The user can rename notes and folders, move notes between folders or back to
the top level, and move a folder together with its contents. The destination
must be clear before a move is completed. Drag-and-drop is not required for
this workflow.

A folder cannot move into itself or any of its subfolders. A move must also
meet the name rules in section 3 at the destination. If CHA rejects a move,
the item and its contents remain in their original location and the tree
stays unchanged.

### 4.4. Format a note

**Goal:** make the document readable and emphasize important material.

1. The user selects text or places the cursor where new text will be entered.
2. The user chooses a formatting command.
3. CHA displays the result in the document.
4. The user continues writing or uses Undo if the change was not intended.

The proposed first version includes bold, italic, underline, headings, lists,
and text highlighting. Undo and Redo are available during editing. Font color
is not included; highlighting provides color emphasis.

Each Replace, Replace All, prompt insertion, picture insertion, paste, link
edit, link removal, or formatting action that changes note content forms
one separate Undo step.
One Undo reverses the complete action, including every replacement made by
Replace All. It does not also reverse adjacent typing or another action.

Redo reapplies the complete action when it remains available in the editor's
history. Normal typing uses the editor's standard Undo grouping.

Searching, copying text, moving the cursor, selecting text, opening a web
link, and automatic saving do not create Undo steps. Automatic saving does
not clear Undo or Redo history.

Saved formatting remains visible when the note is reopened. Restoring an old
editing session's Undo or Redo history is not a requirement.

### 4.5. Find and replace text

**Goal:** locate a passage or correct repeated text in the current note.

1. The user opens Find and enters the text to locate.
2. CHA shows the matches in the open note.
3. The user moves between matches to inspect them.
4. To make a correction, the user enters replacement text, or leaves it empty
   to delete the matched text.
5. The user replaces the current match or chooses to replace all matches in
   that note.
6. CHA saves the result automatically under the rules in section 4.7.
7. The user reviews the result and can reverse the complete Replace or
   Replace All action with one Undo.

**Result:** the user can make a targeted correction without reading and editing
every occurrence manually. No matches is a normal result and is shown clearly.

This workflow searches the open note. Searching across all notes is outside
the proposed first version.

Search and replacement follow these rules:

| Behavior | Requirement |
| --- | --- |
| Match case | Available for ordinary text searches and regular-expression searches. When enabled, letter case must match. |
| Whole word | Matches complete words in ordinary text searches. This option is unavailable while regular-expression search is enabled. |
| Empty replacement | Deletes the current match with Replace, or all matches with Replace All. |
| Regular expressions | Use the extension's RE2-compatible syntax. Lookarounds and backreferences in search patterns are not supported. |
| Replacement text | Inserted literally in both search modes. For example, `$1` inserts the characters `$1`; it does not insert a captured group. |
| Undo and Redo | Apply to the complete Replace or Replace All action, including deletion through an empty replacement, under section 4.4. |

CHA uses Tiptap's MIT-licensed, headless
[Find and Replace extension](https://tiptap.dev/docs/editor/extensions/functionality/find-and-replace)
for search behavior. The choice of search controls remains a design decision
in section 7. Using the extension does not require adopting Tiptap's supplied
panel. Extra search features beyond the behavior specified here are outside
the first version.

During implementation, pin the extension to an exact tested version compatible
with the chosen Tiptap packages and keep the dependency lockfile. Verify the
requirements in this section against that version on both platforms. These
requirements, rather than the current behavior of a demo or documentation
page, define acceptance.

### 4.6. Insert prompt text into a note

**Goal:** prepare text in the Notes panel's prompt box and insert it into
the document shown in that panel.

1. The user opens the destination note. The initial insertion point is the end
   of the document. The user can place the cursor elsewhere or select text
   to replace.
2. In the Notes panel, the user prepares text in its prompt box by typing,
   voice input, or transliteration.
3. The user clicks Send, presses Enter in the prompt box, or says the
   configured voice send phrase, such as "over to you."
4. CHA inserts the submitted text as an ordinary paragraph at the note's
   remembered insertion point, under the rules below. If text was selected
   in the note, the paragraph replaces that selection. Line breaks in the
   prompt are preserved within the inserted paragraph.
5. The note's insertion position moves immediately after the inserted
   paragraph, before any remaining original text. Keyboard focus stays in,
   or returns to, the prompt box.
6. After successful insertion, CHA clears the submitted text from the prompt
   box. The user can prepare the next paragraph immediately.
7. CHA saves the insertion automatically under the rules in section 4.7.

**Result:** the user can build a document from prepared passages without
manually copying and pasting between the two editors.

Moving focus from the note to its prompt box preserves the note's insertion
point or selection. Each successful insertion updates the remembered position,
so successive submissions appear in order before any trailing original text.
The user does not need to place the cursor again between submissions.

| Insertion location | Behavior |
| --- | --- |
| A note has just opened | Insert at the end of the document unless the user places the cursor elsewhere or selects text. |
| Inside a paragraph | Split the paragraph at the cursor and insert the new paragraph between the two parts. Existing text keeps its formatting. |
| Inside a heading | Split the heading at the cursor, keeping both parts at the original heading level. Insert an ordinary paragraph between them. |
| Inside a list item | Split the paragraph inside that item. Keep the inserted paragraph and both remaining parts within the same list item, without creating another bullet or number. |
| At the start or end of a block | Insert before or after that block without creating an empty fragment. If the current paragraph is empty, fill it. |
| Text is selected | Replace the selection with the inserted paragraph. Preserve content outside the selection and apply the same insertion rules to the remaining text at its boundaries. |

The Notes prompt box provides the same text-entry capabilities as the Chat
prompt box, including voice input, transliteration, resizing, and Ctrl+Enter
to insert a line break. Voice input uses the existing configuration and
availability. Separate voice input and transliteration inside the note editor
are not required.

In the Notes prompt box, all three submission methods insert into the
displayed note without sending a chat message or requesting a response from
an AI character.
The voice send phrase is a command and is excluded from the inserted text.
The prompt text is inserted as ordinary paragraph text, with its line breaks
preserved; it is not interpreted as Markdown or HTML. The destination note
must be clear before submission.

The inserted text does not inherit bold, italic, underline, highlighting,
or an existing hyperlink from the insertion point. A paragraph inserted
inside a list item still belongs to that item. HTTP and HTTPS URLs contained
in the submitted text follow the automatic-link rules in section 4.10;
this does not extend an existing link onto the inserted text.

Chat commands and addressing are not processed in the Notes panel. Text such
as `/clear`, `/mcast`, or `@Name` is inserted literally into the note. The Notes
panel has no chat target selector and cannot clear a session or send to a
character through prompt submission. These chat controls and behaviors remain
in the Chat panel.

If insertion fails, the prompt text remains available for retry. Clearing
after a successful insertion removes only the submitted text; text entered
after submission is retained. A later automatic-save failure follows section
4.7 and does not insert the paragraph again.

One Undo reverses the complete insertion, including block splitting and
restoring any selected text that it replaced. One Redo reapplies the complete
action when available.
Undo and Redo in the note do not restore the cleared prompt or change a new
prompt draft.

### 4.7. Keep work saved and change context

**Goal:** move between activities without accidentally losing edits.

1. The user edits a note. CHA saves changes automatically while the user works.
2. The user moves to the Notes prompt box, switches sidebar tabs, opens another
   note or a chat session, opens Settings or another screen, changes vaults,
   or closes CHA normally.
3. Before a user action closes or replaces the note editor, switches or
   replaces its vault, or closes CHA normally, CHA finishes saving or asks
   the user how to handle unsaved changes. Moving focus or switching sidebar
   tabs without closing the note follows the timed saving rule below.
4. The user later returns to the note and continues from the saved contents.

**Result:** ordinary navigation and normal application exit do not silently
discard edits. CHA shows an actionable error when saving fails.

Automatic saving follows these rules:

| Event | User-visible behavior |
| --- | --- |
| During editing | CHA starts an automatic save no later than five seconds after the first unsaved change. Further editing does not restart this deadline. |
| Before leaving | CHA finishes saving before a user action closes or replaces the note editor, switches or replaces its vault, or closes CHA normally. If saving fails, the user can Retry / Stay / Discard. Confirmed deletion is an exception, as described in section 4.8. |

Opening Settings closes the note editor under the same save-before-leaving
rule. CHA finishes saving or resolves a save failure through Retry / Stay /
Discard before Settings opens. Choosing Stay cancels navigation and keeps
the note open.

Finishing saving includes any save already in progress. Closing the editor
must not leave a delayed save that runs afterward. A pending save must not
write into another vault or restore old content after the active vault is
replaced.

CHA saves only when the document has changed. Moving the cursor, selecting
text, or searching without replacement does not require a save. Typing,
formatting, replacement, prompt insertion, picture insertion, paste, Undo,
and Redo follow the same saving deadline. Moving focus to the prompt editor,
toolbar, or another control does not require an immediate save.

The five-second deadline specifies when saving starts; a slow write can take
longer to finish. The deadline applies during continuous typing as well as
after the user stops. Failed saves follow the behavior below.

Normal automatic saving is quiet; no permanent save-status indicator is
required. Saving does not move the cursor, change keyboard focus, or clear
the current Undo or Redo history.

If saving fails, CHA shows an actionable error and keeps the edited content
available so the user can retry or copy it. The error remains until the
affected changes are successfully saved or explicitly discarded, including
through successful confirmed deletion of the note. Starting a retry does
not clear the error. A failed or canceled deletion also leaves the error
and edited content available.

If an action requires saving first, the user can retry, stay with the note
and cancel the action, or explicitly discard the unsaved changes and proceed.
CHA does not silently discard them.
If opening a note fails, CHA reports the failure rather than presenting it
as an empty document.

Whole-vault uploads and copies include the latest saved note edits, including
edits saved when leaving the editor. Changes explicitly discarded by the user
are not included. The effects of vault operations on notes are defined in
section 5.1.

Download replaces the local vault. Its confirmation must make clear that
local notes are included in that replacement. Edits saved when leaving the
editor remain in the previous database's `.bac` backup. They remain in the
active vault only if they are also in the downloaded copy. Section 5.1 defines
the backup limits.

Normal close includes both Quit and closing the last window on macOS, and
Exit or closing the application window on Windows. For each close attempt,
CHA requests completion of saving once and keeps the window and note
available. If there are no unsaved changes or saves still in progress, CHA
proceeds with shutdown immediately. Otherwise, shutdown waits until saving
succeeds or the user explicitly chooses to close without saving.

If saving fails during normal close, CHA cancels the close attempt and
offers Retry / Stay / Discard. If the editor does not respond within a
bounded wait, CHA offers Cancel close / Close anyway and warns that unsaved
changes may be lost. The prompt remains available even if the editor is
unresponsive. CHA does not close just because the wait expired, and no close
timeout runs while the user is deciding.

The wait for saving and the user's decision are separate from CHA's existing
ten-second shutdown limit. That limit applies only after saving completes,
no saving is needed, or the user explicitly chooses to close without saving.

The user does not need a manual Save action. Recovery of changes that were
not yet saved when the application crashed, was forcibly terminated, or
reloaded its page to recover from a fault is outside this requirement.

### 4.8. Remove a note or folder

**Goal:** remove material that is no longer needed.

1. The user selects a note or folder and chooses Delete.
2. CHA makes clear what will be removed and lets the user cancel.
3. The user confirms the deletion.
4. After deletion succeeds, the removed item no longer appears in the tree
   or as an editable document.

Confirmed deletion of the open note also authorizes discarding its unsaved
changes, without a separate save prompt. This applies to folder deletion
only when the confirmed operation includes that note. CHA keeps the edited
contents available until deletion succeeds. If deletion is canceled or
fails, the note and its edits remain available. A save still in progress
must not recreate a deleted note or show an irrelevant error after deletion.

**Result:** the user can maintain the collection without accidentally deleting
unrelated material. A folder's contents must not disappear without an explicit
choice. Deleted items cannot be recovered in CHA. The treatment of nonempty
folders remains a design decision.

### 4.9. Add a picture to a note

**Goal:** keep a picture or screenshot with the text it supports.

1. The user opens a note and chooses where to insert the picture.
2. The user selects a supported picture from a local file or pastes supported
   image data from the clipboard.
3. CHA inserts the picture into the note.
4. CHA saves the note and its picture automatically under the rules in
   section 4.7.
5. The user reopens the note later and sees the picture, including offline.

**Result:** the picture is stored with the note. It remains available if the
original file is moved or removed, and it travels with whole-vault copies
and uploads. The note does not depend on a website or a temporary picture
reference to display a saved picture.

The first version supports PNG, JPEG (`.jpg` and `.jpeg`), WebP, and GIF,
matching CHA's existing picture formats. Clipboard insertion accepts a
supported image representation supplied by the clipboard. CHA does not need
to convert unsupported image formats for Notes.

Each picture can contain at most 1 MiB (1,048,576 bytes) of image data before
Base64 encoding. This limit applies to local files, clipboard images, and
images embedded in pasted formatted content. The resulting note must also
fit within the total size limit in section 5.2. A picture can meet its own
limit and still be rejected because the note would become too large.

One Undo reverses the complete picture insertion or paste, and one Redo
restores it when available. An image reference containing only a website
URL is not sufficient to store a picture; its image data must be obtained
before it can be preserved with the note.

### 4.10. Keep, edit, and follow web links

**Goal:** keep useful web references in a note, correct or remove links, and
open them when needed.

1. The user types or pastes a web URL, or pastes text containing a hyperlink.
2. CHA makes valid HTTP and HTTPS URLs clickable. Pasted hyperlinks with
   these schemes retain their visible text and destination, including named
   links such as "Read the article."
3. CHA saves the links with the note under the rules in section 4.7.
4. The user clicks a link. CHA opens its destination in the system's default
   browser, while keeping the note open with its edits preserved.

**Result:** supported links remain clickable after the note is reopened.
HTTP and HTTPS have the same link-opening behavior on both macOS and Windows:
an ordinary click opens the saved destination in the system's default browser
and leaves the note open. No modifier key is required. An explicit `http://`
destination stays HTTP; CHA does not rewrite it to HTTPS. Linked web pages
are not stored with the note.

An ordinary click opens the link rather than placing the editing cursor
inside its text. To edit the visible text while keeping the link, the user
can move the cursor into it with keyboard navigation from adjacent text.

The user can open a link's context menu with a secondary click, such as a
right-click, without opening the destination. It provides these actions:

- **Edit link:** change the destination to a valid HTTP or HTTPS URL while
  keeping the visible text. An invalid or unsupported destination is rejected
  with an error, and the existing link stays unchanged. Canceling also leaves
  the link unchanged.
- **Remove link:** remove the hyperlink while keeping its visible text and
  other supported formatting. The text can then be clicked and edited as
  ordinary text. This also applies to automatically created links.

These actions are also available through the keyboard when the cursor is
inside a link. A link edit or removal follows the automatic-saving rules in
section 4.7 and forms one separate Undo step. One Undo restores the previous
destination or the removed link; one Redo reapplies the change when available.
Removing a link does not immediately create it again through autolinking,
and saving and reopening the note preserves the removal.

A bare domain recognized as a web address, such as `example.com`, becomes a
link to `http://example.com`. Both explicit HTTPS links and these default
HTTP links must open through the native application on each platform.

HTTP and HTTPS are the only supported link schemes. Addresses using `mailto:`,
`file:`, `javascript:`, `data:`, custom app schemes, or any other scheme remain
plain text. A pasted named hyperlink with an unsupported destination keeps
its visible text but loses the link. CHA does not rewrite an unsupported
scheme into HTTP or HTTPS. This rule applies to typed text, pasted content,
and reopened notes. Unsupported link destinations must not launch a browser,
open a file or another application, or execute code.

## 5. General user expectations

- Creating, reading, editing, organizing, and saving notes works offline.
- The same main workflows are available on macOS and Windows.
- The user can reach essential actions with the keyboard as well as a mouse.
- Text in the languages already used in CHA can be entered, saved, searched,
  and reopened without damage.
- A note is not automatically sent to an AI character or added to conversation
  context merely because it is open.
- The selected note and the destination of an editing action are clear.
- Empty collections and empty folders still allow the user to create items.
- Saved notes, including their pictures and links, remain part of their vault
  when the user copies or transfers the whole vault through existing vault
  operations.

### 5.1. Notes and vault operations

Notes and folders are user content. They follow these rules for existing
vault operations, including operations available without the Notes editor:

| Operation | Behavior for Notes |
| --- | --- |
| New vault without a copy source | Starts with no notes or folders. |
| Copy vault | Copies notes and folders, including note text, formatting, pictures, and links. |
| Merge | Does not copy source notes or folders, or change destination notes or folders. |
| Parent merge | Does not merge notes or folders into the active vault. Its preliminary R2 download replaces the local parent vault, including its notes and folders. |
| Upload | Includes saved notes and folders, with their formatting, pictures, and links. |
| Download into an existing vault | Replaces local notes and folders with the downloaded collection, including its formatting, pictures, and links. |
| Workspace Export / Import | Does not transfer notes or folders. Import leaves existing notes and folders unchanged. |
| Markdown session mirror | Does not include notes. |

R2 Download replaces the local collection rather than combining it with the
remote collection. Local notes, folders, and edits not present in the
downloaded copy are absent from the active vault after replacement.

The previous local database stays in the `.bac` backup, including changes
saved before replacement. Unsaved changes that the user explicitly discarded
are not in that backup. Each successful replacement replaces the previous
`.bac` backup; it is one previous database copy, not a permanent history.
These replacement and backup rules also apply to the parent vault downloaded
as part of Parent merge. The active vault's notes and folders stay unchanged
by the merge. A later merge failure does not undo the completed parent
download.

### 5.2. Picture and note size limits

A note's saved JSON document must fit within 12 MiB, including its text,
formatting, and embedded pictures. Section 9 gives the exact measurement.
This limit includes the encoded pictures; it is not an allowance for pictures
with additional space for text.

Before accepting a document change, CHA checks the resulting content against
the picture and total size limits. This applies to all document changes,
including typing, formatting, picture insertion, paste, prompt submission,
Replace, and Replace All.

If a change would exceed either limit, CHA rejects the complete operation
and shows an actionable error identifying the limit. The note's content,
selection, and Undo and Redo history remain unchanged. A paste or replacement
is not partially applied. A rejected prompt submission leaves the prompt
text available. The user can choose a smaller picture or remove content
before trying again. CHA rejects the change before it can cause repeated
automatic-save failures.

## 6. Boundaries of the proposed first version

The first version focuses on personal documents, a folder tree, rich-text
editing, stored pictures, clickable web links, search and replacement within
a note, and the prompt-to-note workflow. It assumes one note is open for
editing at a time.

The following are not required by this draft:

- Collaboration, sharing, or simultaneous editing by multiple users.
- Automatic AI writing, summaries, or autonomous changes to notes.
- File attachments, tables, or page-layout tools.
- Automatic downloading of pictures referenced only by web URLs.
- Document import, export, printing, or synchronization with external folders.
- Tags, backlinks, or search across the entire note collection.
- Multiple open note tabs, document version history, or a recycle bin.
- Drag-and-drop organization.
- Notes in the browser application.

These boundaries keep the initial feature small. They do not prevent later
additions if a user need justifies them.

## 7. Decisions for the design stage

1. **Notes panel layout:** the size and placement of the note editor and its
   prompt box within the Notes panel, including behavior when space is
   limited. The Notes panel is separate from the Chat panel; no combined
   layout with session history or the session picture panel is required.
   Navigation and prompt submission rules are fixed in sections 3 and 4.6.
2. **Organization and deletion:** the controls for choosing destinations and
   the behavior and controls for deleting folders that contain notes.
3. **Picture controls:** the controls for inserting pictures in the supported
   formats defined in section 4.9. These choices must preserve the agreed
   offline and whole-vault transfer behavior.
4. **Find and replace controls:** choose between Tiptap's supplied
   [Search and Replace panel](https://tiptap.dev/docs/ui-components/components/search-and-replace),
   adapted to CHA's appearance, and a small CHA find bar using the same
   headless extension. Compare the supporting components and dependencies
   needed for each option and prefer the simpler integration. Either option
   must meet section 4.5, including empty replacements and making Whole word
   unavailable in regular-expression mode. Present the proposed controls
   visually and obtain explicit user approval before implementing the UI.
   This PRD does not approve the supplied panel or any other screen design.

## 8. User acceptance scenarios

The feature is ready for normal use when a user can demonstrate these outcomes
on both supported desktop platforms:

1. Create a note at the top level, write formatted text, close CHA normally
   without choosing Save, and reopen it with the same content and formatting.
   On macOS, verify both Quit and closing the last window; on Windows, verify
   both Exit and closing the application window.
2. Create a folder and subfolder, add a note, rename and move it, then find it
   at its new location after restarting CHA. At each level, folders appear
   before notes, and each group stays in alphabetical order after creating,
   renaming, or moving items and after restarting CHA.
3. Switch sidebar tabs without changing the main panel, its submission
   destination, or either prompt draft. With the Notes panel displayed, move
   focus to its prompt box without losing note edits. Note changes follow
   the five-second saving deadline; moving focus does not require an immediate
   save. Open the same conversation from the Chat sidebar: the note editor
   closes after saving or resolving a save failure, and the Chat panel appears
   with that conversation's retained draft. Reopen the note to return to the
   Notes panel with its own draft. Open a different conversation and verify
   that its Chat prompt starts empty. Open another note and verify that the
   Notes-panel draft stays available. Drafts never transfer between the two
   prompts. Chat history is visible in Chat and absent from Notes.
4. Find repeated text, replace one match or all matches in the current note,
   and reverse each complete action with one Undo. For Replace All, verify
   that one Undo reverses every replacement and one Redo reapplies them all,
   without also reversing or reapplying adjacent typing or another action.
   Use text such as `cat`, `Cat`, and `catalog` to check Match case and Whole
   word. Match case applies in both ordinary and regular-expression searches;
   Whole word applies only to ordinary searches and is unavailable in regex
   mode. Verify the regex syntax limits in section 4.5. Replace with `$1` and
   find those literal characters inserted, without captured-group expansion.
   Leave replacement text empty and delete one match or all matches. One Undo
   restores the complete deletion and one Redo reapplies it. These outcomes
   must hold with either approved search UI and the pinned extension version.
5. Use the note editor and its own prompt box in the Notes panel. Prepare text
   through typing, transliteration, and voice input; use Ctrl+Enter for line
   breaks.
   Submit through Send, Enter, and the configured voice phrase. Each method
   inserts an ordinary paragraph at the remembered note position, preserves
   the prompt's line breaks, and moves the note's insertion position immediately
   after that paragraph, before any remaining original text.
   Selected note text is replaced. The voice command is excluded, and no chat
   message or request for an AI character response is made. The submitted text
   clears only after successful insertion, later prompt input remains, and
   focus stays in or returns to the prompt box. If insertion fails, the prompt
   text remains.
   Submit to a freshly opened note without placing the cursor and find the
   paragraph at the end of the document. Insert in the middle of a paragraph
   and a heading: the new paragraph appears between the two remaining parts,
   and existing text keeps its formatting, including the heading level.
   Insert inside a list item and find all three paragraphs within the same
   item, without an extra bullet or number. Insert at a block's start or end
   without an empty fragment, and fill an empty paragraph in place. Replace
   a selection and preserve content outside it under the same boundary rules.
   Insert from a position with bold, italic, underline, highlighting, or a
   hyperlink and find none of those inherited by the new text. HTTP and HTTPS
   URLs in the submitted text still follow the automatic-link rules. Submit
   several passages without returning focus to the note and find them in
   order before any trailing original text.
   One Undo reverses the complete insertion, restores replaced text, and
   restores the block structure before splitting; one Redo reapplies the
   complete action. These actions do not alter the current prompt draft or
   include adjacent typing in the note. The Chat prompt draft is unaffected.
   Submit `/clear`, `/mcast`, and `@Name` as Notes prompt text and find them
   inserted literally, with no effect on a chat session. The Notes panel has
   no chat target selector. Return to a session and verify that the Chat
   prompt still uses its existing commands, addressing, and target selection.
6. Change vaults and return to find the saved notes in their original vault.
   Create a new vault without a copy source and find no notes or folders in it.
7. Cancel deletion without losing edits, then deliberately delete an item
   with a clear understanding of what will be removed. Successful deletion
   of the open note discards its unsaved edits without a separate save
   prompt. This also applies when a confirmed folder deletion includes the
   open note. If deletion fails, the note and its edits remain available.
   A save already in progress does not restore the deleted note or show an
   irrelevant error after deletion. Deleted notes and folders cannot be
   recovered in CHA.
8. Encounter an opening or saving failure and understand what happened without
   losing the edited content or mistaking a failed load for an empty note.
   A save error stays visible until the affected changes are saved or explicitly
   discarded, including through successful confirmed deletion. Starting a
   retry does not hide the error. If a failed save prevents navigation or
   normal exit, choose to retry, stay, or explicitly discard the changes.
9. Keep typing continuously and have an automatic save start within five
   seconds of the first unsaved change. Further editing does not restart the
   deadline. Automatic saves preserve the cursor, focus, and Undo and Redo
   history.
10. Apply formatting, replace text, insert prompt text, or use Undo or Redo and
    have the changed document follow the same saving deadline as typing,
    without requiring an immediate save after each command. Let an automatic
    save finish after Undo and verify that Redo remains available.
11. Open Settings while a note has unsaved edits or a save is in progress.
    CHA finishes saving before closing the note editor and displaying
    Settings. A failed save leaves the note available for Retry / Stay /
    Discard before navigation proceeds; Stay keeps the note open. No delayed
    save runs after the editor closes. Return to the note with all edits
    saved unless they were explicitly discarded. The same save-before-leaving
    rule applies when opening another screen that closes the note editor.
12. Upload or copy the whole vault and find the latest note edits in the
    result, unless they were explicitly discarded. Include folders, formatting,
    pictures, and links. Before Download replaces the local vault, understand
    that local notes and folders will also be replaced. After Download, the
    active collection matches the remote copy. The `.bac` database contains
    the previous local collection and edits saved before replacement, but no
    explicitly discarded unsaved changes. A later successful Download replaces
    that backup. No pending save writes into another vault or restores old
    content after the active vault is replaced.
13. Request normal close while the editor is unresponsive and, after a
    bounded wait, choose Cancel close or Close anyway with a warning about
    unsaved changes. The wait expiring does not silently close CHA, and the
    existing shutdown limit does not cut short saving or the user's decision.
14. Paste mixed content copied from Word and a web page. Keep text, supported
    formatting, HTTP and HTTPS hyperlinks, and supported pictures when image
    data is available. Remove font colors while retaining supported highlighting.
    Retain table text with readable separation, without table layout or
    unsupported styling and objects. One Undo reverses the whole paste.
    Pasting content with nothing supported leaves the note unchanged.
15. Insert local PNG, JPEG (`.jpg` and `.jpeg`), WebP, and GIF pictures, and
    paste a picture from a supported clipboard image representation. Undo
    and Redo each insertion as one step. Save and reopen the note offline
    and see the pictures even after the original files are moved or removed.
    A whole-vault copy or upload includes those pictures.
16. Type or paste explicit HTTP and HTTPS URLs, a bare domain, and named
    hyperlinks with HTTP and HTTPS destinations. Save and reopen the note,
    then click each link to open its saved destination in the system's default
    browser on both macOS and Windows. The bare domain uses HTTP, explicit
    schemes are preserved, and the note stays open with its edits preserved.
    Type and paste `mailto:`, `file:`, `javascript:`, `data:`, and custom-scheme
    addresses, including named hyperlinks with those destinations. Their
    visible text remains without clickable links, including after reopening.
    Clicking that text does not open a browser, file, or application or execute
    code.
17. Create and rename notes and folders with valid names, including names of
    200 Unicode code points. Empty names, longer names, control characters,
    line breaks, and leading or trailing whitespace are rejected without
    changing existing items. At the top level and inside a folder, creating,
    renaming, or moving an item cannot give it the exact name of another note
    or folder there. `Plan` and `plan` can coexist, and the same name can be
    used in different folders.
18. Try to move a folder into itself or a subfolder, or move an item into a
    location with a duplicate name. CHA explains why the move cannot proceed.
    The item and its contents stay at their original location; no item is
    overwritten, merged, or renamed automatically.
19. Merge another vault's configuration or import workspace configuration and
    find the existing notes and folders unchanged, with no source notes or
    folders added. Workspace Export and the Markdown session mirror produce
    no note documents.
20. Use Parent merge where it is available. The active vault's notes and
    folders remain unchanged. The local parent vault receives the remote
    collection, and its previous saved collection stays in its `.bac` backup
    under the same rules as Download. If the merge fails after the parent
    download succeeds, the downloaded parent collection remains in place.
21. Insert a supported picture at the 1 MiB limit when the note has enough
    room. Try a picture over that limit from a local file, clipboard image,
    and image embedded in pasted formatted content. Each oversized picture
    is rejected with an actionable error. A mixed paste containing text and
    an oversized picture is rejected in full. The note, selection, and Undo
    and Redo history stay unchanged.
22. Add individually permitted pictures and text until a further change would
    exceed the total size limit. Verify that picture insertion, paste, typing,
    formatting, prompt submission, Replace, and Replace All cannot exceed it.
    Rejected operations leave the note unchanged, and rejected prompt text
    remains available. Remove content and successfully make a change that now
    fits. With a note near the limit, verify that typing remains responsive
    and automatic saving and reopening work on both platforms.
23. Open a link with an ordinary click. Open its context menu with a secondary
    click without launching the browser. Use Edit link to correct its HTTP or
    HTTPS destination while keeping the visible text. Invalid or unsupported
    destinations and canceled edits leave the link unchanged. Use Remove link
    on an incorrect autolink and retain its text and other formatting as
    ordinary editable content; the link is not immediately recreated. Reach
    both actions with the keyboard, and use keyboard navigation to edit link
    text without opening its destination. Undo and Redo each link change as
    one complete action. Save and reopen the note and verify that corrected
    destinations and link removals remain in effect on both platforms.

## 9. Agreed picture storage choice and size limits

For the first version, picture data is embedded as Base64 in the note's JSON
document and saved with that document in the vault's SQLite database. This
keeps each note and its pictures together without separate image files or
an external image service. Picture import must obtain the actual image data;
saving only a website URL or a temporary browser reference does not meet the
offline requirement.

The picture limit is 1 MiB (1,048,576 bytes) of image data before Base64
encoding. The note limit is 12 MiB (12,582,912 bytes) for the complete
document JSON serialized without extra whitespace, measured in UTF-8 bytes.
It includes text, formatting, Base64 image data, and JSON escaping, but
excludes the bridge request envelope and its metadata.
Character counts and the combined sizes of original picture files are not
the measurement. The gap below the native bridge's separate 16 MiB request
limit leaves room for the request envelope.
