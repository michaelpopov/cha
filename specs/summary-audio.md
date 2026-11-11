# L1 summary audio

## User flow

In the native app, open the actions menu of a note that has an L1 summary
and choose **Generate L1 audio**. The note must be read-only, as for
full-text audio. Choose a voice in the existing single-note audio dialog.
If L1 audio already exists, a successful run replaces it. A failed run
leaves the existing clips available.

Open the note in **L1** mode. Once audio is ready, right-click a paragraph
or heading and choose **Play from here**. Use the existing playback speed,
pause, and stop controls. Switching modes stops playback.

```text
│ Generate audio               │
│ Generate L1 audio            │
│ Compress                     │
│ Summarize                    │
```

**Generate audio** uses full text, including when L1 is open.
**Generate L1 audio** appears only for a note with a saved L1. It is
disabled while the note is editable. The existing dialog can use
“Generate L1 audio” as its accessible name. The editor uses the same
playback menu for either source. If L1 mode falls back to full text, the
available playback is for the full text shown in the editor.

## Generation and playback

Use the document ID as the audio key. Full text already uses the original
note ID. L1 uses the ID of its existing version row, where
`compressed = 1 AND level = 1`. Both audio tables already reference
`note_items.id`. This gives each document its own clips and at most one
pending attempt, with the existing schema and batch recovery.

1. When the user chooses **Generate L1 audio**, fetch `notes.get` with
   mode `l1`. Require a real L1 document and check the vault context before
   opening the dialog. If L1 has disappeared, show an error. Use the
   returned `document_id` for the dialog's audio status and clip count.
2. Use `noteAudioBlocks()` on that saved document, as for full text. Pass
   the document ID, voice, and blocks to the existing background start
   call. The native code validates that the ID is an L1 row of a read-only
   note. Resolve the owner with the same rules as note editing and apply
   the owner's read-only flag when starting and saving the audio. Reject
   generation for L2, compressed rows, folders, and ANNOTATIONS.
3. An L1 replacement keeps the row ID. To prevent a stale dialog from
   starting a job for the previous text, return a hash of the saved L1
   content with `notes.get` and send it back with the start call. Require
   it for L1 and compare it in the same transaction that accepts the job.
   On mismatch, tell the user to reopen the audio dialog. The hash needs
   no database column.
4. Keep the same provider settings, batching, Events, and media resource
   handling. Use the L1 document ID for status, clip requests, and
   `noteAudio.changed` notices. Identify L1 in the job's Events text.
   Enable the existing playback hook for full text and L1 on a read-only
   note, and give it the ID of the document actually loaded. Keep the
   tree's audio indicator and folder generation tied to full text.

## When L1 changes

- When a local summary run or D1 applies a different L1 document, abandon
  its pending attempt and remove its clips in the same transaction as the
  replacement. Apply the same cleanup before deleting an L1 row. Leave
  L1 audio available when a D1 record carries identical L1 content.
- When a note becomes editable, clear its L1 clips and abandon its L1
  attempt in the same transaction. This lets users edit freely and
  generate fresh audio after making the note read-only again. Highlights
  and comments saved locally while read-only leave the narration intact
  because the playback blocks omit comments and ignore formatting.
- Reuse the existing job retirement checks. A late worker must find its
  attempt before it saves clips. If the attempt was removed, discard the
  result. Send the existing audio-change notice for a locally invalidated
  L1 document so playback stops. Incoming D1 changes use the existing
  editor reload to stop playback and release its resources.

## Transfer

Local vault copies carry the audio tables. Folder export already
includes clips for each exported document row. On import, copy L1 clips
to the new L1 document ID as each version row is restored. D1 and R2
omit audio clips from transfers.

## Validation

- Use a fake provider to generate full-text and L1 audio for the same
  read-only note. Verify the provider receives different text and the
  editor plays the correct clips in each mode. Reopen the vault and
  verify both still play.
- Verify a missing L1, an editable note, and an L1 changed after the
  dialog opened cannot start an L1 job. Check that L1 mode with full-text
  fallback uses the full-text clips.
- Regenerate L1 during a pending audio job and let the old job finish.
  Verify the old result cannot save clips and full-text audio still
  plays. Cover incoming D1 replacement and removal, making the note
  editable, and deleting the note. Verify local highlights and comments
  on a read-only L1 preserve its audio.
- Export and import a folder with both sets of clips. Verify playback
  uses the new document IDs. Run the existing full-text audio tests.
