# The current note in Chat

Draft PRD · November 17, 2026

A user opens *Trip plan* in Notes, switches to Chat, and asks, “Does this note
say when I can cancel the hotel?” Jev recognizes that the question refers to the
selected note. CHA adds a copy of *Trip plan* to the conversation, identifies it
by name in the chat, and sends the question to the intended character together
with the name of the note. The user can do this in one message without copying
a reference or issuing an attachment command.

This PRD covers the desktop application. ChaWeb does not change.

## Which note is current?

The current note is the note the user opened last in this window. The
application core keeps this selection, not the screen. The note stays current
until the user deletes it, closes the application, or changes the vault. Moving
between screens never clears it. The same rule applies in single view and in
split view.

| User action | Expected result |
| --- | --- |
| Open another note | That note becomes current. |
| Switch between Notes and Chat, change the layout, or open or create another conversation | The note stays current. Returning to Notes brings the user back to it. |
| Go to Settings, Events, or any other screen | The note stays current. Returning to Notes brings the user back to it. |
| Browse a folder or open a menu on another item | The note stays current. |
| Rename, move, or edit the current note | The same note stays current under its latest title and location. |
| Discard unsaved edits in the save-failure dialog | The edits are lost. The note stays current. |
| Delete the current note or a folder containing it, in this window or elsewhere, for example through sync | The selection becomes empty, even if other notes remain. The next note the user opens becomes current. |
| Close the application | The selection is lost. The next start begins with no current note. |
| Switch vaults, or replace the active vault with its R2 copy | The selection becomes empty. Other vault operations, such as an upload or a merge, keep the note current. |

Ordinary chat works with an empty selection, including in a vault with no
notes.

## Window title

The window title shows the current chat and the current note on every screen.
It no longer shows the vault name.

| State | Window title |
| --- | --- |
| A chat is open and a note is current | `CHA: Investing Strategies for Middle-Class Retirement — Trip plan` |
| A chat is open and no note is current | `CHA: Investing Strategies for Middle-Class Retirement` |
| No chat is open and a note is current | `CHA: Trip plan` |
| No chat is open and no note is current | `CHA` |

After the user renames the current note, the title shows the new name.

## When a message uses the note

Jev looks for a request to use the selected note. It does this for every
message while a note is selected, whatever the recipient: no recipient,
`@Name`, `@-`, or `/mcast`. The recipient never affects the note decision. For
a message with an explicit recipient, Jev decides only about the note, and the
message goes to the recipients the user chose. Commands that are not messages,
such as a Ref ID, a file path, or `/clear`, do not use the note. Jev gets the
message and the name of the selected note. Opening a note alone adds nothing to
a conversation. The user can discuss another subject while keeping the note
selected.

| User message | Expected behavior |
| --- | --- |
| “Pull the deadlines out of the note I just opened.” | Add the current note and answer from its contents. |
| “Seneca, quote the cancellation clause in my current note.” | Give Seneca the note and the question. |
| “@Seneca, quote the cancellation clause in my current note.” | Give Seneca the note and the question. |
| “/mcast What do you think of my current note?” | Give each character the note and the question. |
| “@- Keep my current note in this chat for later.” | Add the current note and record the self-note. No character answers. |
| “What does Packing list say about shoes?” while *Trip plan* is selected | CHA does not add *Trip plan*. |
| “Write a thank-you note to the hotel.” | Draft the requested text. The selected note stays out of the conversation. |
| “Note to self: call the hotel.” | Record the self-note through the usual chat behavior. The selected note stays out of the conversation. |
| “Make that answer shorter.” | Revise the earlier answer using the conversation, even if the user has since selected a different note. |
| Any message while no note is selected | Ordinary chat behavior. CHA does not look for a note request. |

An ambiguous “summarize this” is insufficient on its own to add the selected
note. The user can say “summarize this note” to identify the source. Automatic
attachment depends on Jev. When Jev is disabled or cannot classify the message,
CHA follows its ordinary chat behavior without automatically adding a note.

The note decision and the recipient decision are separate. A missing or invalid
note decision means that CHA adds no note. CHA uses a valid recipient decision
as it does today. CHA shows no new notice when the note decision fails.

While a note is selected, a message with an explicit recipient waits for Jev's
note decision before the answer starts. This delay, up to the Jev timeout, is
accepted.

## What the character reads

The selection when CHA receives the message determines which note the message
refers to. CHA receives the message after it finishes dictated text and after
it saves pending edits, so a note the user opens during that short time counts.
If the user opens *Packing list* while CHA is processing a question about
*Trip plan*, the character still answers from *Trip plan*.
This also applies when the conversation already contains copies of both notes.

CHA tells the character which note the message refers to. It adds the note's
name to the request for that message only. The conversation shows the message
as the user typed it, and later requests do not repeat the name.

CHA uses the same text as for a note added with a Ref ID: the compressed version
when the note has one, otherwise the full saved text. A chat never uses L1 or L2.

In split view, the open note can have unsaved edits when the user presses Send.
CHA saves them before it sends the message. If the save fails, CHA keeps the
message in the composer and shows the save error. This applies to every message,
including one that does not use the note.

The copy belongs to the conversation. The user can ask follow-up questions
about it after switching notes or deleting the original. Each time CHA uses the
current note, it compares the note with the chat's copy:

- If the chat has no copy, CHA adds one and shows “Added note …”.
- If the note's name or text changed, CHA refreshes the copy and shows
  “Updated this chat’s copy of note …”.
- If nothing changed, CHA keeps the copy and shows no notice.

In every case, the character gets the note's name with the message. A Ref ID
still refreshes the copy and shows its notice every time. Earlier messages and
answers remain as the user saw them.

A copy stays in the conversation until `/clear`, and every later request in
that conversation sends it. Copies of several notes add up, and a wrong Jev
decision also leaves a copy. This cost is accepted.

## When CHA cannot supply the requested note

If the selected note disappears before CHA can read it, or its text exceeds the
existing 256 KiB chat attachment limit, CHA explains the specific problem and
retains the question for retry. It waits for the user before requesting an
answer. An older chat copy or another note in the vault cannot silently supply
the answer to that request.
