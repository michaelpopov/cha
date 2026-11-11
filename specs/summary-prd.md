# Note summaries: product requirements

Date: November 11, 2026

Status: Requirements agreed with the user. This document does not specify the
technical design or the UI design.

## 1. Purpose

CHA makes summaries of note text at several levels. A reader can then read a
book, or a set of articles, in a sequence that the reader selects:

1. Read the folder summary (L3) to learn what the book is about and to get a
   map of its chapters.
2. Read the L2 summary of a chapter to learn what the chapter teaches.
3. Read the L1 text of a chapter for more detailed study.
4. Read the original text (L0) for small details or for the author's style.

The main user is a reader with ADHD. The reader does not have to start with a
wall of text. The reader gets structured material at different levels and
selects the depth for each chapter.

The feature applies to any notes: book chapters from an EPUB import,
scientific articles, and other text.

## 2. Levels

| Level | Scope | Source | Content |
| --- | --- | --- | --- |
| L0 | One note | — | The original text of the note. |
| L1 | One note | L0 | The text with the parts removed that are not necessary. |
| L2 | One note | L0 | The main ideas and key facts in a short form. |
| L3 | One folder | The notes of the folder | One Summary note for the folder. |

L1 and L2 are made from L0, independently of each other. L2 is not made from
L1.

### 2.1 L1

Intent: L1 is the original text with the unnecessary material removed. It
keeps the message of the text. It stays close to the original text in style
and flow, and it is coherent text. The model removes material and preserves
the original text as best effort. The model can change the text when a change
is necessary.

Version 1 makes L1 with the same method as the current compressed version:

- The default L1 prompt is an exact copy of
  [compression-prompt.md](../resources/compression-prompt.md).
- The model writes the full L1 text.

The L1 prompt is a separate prompt. A change to the compression prompt does
not change the L1 prompt.

### 2.2 L2

L2 gives the main ideas and key facts of the note in a short form that a
reader can understand easily. It can use lists, short paragraphs, and other
forms that give the information in less volume.

The default L2 prompt has these requirements:

- Each L2 starts with the same short part: one to three sentences about the
  subject of the text and its main point.
- The model selects the form of the rest of L2 from the content of the text.
  The prompt gives a short guide for each kind of text, for example:
  - a book of ideas or arguments: the claim, the reasons, and the evidence;
  - a textbook or a technical text: definitions, procedures as steps, and
    comparisons;
  - history, biography, or journalism: people, events, and causes in time
    order;
  - a scientific article: the question, the method, the results, and the
    limits;
  - a how-to text: the practices, their steps, and when to use them;
  - philosophy or an essay: the chain of reasoning in short paragraphs.
- The model uses lists first. Tables are permitted, but notes show them as
  plain lines in version 1 (see section 5).

### 2.3 L3

L3 is one note with the name **Summary** in a folder. It summarizes the
included notes of that folder (see section 4). Version 1 includes only the
notes directly in the folder. It does not include the notes of subfolders.

CHA makes L3 in two steps:

1. For each included note, in tree order, CHA makes one model call.
   - The input is the compressed text of the note. If the note has no
     compressed version, the input is its original text. Chats use the same
     rule today.
   - The output is the section for that note: a few paragraphs with its main
     ideas and key facts.
2. CHA makes one model call for the folder.
   - The input is all sections from step 1.
   - The output is the overview: the subject of the folder, its main
     argument, and how the notes connect.

CHA then writes the Summary note in this order:

1. The overview.
2. The sections, in tree order. Each section has the name of its note as its
   heading.

The L3 section prompt tells the model how to treat a note that has no
substantive content, for example a table of contents or an index. This is
best effort.

CHA does not keep the sections from step 1. Each L3 run makes all sections
again.

### 2.4 Rules for all levels

- The result is in the language of the source text.
- The prompts have no size targets. If the results are too long, the user
  changes the prompts.
- The user accepts that the model can change the text in L1, and that L2 and
  L3 are rewrites made by a model.

## 3. Prompts

CHA adds four prompts to the vault prompt catalog, as defined in
[prompts-design.md](prompts-design.md):

| Prompt | Use |
| --- | --- |
| L1 | Makes L1 of a note. Its default text is a copy of the compression prompt. |
| L2 | Makes L2 of a note. |
| L3 section | Makes the section of one note for L3. |
| L3 overview | Makes the overview of the folder from all sections. |

The user can view and edit these prompts in Settings, as the other catalog
prompts.

## 4. Starting the work

One menu item starts the work for all levels. Its label is part of the UI
design.

### 4.1 On a note

The menu item makes L1 and L2 of the note. It replaces the existing L1 and
L2.

- The note must have 4 KiB to 256 KiB of text. Otherwise, CHA does not start
  the work and shows a message.
- The exclusion flag (section 4.3) does not apply to this operation.

### 4.2 On a folder

The menu item processes the included notes directly in the folder:

- It makes L1 for each included note that has no L1.
- It makes L2 for each included note that has no L2.
- It makes L3 for the folder. It replaces the content of the Summary note of
  the folder (section 5.2). If the folder has no Summary note, CHA makes one.

An included note is a note directly in the folder that meets all these
conditions:

- It is not a summary note. Session summary notes and the Summary note are
  not included.
- Its exclusion flag is not set.
- It has a minimum of 4 KiB of text.

If the folder has no included notes, the operation does not make L3.

The 256 KiB maximum also applies in a folder operation:

- CHA does not make L1 and L2 for a note with more than 256 KiB of text.
  Events records this as a failure of that note.
- For L3, the same maximum applies to the input of each section. If a note is
  over the maximum and has no compressed version, L3 fails. The error names
  the note. The user can compress that note and run the operation again.

### 4.3 Exclusion flag

Each note has a setting "excluded from summaries".

- The user changes it in the note menu. The menu shows the setting as a check
  mark. The tree does not show it.
- Folder operations skip excluded notes for all levels.
- The flag goes with the note in folder export, folder import, and D1 sync.

Automatic classification, for example by Jev, is not in version 1.

### 4.4 Provider

All levels use the compression provider that is selected in Notes settings.
There is no new setting. If no compression provider is selected, CHA does not
start the work and shows the same message as compression.

## 5. Results as documents

- L1 and L2 are versions of their note, as the compressed version is. They do
  not show as separate items in the tree.
- CHA converts the model's Markdown into a formatted document with headings,
  paragraphs, lists, and text formatting, as it does for session summaries.
  The result is not one plain paragraph with Markdown marks.
- Tables become plain lines of text in version 1. The Notes editor does not
  get table support.
- The user opens L1 and L2 in the Notes editor as regular documents. The user
  can add highlights and comments. L1 and L2 use the read-only setting of the
  original note, as the compressed version does. Thus the user can edit their
  text only when the original note is not read-only. EPUB chapters are
  read-only.
- The user can open L0, L1, L2, and the compressed version of a note in the
  Notes editor. This extends the editor design in
  [compressed-note-edit-design.md](compressed-note-edit-design.md).
- A run on a note replaces the L1 and L2 of that note. A run on a folder
  replaces only the Summary note. It keeps the existing L1 and L2 of each
  note. Each replacement removes the user's edits, highlights, and comments
  in the document that it replaces. Compression works in the same way today.
  Section 6.1 gives the rules for failures.
- A change to L0 does not change L1, L2, or L3. The user runs the operation
  again to make new versions.

### 5.1 Tree indicator

The existing dot on a note in the tree shows that the note has a compressed
version, L1, or L2.

### 5.2 Summary note

- Its name is **Summary**. If an item with that name already exists in the
  folder, CHA adds a number to the name, as it does for session summaries,
  for example "Summary (2)".
- CHA identifies the Summary note of a folder by a link, as it identifies
  session summary notes. It does not identify the note by its name. CHA never
  replaces an ordinary item with the name "Summary". An ordinary note with
  that name is an included note, as any other note.
- If the user deletes the Summary note, the next folder operation makes a new
  one.
- It has the same menu rules as a session summary note: no **Rename**, no
  **Compress**, no **Show compressed**, and no summary menu item.
- It does not show in the search that characters use for session summaries.
- L1, L2, L3, and compression ignore it.
- In other respects it is a regular note. The user can attach it to a chat.
  ANNOTATIONS collects its highlights and comments.

### 5.3 ANNOTATIONS

ANNOTATIONS collects the highlights and comments of the original text (L0)
and of the Summary note. It does not collect the highlights and comments of
L1 and L2 in version 1.

## 6. Process

The operation follows the blocking compression in
[compressed-note-edit-design.md](compressed-note-edit-design.md):

- A modal progress dialog with Cancel blocks the application window for the
  full operation.
- CHA saves the current edits before it starts, and it makes the levels from
  the saved text.
- CHA processes the notes one at a time, in tree order.
- The model calls for one note run at the same time: L1, L2, and, in a folder
  operation, the L3 section. Thus a maximum of three calls run at the same
  time.
- After the last note, CHA makes the L3 overview call and writes the Summary
  note.
- Settings → Events records the result of each job.

### 6.1 Failures

- CHA replaces each level separately, and only when its new result is
  complete and saved. If a model call, the Markdown conversion, or the save
  fails, the old version stays with its edits, highlights, and comments.
  Example: in a run on a note, L1 succeeds and L2 fails. The new L1 replaces
  the old L1. The old L2 stays.
- If a call for a note fails, the operation continues with the next note.
  Events records each failure.
- If an L3 section or the L3 overview fails, CHA does not write the Summary
  note. The old Summary note stays.

### 6.2 Cancel

- Cancel stops the calls that run and starts no more calls.
- If Cancel stops a call, the old version of that level stays with its
  edits, highlights, and comments.
- The L1 and L2 results that CHA saved before Cancel stay.
- The Summary note does not change.

### 6.3 Next run

A folder operation skips the L1 and L2 that already exist. After a failure or
a Cancel, a new run makes only the missing L1 and L2. L3 always starts again
from the beginning.

## 7. Chats, export, sync, and ChaWeb

- Chats: L1 and L2 are for reading only. Chats continue to use the compressed
  text, or the original text when there is no compressed version.
- Folder export and import: L1, L2, and the exclusion flag go with their
  note, as the compressed version does.
- D1 sync: L1, L2, and the exclusion flag go with their note, as the
  compressed version does.
- R2 sync: it copies the full database, so it copies L1, L2, and the
  exclusion flag.
- ChaWeb: it shows the Summary note as a regular note. It does not show L1
  and L2.

## 8. Compressed version

The compressed version stays a separate version for chats. It is not a
reading level. A change of the compression prompt, for example to make the
compressed text shorter and better for model processing, is a separate task.

## 9. UI design

Each visible change needs a visual design and the user's approval before
implementation. This includes:

- the label of the menu item for summaries;
- the selection of L0, L1, L2, and the compressed version in the editor;
- the progress dialog and its text;
- the exclusion item in the note menu;
- the messages for notes that are too small or too large.

## 10. Known limits of version 1

- The model can change the text of L1.
- Until the compression prompt changes, L1 and the compressed version are
  almost the same text. A user who makes both pays twice.
- The default L1 prompt says that the reader is an LLM, because it is a copy
  of the compression prompt. The user can edit it.
- The model gets text only, so L1 and L2 have no pictures.
- One menu item makes all levels. The user cannot make only one level again.
  A folder operation does not replace existing L1 and L2. To make them again
  for all notes, the user runs the operation on each note.
- A run on a note removes the user's edits, highlights, and comments in its
  L1 and L2. A run on a folder removes them in the Summary note.
- After a partial failure, the L1 and L2 of a note can come from different
  runs. They can thus come from different L0 text or from different prompts.
  Events records the failure.
- The L3 overview sees only the sections, not the full text. It cannot show
  a link between notes if no section mentions it.
- A short real chapter under 4 KiB, for example a prologue, is not in L3.
- A note over 256 KiB without a compressed version stops L3.
- A folder operation for a large book can block the application for one hour
  or more. A book with 40 chapters needs about 120 model calls.

## 11. Out of scope for version 1

- Movement between levels, for example links from L3 to notes, or opening the
  same heading at a different level.
- Notes in subfolders for L3.
- Table support in the Notes editor.
- Automatic classification of notes without content, for example by Jev.
- Changes to the compression prompt.
- L1 and L2 in ChaWeb.
- Highlights and comments of L1 and L2 in ANNOTATIONS.
- Size targets in the prompts.
- Automatic regeneration when the original text changes.
- More than one note processed at the same time.
