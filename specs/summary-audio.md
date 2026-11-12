# L1 summary audio

The user made these decisions on November 11, 2026:

- The selected note text mode controls **Generate audio** on notes and
  folders. Full text makes full-text audio, L1 makes L1 audio, and L2 and
  Compressed have no audio.
- In L1 mode, a note that has no L1 gets no audio.
- The tree tint shows the audio of the selected mode.

## User flow

The selected mode is the global, session-only value of the editor's mode
menu, `AppState.noteTextMode`. It applies also when no note is open.
Audio generation and playback only read this mode. They do not change the
mode or the mode button, including its icon and title.

| Mode | **Generate audio** on a note | **Generate audio** on a folder | Tree tint |
| --- | --- | --- | --- |
| Full text | As today. | As today: read-only notes directly in the folder that have no full-text audio. | Full-text audio, as today. |
| L1 | Enabled for a read-only note that has an L1. | Read-only notes directly in the folder that have an L1 and no L1 audio. Other notes are skipped. | L1 audio. |
| L2, Compressed | Disabled. | Disabled. | None. |

The menu keeps one **Generate audio** item with its current label and
position. The audio dialogs do not change.

```text
Full text or L1 mode               L2 or Compressed mode
┌──────────────────────────────┐   ┌──────────────────────────────┐
│ Make editable                │   │ Make editable                │
│ Generate audio               │   │ Generate audio               │  ← disabled
│ Compress                     │   │ Compress                     │
│ …                            │   │ …                            │
└──────────────────────────────┘   └──────────────────────────────┘
```

In Full text mode, the item is disabled while a full-text job of the note
runs, as today. In L1 mode, the menu does not check for a running L1 job.
If an L1 job is already running, the start fails with the existing "Audio
generation is already running…" message. In every mode, the item is
disabled for an editable note, as today.

If L1 audio already exists, a successful run replaces it. A failed run
leaves the existing clips available.

To listen, open the note. Once audio is ready, right-click a paragraph or
heading and choose **Play from here**. Use the existing playback speed,
pause, and stop controls. Switching modes stops playback. Playback uses
the document that the editor shows: the full text or the L1. If a mode
falls back to full text, the available playback is for the full text.

## Generation and playback

Use the document ID as the audio key. Full text already uses the original
note ID. L1 uses the ID of its existing version row, where
`compressed = 1 AND level = 1`. Both audio tables already reference
`note_items.id`. This gives each document its own clips and at most one
pending attempt, with the existing schema and batch recovery.

Full-text generation does not change. In L1 mode:

1. The tree passes the read-only target notes that have an L1 to the
   existing dialog. While the dialog loads, it finds the L1 document ID of
   each target with `notes.get` mode `l1` and checks the vault context. A
   result that is the full text means that L1 has disappeared. A folder run
   skips that note. The single-note dialog shows an error. The dialog uses
   the document ID for the clip count and, in a folder run, for the pending
   status.
2. When the user clicks **Generate**, the dialog fetches `notes.get` with
   mode `l1` again for each target, as its `prepare` step reads full text
   today. It requires the same `document_id`. Otherwise the single-note
   dialog shows an error and tells the user to reopen the dialog, and a
   folder run skips the note. Use `noteAudioBlocks()` on this document, as
   for full text.
3. Pass the document ID, voice, and blocks to the existing background start
   call, with the existing only-if-missing flag in a folder run. The native
   code validates that the ID is an L1 row of a read-only note. Resolve the
   owner with the same rules as note editing and apply the owner's
   read-only flag when starting and saving the audio. The only-if-missing
   check uses the clip count and the pending attempt of the L1 row. Reject
   generation for L2, compressed rows, folders, and ANNOTATIONS.
4. Keep the same provider settings, batching, Events, and media resource
   handling. Use the L1 document ID for clip requests, pending status, and
   `noteAudio.changed` notices. Enable the existing playback hook for full
   text and L1 on a read-only note, and give it the ID of the document
   actually loaded.

All audio Events texts and the "already running" error name an L1 job as
`L1 of “<note name>” (note <note ID>)`, for example `Audio ready for L1 of
“Chapter 3” (note 42): 40 clips saved.` This includes the "Audio skipped"
event of a folder run. The name is the note's name when the job starts, as
for full text. Do not use the name of the L1 row: it is a copy from the
time CHA created the row. The ID is the note ID from the L1 row's
`parent_id`, not the L1 row ID.

If a new L1 is saved between the read in step 2 and the start transaction,
the job can save clips of the old text. Full text has the same short gap
with an incoming D1 `note.update`. After the start, the cleanup in the next
section handles each L1 change.

## Tree tint

Add `has_l1_audio` to `NoteItem`, its JSON, `resources/dto.yaml`, and the
note guard in `webapp/src/api/noteGuards.ts`. It is true when the note's L1
row has clips:

```sql
EXISTS (SELECT 1 FROM note_items v JOIN note_audio a ON a.note_id=v.id
        WHERE v.parent_id=n.id AND v.compressed=1 AND v.level=1)
```

`has_audio` keeps its meaning: the note has full-text audio. ChaWeb uses
the same `NoteItem` JSON and guard.

`NotesTree` receives the selected mode. The row tint is `has_audio` in Full
text mode and `has_l1_audio` in L1 mode, always with `read_only`, as today.
L2 and Compressed modes show no tint. The tree item key adds
`has_l1_audio`. A mode change only renders the tree again, because the list
already carries both flags. The existing refresh on `cha-note-audio-changed`
also covers L1 jobs.

Known effect: in L1 mode, a note with no L1 shows its full text and can
play its full-text clips, but its row has no tint. L2 and Compressed modes
have the same effect for full-text fallback.

## When L1 changes

- When a local Summarize run writes L1, abandon the pending L1 attempt and
  remove the L1 clips in the same transaction as the write. Do the same for
  each D1 record that writes or deletes the L1 row: `note.update` with any
  `l1_content`, also null, and `note.level` for `l1`. D1 does not compare
  the content. `apply_note_update()` uses the same rule for full-text
  audio. Remove the attempt before deleting an L1 row, because
  `note_audio_attempts` uses `ON DELETE RESTRICT`.
- When a note becomes editable, abandon its L1 attempt in the same
  transaction, as for full text. Keep the L1 clips, as for full text. This
  applies to the tree command and to D1 `note.read_only`. Highlights and
  comments saved locally while read-only leave the narration intact
  because the playback blocks omit comments and ignore formatting.
- Reuse the existing job retirement checks. A late worker must find its
  attempt before it saves clips. If the attempt was removed, discard the
  result. After a local Summarize run writes L1, send the existing
  audio-change notice for the L1 document so playback stops. A note that
  becomes editable needs no notice, because the playback hook stops when
  the note is no longer read-only. Incoming D1 changes use the existing
  editor reload to stop playback and release its resources.

Known effect: if the user edits L1 while the note is editable and then
makes the note read-only again, **Play from here** can play clips of the
old text. Generate audio again in L1 mode to replace them. Full text has
the same effect.

## Transfer

Local vault copies carry the audio tables. Folder export already
includes clips for each exported document row. On import, copy L1 clips
to the new L1 document ID as each version row is restored. D1 and R2
omit audio clips from transfers.

## Validation

- Use a fake provider. Generate audio for the same read-only note in Full
  text mode and in L1 mode. Verify the provider receives different text
  and the editor plays the correct clips in each mode. Reopen the vault and
  verify both still play.
- Verify the menu in each mode: L2 and Compressed disable **Generate
  audio** on notes and folders; L1 disables it on a note with no L1 and on
  an editable note.
- Run a folder in L1 mode. Verify it skips notes with no L1, editable
  notes, and notes with L1 audio or a running L1 job, and that it does not
  change full-text audio. Verify a folder run in Full text mode works as
  today.
- Verify the tint in each mode, after a mode change, and after an L1 job
  finishes.
- Verify an L1 removed after the dialog opened cannot start a job, and an
  L1 replaced after the dialog opened starts a job with the new text.
  Check that L1 mode with full-text fallback plays the full-text clips.
- Verify the Events texts, also "Audio skipped", and the "already running"
  error name an L1 job with the note's name and the original note ID.
- Regenerate L1 during a pending audio job and let the old job finish.
  Verify the old result cannot save clips and full-text audio still
  plays. Cover incoming D1 replacement and deleting the note. Verify local
  highlights and comments on a read-only L1 preserve its audio.
- Make the note editable while an L1 job runs. Verify the job is abandoned
  and the L1 clips stay. Repeat with an incoming D1 `note.read_only`
  record.
- Let D1 remove L1 (null `l1_content`) while an L1 job runs. Verify the
  record applies, the L1 row and its clips are gone, and the job is
  abandoned.
- Restart CHA while an L1 job runs. Verify recovery saves the clips to the
  L1 row.
- Export and import a folder with both sets of clips. Verify playback
  uses the new document IDs. Run the existing full-text audio tests.
