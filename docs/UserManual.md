# CHA User Manual

This manual describes the daily use of CHA: conversations, notes, voice, the
Library, and the ChaWeb browser client.

It does not describe installation or setup. The Settings screens, providers,
API keys, voices, vaults, synchronization, and the creation of forums,
characters and personas are subjects of the administrator guide. Where a
function needs setup, this manual says so in a line that starts with
**Setup:**.

## Contents

**Part 1 — Guide**

1. [About CHA](#1-about-cha)
2. [Concepts](#2-concepts)
3. [The main window](#3-the-main-window)
4. [First steps](#4-first-steps)

**Part 2 — Tasks**

5. [Sessions](#5-sessions)
6. [Conversations](#6-conversations)
7. [Material in a chat](#7-material-in-a-chat)
8. [What characters can use](#8-what-characters-can-use)
9. [Voice](#9-voice)
10. [Pictures](#10-pictures)
11. [Notes](#11-notes)
12. [Library](#12-library)
13. [ChaWeb](#13-chaweb)

**Part 3 — Reference**

14. [Commands and addressing](#14-commands-and-addressing)
15. [Keyboard](#15-keyboard)
16. [View menu](#16-view-menu)
17. [Limits](#17-limits)
18. [Messages](#18-messages)

---

# Part 1 — Guide

## 1. About CHA

CHA is a desktop application for conversations with AI characters. It runs on
macOS and Windows.

With CHA you can:

- Talk to one character, or to several characters in the same conversation.
- Dictate your messages and listen to the answers.
- Keep notes, import books, web pages and text files, and mark them up.
- Give a character a note, a text file, or a searchable Library to work with.

ChaWeb is a second client. It runs in a web browser, for example on a phone,
and it uses the same data. ChaWeb has fewer functions than the desktop
application. See [ChaWeb](#13-chaweb).

## 2. Concepts

**Vault.** A vault is one complete set of data: forums, characters, personas,
sessions and notes. One vault is active at a time. When you change the vault,
all lists in the window change.

**Forum.** A forum is a group of characters. Each forum has its own sessions
and its own folder in Notes. In a forum you always speak as one persona.

**Character.** A character is an AI participant. It has a name, a description
and a profile. It can also have a voice, a text style and pictures.

**Persona.** A persona is the identity that you speak as. The chat screen shows
it after **From:**.

**Session.** A session is one conversation in one forum. CHA stores it in the
vault.

**Entrance and Welcome.** Each vault has the built-in forum **Entrance**. It
contains one session, **Welcome**. CHA starts in Welcome. There you speak as
**Guest** with the character **Assistant**. CHA does not keep the Welcome
conversation after you close the application. You cannot create other sessions
in Entrance.

**Assistant.** Assistant is a built-in character. In the Welcome session it can
examine and change the CHA configuration when you ask it to. The administrator
guide describes this function.

**Notes.** Notes are documents in a folder tree. Each vault has its own notes.

**Library.** A Library is a set of cloud stores that are selected for one
forum. A cloud store holds a copy of your notes at a provider, in a form that
characters can search.

## 3. The main window

The window has a sidebar on the left and a main panel on the right.

### Sidebar

At the top of the sidebar there are two tabs:

- **Chat** shows the forums and the recent sessions, and the chat in the main
  panel.
- **Notes** shows the notes tree, and the note in the main panel.

When you go from Notes to Chat, CHA saves the note and its scroll position.
When you go back to Notes, the same note opens at the same position, in the
same text mode. When you go back to Chat, the same session shows. The text
in the two prompts stays. If the save of the note fails, the dialog with
**Retry**, **Stay** and **Discard** shows. See [Saving](#saving).

In split view, a tab changes the sidebar. The note and the chat both stay
visible.

The **Chat** tab has two lists:

- **Forums** shows all forums of the vault, Entrance included. Click a forum
  to see its sessions.
- **Recent Sessions** shows the used sessions of all forums. The session with
  the newest change is first. Each row shows the session name and the forum
  name. Click a row to open the session.

A dot on a session row shows activity: a character is generating an answer in
that session, or the session has a new answer that you did not see.

At the bottom of the sidebar there is the **Vault** list and the **Settings**
button.

### Menus

A forum row and a session row have a menu button at the right. A right-click
on the row opens the same menu. The Entrance forum and the Welcome session
have no menu.

In an open menu, use the Up and Down arrow keys, Home and End to move, and
Escape to close the menu.

### Main panel

The main panel shows the chat or a note. With **View → Split view**, it shows
the note on the left and the chat on the right.

The window title shows the name of the open session and the name of the open
note.

### Parts that you can resize

| Part | With the mouse | With the keyboard |
| --- | --- | --- |
| Sidebar width | Drag the divider. | Left and Right arrow keys on the divider. |
| Forums and Recent Sessions | Drag the divider between the lists. | Up and Down arrow keys, Home, End. |
| Notes and chat in split view | Drag the divider. | Left and Right arrow keys. |
| Message editor | Drag the bar above it. Double-click to reset. | Up and Down arrow keys, Home to reset. |
| Picture panel | Drag the divider. | Left and Right arrow keys. |

### Show and hide parts

The **View** menu shows and hides the sidebar, the toolbars, the picture
panel and other parts. See [View menu](#16-view-menu). CHA keeps these
selections for the next start.

### Change the vault

1. Click the **Vault** list at the bottom of the sidebar.
2. Select a vault.
3. If the vault has a password, the dialog **Open** *vault name* shows. Type
   the password and click **Open vault**.

CHA saves the open note, changes the vault, and shows the Welcome
conversation. The text that you typed in a prompt but did not send is removed.

Only one CHA process can open a vault at a time. If a different CHA process,
for example ChaWeb, has the vault open, CHA shows **Close other CHA processes
using this vault, then try again.** in the sidebar. For a protected vault,
the message shows in the password dialog. If CHA cannot open the vault for a
different reason, it also shows the reason. In these cases the active vault
does not change.

In ChaWeb, use the command `/open`. See
[Change and refresh the vault](#change-and-refresh-the-vault).

**Setup:** The administrator guide describes how to create and manage vaults.

### Events

Some tasks continue in the background: note audio, **Summarize**, session
summaries, and uploads to a store. CHA records their results as events.

To see the events:

1. Click **Settings** at the bottom of the sidebar.
2. Click **Events**.

Each event has a type (information, success, warning or error), a date and
time, and a message. CHA keeps the newest 1000 events.

## 4. First steps

**Setup:** Before the first conversation, the vault must have a forum with a
minimum of one character, and that character must have a provider. See the
administrator guide.

1. Start CHA. The Welcome conversation shows.
2. On the **Chat** tab, click a forum in the **Forums** list. The **Sessions**
   screen shows.
3. Click **New session**. An empty chat opens.
4. Type a message and press Enter.
5. Read the answer.

After the first message, CHA gives the session a name and adds it to **Recent
Sessions**.

---

# Part 2 — Tasks

## 5. Sessions

### The Sessions screen

To open the Sessions screen, click a forum in the **Forums** list.

The screen shows:

- **All forums** — goes to the list of forums.
- A row with the forum name and the names of its members.
- **New session**. The Entrance forum does not have this row.
- The sessions of the forum. The session with the newest change is first. Each
  row shows the name and the age, for example **Now**, **5m**, **3h**, **2d**,
  or a date.

CHA has no search function for sessions.

### Create a session

Use one of these methods:

- On the Sessions screen, click **New session**.
- In the sidebar, open the menu of a forum and click **Create new session**.

The chat opens immediately.

### Session names

A new session has the name **New session**. After your first message, CHA
makes a short name of a maximum of six words, in the language of your message.
If CHA cannot make a name, it uses the date and time as the name.

When you rename a session yourself, CHA does not change the name again.

### Sessions that you do not use

If you create a session and go away from it before you send a message or add a
file, CHA deletes that session.

### Open a session

Click the session in **Recent Sessions** or on the Sessions screen.

If the session cannot open, the screen shows **Session unavailable** with the
buttons **Retry** and **Browse sessions**.

### The session menu

The menu is available for the rows in **Recent Sessions**. It has these
commands:

| Command | Result |
| --- | --- |
| **Download** | Saves the conversation as a Markdown file. The suggested file name is the session name. |
| **Save summary** | Writes a summary of the conversation into Notes. See [Session summaries](#session-summaries). |
| **Clear audio cache** | Removes the stored audio of the answers in this session. |
| **Rename…** | Opens the dialog **Rename session**. Type the name and click **Rename**. |
| **Delete…** | Opens the dialog **Delete session?**. Click **Delete**. You cannot open a deleted session again. |

## 6. Conversations

### The chat screen

From top to bottom, the chat screen has:

- The chat toolbar, with the lists **Character**, **Model** and **Effort**, and
  the **Playback speed** control.
- The conversation.
- The prompt area: the message editor, the target list, the microphone button
  and the send button.
- A status row. It shows the forum, **From:** your persona, and **To:** the
  target.

### Send a message

1. Type the message in the editor.
2. Press Enter, or click **Send message**.

To start a new line in the message, press Ctrl+Enter. On macOS this is the
Control key. Shift+Enter does not start a new line. It sends the message.

After you send, the conversation moves so that your new message is at the top.
If you scroll up while an answer arrives, the view stays where you put it.
Scroll to the end to follow the answer again.

While a character generates an answer, you can type the next message but you
cannot send it.

### Stop an answer

Click **Stop generation**. This button replaces the send button while an
answer is in work.

The part of the answer that is already there stays in the conversation with
the status **Stopped**. If the message went to several characters, the
remaining characters do not answer.

### Select who gets your messages

The target is the receiver of your messages. The status row shows it after
**To:**.

To change the target, use the list **Choose message target** in the prompt
area:

- A character name — that character answers.
- **All characters** — each character of the forum answers, one after the
  other.
- **Self-notes** — CHA saves your messages in the conversation and no
  character answers. The characters see these messages later as part of the
  conversation.

The **Character** list in the chat toolbar also changes the target.

CHA keeps the last character that you select as the start target for new
sessions in that forum.

### Address one message to one character

Start the message with `@` and the name of a character:

```
@Anna what do you think?
```

That character answers this one message. The target does not change.

You can type the full name, one word of the name, or the start of the name.
Capital letters are not important. If the text agrees with more than one
character, CHA asks you to type more of the name.

To write a note to yourself in the middle of a conversation, use `@-`:

```
@- check this number tomorrow
```

To start a normal message with the character `@`, type `@@`.

### Send one message to several characters

Use the command `/mcast`:

```
/mcast What is your opinion?
/mcast @Anna, @Boris What is your opinion?
```

Without names, all characters of the forum answer. With names, only those
characters answer. All of them get the same conversation history, so no
character sees the answer of another character to this message before it
answers.

The conversation shows your message one time, with the answers below it.

### Automatic recipient detection

**Setup:** This function works only when recipient detection is configured.

When you send a message without `@` and without a command, CHA reads the
message and looks for the name or the nickname of a character.

| Your message | Result |
| --- | --- |
| Addresses one character by name or nickname | The message goes to that character. The target changes to that character. |
| Addresses the whole forum | The message goes to all characters. The target changes to **All characters**. |
| Is clearly a note to yourself | CHA saves it as a self-note. The target changes to **Self-notes**. |
| Has no clear receiver | The message goes to the current target. |

If the detection fails, CHA uses the current target and tells you in the
conversation.

### Model and effort

The chat toolbar shows the settings of one character: the character that
generates an answer, or the target, or the last speaker.

- **Model** selects the provider for this character. **Default** uses the
  provider of the character. The list is disabled when the forum has a default
  provider.
- **Effort** selects the reasoning effort: **Default**, **None**, **Low**,
  **Medium**, **High** or **Extra high**. **Default** uses the effort of the
  character. If the character has no effort setting, the effort of its
  provider applies.

The selections apply to that character in this session only.

Google (Gemini) and Anthropic models also use the effort. With **None**, CHA
sends no thinking settings to these models, and the model uses its own
default.

### Information and controls on a message

Below each message there is a row with information and buttons.

| Item | Function |
| --- | --- |
| Time | The time of the message. Point at it to see the full date. |
| Token count | The size of the request and the answer. Point at it to see the input and output tokens. |
| Globe | The request included data from the web. |
| **Copy to clipboard** | Copies the text of the message. |
| Speaker | Plays the answer. See [Voice](#9-voice). |
| Cover | Hides the conversation up to this answer from the characters. See below. |
| Trash | Deletes the answer and your message that caused it. |

An answer that failed shows as an entry with the name **Error** and the status
**Failed**. You can delete it with the trash button.

### Delete an answer

1. Click the trash button below the answer.
2. In the dialog **Delete response?**, click **Delete response**.

CHA deletes the answer and the message that caused it. You cannot undo this.

### Cover the start of a conversation

In a long conversation you can keep the early part on the screen but not send
it to the characters.

1. Find the last answer of the part that you want to cover.
2. Click the cover button below that answer.

The covered part moves into a box. The characters do not get it with later
messages. To move the limit, click the cover button on a different answer. To
remove the cover, click **Uncover transcript** on the last covered message.

The cover is temporary. CHA keeps it in memory only, and does not store it
with the session. The cover stays only while the session is open. When you
open a different session, change the vault, or close CHA, the cover is
removed. Then the characters get the full conversation again.

### Delete the full conversation

Send the command `/clear`.

CHA deletes all messages of the session immediately. It does not ask for
confirmation, and you cannot undo this.

### Progress of an answer

While an answer is in work, a line shows what the character does: it prepares
the answer, writes it, searches the web, or reads pages. When the character
uses the web, you can open the line to see the search texts and the sites.

### How answers show

Answers show with headings, lists, tables and code blocks. Web links in an
answer show as plain text. You cannot click them.

### If the connection to a session stops

A bar above the prompt area shows the state, for example **Reconnecting live
updates…**. If CHA cannot restore the connection, use the buttons **Retry**,
**Browse sessions** or **Return to start**. Your conversation is saved.

## 7. Material in a chat

You can give the characters a text file or a note. CHA keeps a copy in the
session and sends the text with each later message.

### Add a text file

Use one of these methods:

- Drag one file from the file manager onto the message editor.
- Send a message that contains only the full path of the file.

The conversation shows the message *Added file “name” to this chat.*

The file must be UTF-8 text of a maximum of 256 KiB.

To replace the copy with a newer version of the file, add the file again.

### Add a note

1. Click the **Notes** tab.
2. Open the menu of the note and click **Copy Ref ID**. The Ref ID is `##`
   and six characters.
3. Go to the chat. Paste the Ref ID as the only text of a message and send it.

The conversation shows the message *Added note “name” to this chat.*

Important points:

- The chat has its own copy. Later changes in the note do not change the copy.
  To refresh the copy, send the Ref ID again.
- If the note has a compressed version, the chat uses the compressed text. See
  [Summarize a note or a folder](#summarize-a-note-or-a-folder). A chat never
  uses L1, L2 or L3.
- The limit is 256 KiB of text.

With **View → Split view** you can read the note and use the chat at the same
time.

### Ask about the note that you read

**Setup:** This function works only when recipient detection is configured.

The note that you opened last on the **Notes** tab is the current note. It
stays the current note when you go to the **Chat** tab, and in split view.

In the chat, ask about the note in normal words. For example:

```
What does this note say about refunds?
```

You can also use words such as “this note”, “my current note”, or the name
of the note. Recipient detection decides if your message is about the
current note. If it is, CHA adds a copy of the note to the chat before the
character answers. The conversation shows *Added note “name” to this chat.*
If the chat already has an older copy, it shows *Updated this chat’s copy of
note “name”.* If the chat already has the same copy, no message shows.

Important points:

- The same rules apply as for [Add a note](#add-a-note): CHA uses the
  compressed text if the note has one, and the limit is 256 KiB of text. The
  text mode of the Notes screen is not important.
- “Summarize this” alone is not sufficient. Say which note you mean.
- If you change the vault or delete the note, there is no current note.
- ChaWeb does not have this function.

## 8. What characters can use

The characters decide when to use these sources. You do not start them with a
command. Ask your question in normal words.

Summaries and the Library are not available in the Welcome session.

### Summaries of earlier sessions

A character can search and read the saved summaries of other sessions in the
same forum. It cannot read the original messages of other sessions.

A session has a summary only after you use **Save summary**. See
[Session summaries](#session-summaries).

### The web

**Setup:** Web search and page reading must be configured and enabled for the
character.

A character can search the web and read pages. The progress line shows this
work. After the answer, the globe icon shows that the request included data
from the web.

### The Library

**Setup:** An administrator selects the cloud stores of a forum Library.

All characters of a forum can search the Library of that forum. See
[Library](#12-library).

## 9. Voice

### Dictate a message

**Setup:** The microphone button shows only when voice input is configured.

1. Click **Start voice input**.
2. Speak. The text shows in the message editor.
3. Click **Stop voice input**, or send the message.

Dictation is in English. It is in Russian while the **A→Я** button is on.

Only one prompt can use the microphone at a time: the chat prompt or the notes
prompt.

### Send by voice

Say the send phrase at the end of your message. The default phrase is
“over to you”.

CHA removes the phrase, waits one second, and sends the message. The
microphone stays on, so you can continue to speak after the answer.

### Spoken punctuation

| Say (English) | Say (Russian) | Result |
| --- | --- | --- |
| comma | запятая | `,` |
| period, full stop | точка | `.` |
| question mark | вопросительный знак, знак вопроса | `?` |
| exclamation mark, exclamation point | восклицательный знак, знак восклицания | `!` |
| new line | новая строка, с новой строки | A new line |

### Listen to an answer

**Setup:** The speaker buttons show only when voice output is configured.

1. Click the speaker button below an answer. CHA generates the audio and plays
   it. The next time, the audio starts without delay because CHA stores it.
2. To stop, click the button again. The next play continues from the position
   where you stopped.

To pause and continue, click the text of the conversation, or press Space when
the conversation has the focus.

CHA plays one audio at a time, in chat and in notes.

### Automatic audio for new answers

Click **Auto audio response** in the status row.

- Each new answer plays automatically, in the order of the conversation.
- The microphone starts, if it is not already on.

Click the button again to stop the automatic audio and the microphone. The
function also goes off when you open a different session or change the vault.

### Playback speed

Use **−** and **+** in the **Playback speed** control of the chat toolbar. The
range is 0.5× to 2.0×. The speed stays the same when you open a different
session.

### Stored audio

CHA stores the audio of each answer in the session. To remove it, use **Clear
audio cache** in the session menu. If the stored audio is gone, click the
speaker button to generate it again.

### Microphone during playback

While audio plays, CHA mutes the microphone and ignores what it hears. This
prevents the voice of a character from going into your message.

### Costs of voice

CHA records the use of the voice services: the audio of each answer, each
dictated message, and the audio of notes. When CHA knows the price of the
model, it also calculates an estimate of the cost. CHA writes this data into
its log. No screen shows it. The administrator guide tells where to find it.

### Type Russian with a Latin keyboard

1. Select **View → Russian**. The **A→Я** button shows in the prompt areas.
2. Click **A→Я**, or press Ctrl+Shift+Y.

While the button is on, CHA changes the Latin letters that you type into
Russian letters. Text that you paste does not change.

| Type | Result | Type | Result |
| --- | --- | --- | --- |
| `zh` | ж | `ja`, `ya`, `q` | я |
| `ch` | ч | `ju`, `yu` | ю |
| `sh` | ш | `jo`, `yo` | ё |
| `shh`, `w` | щ | `je` | э |
| `y` | ы | `'` | ь |
| `#` | ъ | `s+h` | сх |

The character `+` keeps two letters apart.

## 10. Pictures

**Setup:** A character must have picture files.

Select **View → Pictures**. A panel at the side of the chat shows a picture of
the target character or of the character that speaks.

If the character has more than one picture, click the picture to see the next
one. The picture also changes with each new answer.

## 11. Notes

### The Notes screen

Click the **Notes** tab to see the tree. Click a note in the tree to open it.

From top to bottom, the Notes screen has:

- The toolbar.
- The note.
- The prompt.

**View → Toolbar** and **View → Notes prompt** show and hide the toolbar and
the prompt.

### The tree

The first row of the tree is the vault. Below it are folders and notes. In
each folder the order is: folders, then notes in alphabetical order, then the
item **ANNOTATIONS**.

Each row has a menu button, **⋯**. A small dot on a note shows that the note
has other versions: a compressed version, L1, L2 or L3. See
[Versions of a note](#versions-of-a-note). A read-only note that has audio has
a tinted background. The tint shows the audio of the selected text mode. See
[Audio for a note](#audio-for-a-note).

Keyboard: use the arrow keys, Home and End to move in the tree. Press Enter to
open a note or a folder. Press Shift+F10 to open the menu of a row.

The tree has no command to move an item to a different folder.

### Kinds of items

| Item | What it is | What you can do |
| --- | --- | --- |
| Folder | Holds notes and folders. | All folder commands. |
| Note | A document. | All note commands. |
| **Forums** folder | CHA makes it. It has one folder for each forum, with the name of the forum. | **Export folder…** only. You cannot create, rename or delete items here. When a forum is deleted, its folder and the notes in it are deleted too. |
| Session summary note | The summary of a session, in the folder of its forum. | The note commands, but not **Rename**, **Summarize** and **Exclude from summaries**. |
| **Summary** note of a folder | **Summarize** on a folder makes it. See [Summarize a note or a folder](#summarize-a-note-or-a-folder). | The note commands, but not **Rename**, **Summarize** and **Exclude from summaries**. |
| **ANNOTATIONS** | CHA makes it. It lists the highlights and comments of the notes in one folder. | Open it and use **Copy Ref ID**. You cannot change it. |

### Create a note or a folder

1. Open the menu of a folder, or of the vault row.
2. Click **New note** or **New folder**.
3. Type a name and click **Create**.

A new note opens immediately.

In the field for a new note you can also type a web address or the full path
of a text file. CHA then creates the note and imports the page or the file
into it.

Rules for names: a maximum of 200 characters, no space at the start or the
end, and no two items with the same name in one folder. The name
**ANNOTATIONS** is reserved.

### Rename and delete

Open the menu of the item and click **Rename** or **Delete**.

CHA asks for confirmation before it deletes. It does not ask when the folder
is empty or the note has almost no text. When you delete a folder, CHA deletes
all notes and folders in it. You cannot undo a deletion.

### Write and format

Type in the note. The toolbar has these controls:

| Button | Commands |
| --- | --- |
| **Note text mode** | **Full text**, **L1**, **L2**, **L3**, **Compressed**. See [Versions of a note](#versions-of-a-note). |
| **Content** | **Upload** (import a text file), **Copy All** |
| **History** | **Undo**, **Redo** |
| **Edit** | **Bold**, **Italic**, **Underline** |
| **Highlight** | Marks or unmarks the selected text. |
| **Font size** | **Increase font size**, **Decrease font size** |
| **Playback speed** | **Decrease speed**, **Increase speed**, **Default speed** |
| **Headings** | **Title**, **Chapter**, **Section** |
| **Insert** | **List** (**Bullet list**, **Numbered list**), **Picture**, **Code** |
| **Find** | Opens the find bar. |
| **Jump to heading** | Shows the headings of the note. Click one to go there. |
| **Highlights** | Shows the highlights and comments of the note. Click one to go there. |

A right-click in the note opens a menu with **Cut**, **Copy**, **Copy JSON**,
**Paste**, **Bold**, **Italic**, **Highlight** and **Add comment**.

More points:

- CHA keeps the font size for the next start.
- A web address that you type or paste becomes a link. A click on a link opens
  it in the web browser.
- To add a picture, use **Insert → Picture**, or paste an image. The formats
  are PNG, JPEG, WebP and GIF.
- When you paste formatted text, CHA keeps the basic formatting and removes
  the rest. A table becomes lines of text.

### The notes prompt

The prompt below the note is a second way to add text. Type in the prompt and
press Enter. The text becomes a new paragraph at the cursor position. Ctrl+Enter
starts a new line in the prompt.

You can dictate into this prompt as you do in a chat.

The prompt does not send text to a character. It also has these special
functions:

- A web address imports that page. See [Import a web page](#import-a-web-page).
- The full path of a text file imports that file. See
  [Import a text file](#import-a-text-file).
- Text in the prompt becomes a comment. See [Comments](#comments).

### Saving

CHA saves a note automatically, five seconds after a change. It also saves
when you open a different note, go to a different screen, or close the window.
There is no save command.

If a save fails when you go away from a note, a dialog gives you three
choices: **Retry**, **Stay** in the note, or **Discard** the changes.

CHA remembers the scroll position of each note.

### Read-only notes

A read-only note is safe from accidental changes. To change the state, open
the menu of the note and click **Make read-only** or **Make editable**.

In a read-only note:

- You cannot change the text, and you cannot add text with the prompt.
- You can add and remove highlights and comments.
- You can generate and play audio.

CHA makes a note read-only when you import a file or a book into it, or a web
page into an empty note. Session summary notes and **Summary** notes of
folders are also read-only.

The read-only state applies to all versions of the note: the full text, L1,
L2, L3 and the compressed version.

### Find and replace

Click **Find** in the toolbar, or press Cmd+F (macOS) or Ctrl+F (Windows) in
the note.

- Type in **Find**. Press Enter for the next match and Shift+Enter for the
  previous match.
- Type in **Replace** and click **Replace** or **Replace all**.
- Select **Match case**, **Whole word** or **Regular expression** if
  necessary.
- Press Escape to close the bar.

The function searches the open note only. CHA has no search across notes.

### Highlights

1. Select text.
2. Click **Highlight** in the toolbar, or right-click and click **Highlight**.

Do the same again to remove the highlight. Highlights also work in read-only
notes.

### Comments

1. Type the comment in the prompt. Do not send it.
2. Right-click the paragraph that the comment is about.
3. Click **Add comment**.

The comment shows as a colored paragraph before that paragraph. To remove it,
right-click the comment and click **Delete comment**.

### ANNOTATIONS

When a note in a folder gets a highlight or a comment, CHA makes the item
**ANNOTATIONS** in that folder. It lists the highlights and comments of the
notes that are directly in the folder, by note and by chapter.

ANNOTATIONS collects only the highlights and comments of the full text. It
does not collect them from L1, L2, L3 and the compressed version.

Click the button at an entry to open the note at that highlight or comment.
The note opens in **Full text** mode.

### Import a text file

Use one of these methods:

- Open a note and click **Content → Upload**.
- Send the full path of the file in the notes prompt.
- Type the full path in the field of the **New note** dialog.

The file **replaces** the content of the note, and the note becomes read-only.
If the note still has the name **New note**, CHA uses the first heading of a
Markdown file as the name.

The file must be UTF-8 text of a maximum of 256 KiB. CHA reads Markdown files
(for example `.md`) as Markdown and all other files as plain text. In plain text, two or more empty
lines become one paragraph break.

### Import a web page

**Setup:** Page reading must be configured.

Use one of these methods:

- Send the web address in the notes prompt.
- Type the web address in the field of the **New note** dialog.

CHA adds the page at the end of the note. For a YouTube address, CHA imports
the transcript of the video. If the note was empty, CHA gives it the title of
the page and makes it read-only.

A status line shows the progress. The import continues if you open a different
note.

### Import a book

1. Open the menu of a folder and click **Import EPUB…**.
2. Select the `.epub` file.

CHA makes a folder with the title of the book. The folder has one read-only
note for each chapter, with a number at the start of the name. A chapter
without text and without pictures does not become a note.

### Export and import a folder

- **Export folder…** saves a folder with all its notes as one file. The file
  also contains the other versions of each note (compressed, L1, L2, L3), the
  **Exclude from summaries** setting, and the link to the **Summary** note of
  each folder.
- **Import folder…** adds such a file below the folder that you select.

### Export a folder as a book

**Export EPUB…** saves a folder as one EPUB book. You can read the book in an
e-book reader.

1. Open the menu of a folder and click **Export EPUB…**. The tree shows
   **Exporting EPUB…**.
2. In the system dialog, select the location of the file and click **Save**.

CHA saves the open note before the export. The book has these parts:

- The title of the book is the name of the folder. In L1, L2 or L3 mode, CHA
  adds ` (L1)`, ` (L2)` or ` (L3)` to the title and to the file name.
- Each note of the folder and of its subfolders is one chapter, in the order
  of the tree. The **Summary** note of a folder is the first chapter of that
  folder.
- Each subfolder is a group in the table of contents of the book.
- In L1, L2 or L3 mode, each chapter is the L1, L2 or L3 of its note. A note
  that does not have the version gives its full text. In the other modes, each
  chapter is the full text.
- ANNOTATIONS is not in the book.

The book contains the formatted text, the pictures and the web links.

### Versions of a note

A note can have more versions than its full text. A model makes these
versions:

| Version | What it is | Command that makes it |
| --- | --- | --- |
| **Full text** | The original text of the note. | — |
| **L1** | The text without the parts that are not necessary. It stays close to the original text. | **Summarize** |
| **L2** | A short form of the main ideas and the key facts, for example lists. It starts with the name of the note. | **Summarize** |
| **L3** | A short section about the note. The **Summary** note of the folder contains the L3 of each note. | **Summarize** |
| **Compressed** | A shorter text for chats. | **Summarize** |

The tree shows one row for each note. A dot on the row shows that the note
has other versions.

To open a version, use the **Note text mode** button at the start of the
toolbar:

1. Click **Note text mode**.
2. Click **Full text**, **L1**, **L2**, **L3** or **Compressed**.

The mode applies to all notes that you open after this. If the note does not
have the version of the mode, CHA shows the full text. The tooltip of the
button then shows, for example, *L1 mode; showing full text*. At each start,
CHA uses **Full text** mode.

Important points:

- You can edit each version, and add highlights and comments to it, as in the
  full text. The read-only state of the note applies to all versions.
- A change in one version does not change the other versions. When you change
  the full text, the other versions do **not** change. Make them again.
- A new version from **Summarize** replaces the old version, with its
  changes, highlights and comments.
- The text mode also controls **Generate audio** and **Export EPUB…**.

### Summarize a note or a folder

**Setup:** The vault must have a provider. The administrator can set the
provider and the effort that the dialog shows at the start.

**Summarize** makes the other versions of a note: the compressed version, L1,
L2 and L3. On a folder, it also makes the **Summary** note of the folder. You
can then read a book, or a set of articles, at the depth that you select:

1. The **Summary** note of the folder: an overview of the folder and the L3
   of each note.
2. The L2 of a note: the main ideas and the key facts.
3. The L1 of a note: the text without the parts that are not necessary.
4. The full text.

A chat uses the compressed version in place of the full text when you add the
note to the chat. Thus a long note uses less of the model's capacity.

L1, L2, L3 and the **Summary** note are in the language of the text.

To start **Summarize**:

1. Open the menu of a note or a folder and click **Summarize**.
2. Set the fields of the dialog:

   | Field | Value |
   | --- | --- |
   | **Provider** | The provider that makes the text. |
   | **Effort** | The reasoning effort. |
   | **Compressed**, **L1**, **L2**, **L3** | The versions to make. At the start, all are selected. |
   | **Overwrite** | When it is on, CHA replaces the versions that exist. At the start, it is on for a note and off for a folder. |

3. Click **Start**. **Start** is available when a provider and a minimum of
   one version are selected.

CHA saves the open note and starts the work. The dialog closes. The work
continues in the background. You can read and edit notes and use the chat.
[Events](#events) records the start, the result of each version, and the end.
There is no other progress indicator. When CHA saves a new version, the tree
and the open note show it.

Only one **Summarize** can run at a time. While one runs, the dialog shows
**Summarization is already running for “**_name_**”.**

There is no command to stop a running **Summarize**. When you change the
vault or close CHA, the work stops. The versions that CHA saved before stay.
Events records **Summarization cancelled for** _name_.

**Summarize** on a note makes the selected versions of the note. It does not
make a **Summary** note. The note must have a minimum of 4 KiB of text.

**Summarize** on a folder uses only the notes directly in the folder, not the
notes of subfolders. It ignores summary notes, notes with less than 4 KiB of
text, and notes with **Exclude from summaries**. CHA works on a maximum of
three notes at the same time. For each note, it makes the selected versions
in this order: compressed, L1, L2, L3.

With **Overwrite** off, CHA does not replace a version that exists. It makes
only the versions that are missing.

If **L3** is selected for a folder, CHA then writes the **Summary** note of
the folder. It starts with the name of the folder and an overview: the subject
of the folder, its main argument, and how the notes connect. Then it has one
section for each note: the name of the note as the heading, and the L3 of
the note. CHA writes the **Summary** note again also when all notes already
have an L3.

The **Summary** note contains a copy of the L3 of each note, with your
changes, highlights and comments in the L3. To change a section, edit the L3
of the note. Then use **Summarize** on the folder with only **L3** selected
and **Overwrite** off.

Rules for the **Summary** note:

- The **Summary** note is read-only. Each folder operation with **L3**
  replaces its content. The highlights and comments that you added in the
  **Summary** note itself are lost. If an item with the name **Summary**
  exists, CHA uses a name with a number, for example **Summary (2)**.
- CHA does not replace a note that you made with the name **Summary**. Such a
  note is an ordinary note.
- If you delete the **Summary** note, the next folder operation makes a new
  one.
- You can add the **Summary** note to a chat. The characters do not find it
  when they search the summaries of sessions.

To keep a note out of the folder operation, open the menu of the note and
click **Exclude from summaries**. A check mark shows in the menu. Click again
to remove it. The tree does not show this setting. **Summarize** on the note
itself ignores this setting.

If one step fails:

- A version that fails does not change. The old version stays. The work
  continues with the next step.
- If the L3 of a note or the overview fails, CHA does not write the
  **Summary** note. The old **Summary** note stays.
- Events records each failure.

Important points:

- A model rewrites the text. L1 can be different from the original text.
- The model always uses the full text of the note.
- L1, L2 and L3 have no pictures. A table becomes lines of text.
- A chat never uses L1, L2 or L3.
- When you change a note, the other versions do **not** change. Chats
  continue to use the old compressed text. Use **Summarize** again to make
  new versions.
- A very long note can be too large for the model. Then the step fails, and
  Events records the reason.
- A folder with many notes can take one hour or more.
- When the provider uses an API key of OpenAI or of Google (Gemini), CHA
  writes an estimate of the cost of each note into its log. No screen shows
  it.

### Audio for a note

**Setup:** The note audio service must be configured.

Audio is available only for read-only notes. The selected text mode controls
which text CHA reads aloud:

| Mode | **Generate audio** |
| --- | --- |
| **Full text** | Makes audio of the full text. |
| **L1** | Makes audio of the L1. A note without L1 gets no audio. |
| **L2**, **L3**, **Compressed** | Not available. |

To generate the audio:

1. Select **Full text** or **L1** in **Note text mode**.
2. Open the menu of a read-only note and click **Generate audio**.
3. Select a voice and click **Generate**. Below the list, the dialog shows
   information about the selected voice. To hear a short sample of an English
   or Russian voice, click **Listen**. Click **Stop** to stop the sample.
4. Wait. The work continues in the background. The result shows in
   [Events](#events).

**Generate audio** on a folder generates audio for the read-only notes
directly in that folder that have no audio. In L1 mode, it uses only the notes
that have an L1 and no L1 audio.

New audio replaces the old audio of the same text. When **Summarize** makes a
new L1, CHA deletes the audio of the old L1.

To listen:

1. Open the note.
2. Right-click a paragraph and click **Play from here**.

CHA plays the text that the editor shows: the full text or the L1. It plays to
the end of the note and marks the paragraph that it reads. Press Space to
pause and continue. To stop, right-click and click **Stop playing**. To change
the speed, use **Playback speed** in the toolbar. A change of the text mode
stops the audio.

CHA does not read code blocks, pictures and comments aloud.

### Session summaries

**Setup:** A compression provider must be selected.

1. On the **Chat** tab, open the menu of a session in **Recent Sessions**.
2. Click **Save summary**.

CHA writes a read-only note into the folder **Forums /** *forum name*. The note
has a short summary and a long summary. The chat shows a message with the name
of the note.

When you save the summary of the same session again, CHA replaces the content
of that note.

The characters of the forum can read these summaries. See
[What characters can use](#8-what-characters-can-use).

You cannot save a summary of the Welcome session.

## 12. Library

A Library lets the characters of a forum search large collections of notes.
The notes are copied to a cloud store at a provider (OpenAI or Gemini).

**Setup:** A cloud store API key is necessary. An administrator selects the
stores of each forum and manages the stores.

### Upload a folder to a store

1. On the **Notes** tab, open the menu of a folder.
2. Click **Upload to store**.
3. Select the **API key**.
4. If you want, type a description. A good description helps the characters to
   decide when to search the store.
5. Click **Upload**.

The dialog shows the progress. When the upload is complete, the dialog closes,
and [Events](#events) has an entry with the number of uploaded notes.

The result is a new store with the name of the folder. It contains one
document for each note in the folder and its subfolders.

To stop an upload, click **Cancel**. CHA removes the data that it already
uploaded.

Important points:

- The store is a copy. Later changes in the notes do not go to the store.
- You cannot add notes to an existing store. To update a store, an
  administrator must delete it. Then upload the folder again.
- Two stores cannot have the same name.
- Empty notes and **ANNOTATIONS** are not uploaded.
- While an upload runs, you cannot use the other commands of that folder.

### How the characters use the Library

Ask your question in a session of a forum that has a Library. The character
decides when to search. CHA tells the characters to search the Library before
they search the web.

The chat does not show that a Library search occurred, and it does not show a
list of sources. A source shows only if the character writes it in the answer.

## 13. ChaWeb

ChaWeb is the browser client of CHA. It is made for a phone, and it also works
in a desktop browser.

**Setup:** An administrator installs ChaWeb and gives you its address. ChaWeb
uses one vault at a time. Forums, characters and voices come from that vault.

### Screens

A bar at the bottom has two links:

- **Chats** shows the list of sessions, or goes back to the conversation that
  you used last.
- **Notes** shows the notes.

The conversation screen and the note screen have no back button. Use the back
function of the browser, or the bottom bar.

### Sessions

The session list shows the sessions of one forum, with the newest change
first.

- To see a different forum, tap the forum name at the top and select a forum.
  The list shows the names of the forum members.
- To open a session, tap it.
- To start a session, tap **New Session**, type a message and send it. ChaWeb
  creates the session when you send the first message.
- To delete a session on a phone, swipe the row to the left and tap
  **Delete**. With a mouse, point at the row, click the trash icon, and click
  **Delete**. ChaWeb does not ask for confirmation.

ChaWeb shows the built-in forum with the name **Welcome** and its session with
the name **Entrance**.

### Send a message

1. Type in the message field.
2. Press Enter. On a phone, use the send key of the keyboard.

Ctrl+Enter starts a new line. To get more space for a long message, tap
**Expand editor**.

The addressing rules are the same as in the desktop application: `@Name` and
`/mcast` work. See [Commands and addressing](#14-commands-and-addressing).
ChaWeb has no list to select the target.

A note Ref ID that you copied in the desktop application also works here. Send
it as the only text of a message to add the note to the chat.

To stop an answer, tap **Stop**.

To copy a message, tap the copy button below it.

### Voice mode

**Setup:** Voice input and voice output must be configured. The microphone
works only when ChaWeb is opened with a secure (HTTPS) address.

Tap **Voice mode** to start dictation and automatic audio together.

- When **Speak now** shows, speak. Your words go into the message field. You
  cannot type while voice mode is on.
- To send, tap **Send** or say the send phrase (default: “over to you”).
- The answer plays automatically. During that time the status shows
  **Microphone paused**. Then the microphone starts again.

The spoken punctuation words are the same as in the desktop application.

Voice mode goes off when you tap the button again, open a different
conversation, or put the browser in the background.

### Listen to one answer

Tap the **Read aloud** button below an answer. Tap again to stop. The next tap
continues from that position.

### Notes

Tap **Notes**, select a folder at the top, and tap a note.

In ChaWeb you can only read notes. You cannot change them. ChaWeb shows the
full text of a note. It does not show L1, L2, L3 and the compressed version. It
shows the **Summary** note of a folder as a regular note.

### Change and refresh the vault

**Setup:** The commands that use R2 need R2 credentials in the active vault.
The commands do not work with protected vaults. See the administrator guide.

ChaWeb has no **Vault** list. Type these commands in the session
**Entrance** of the forum **Welcome**:

| Command | Result |
| --- | --- |
| `/open <name>` | Opens the vault `<name>`. If the server does not have this vault, ChaWeb gets it from R2 first. |
| `/download` | Shows a warning. It changes nothing. |
| `/download confirm` | Replaces the active vault with its copy in R2. |

- To refresh a vault that is not active, open it first. Then type
  `/download` and `/download confirm`.
- You can also ask the Assistant in normal words, for example “Refresh this
  vault”. It tells you the command to type. It does not do the command.
- For a vault on the server, capital letters in the name are not important.
  For a vault from R2, type the name of its R2 file without `.sqlite3`, with
  the correct capital letters. The result notice shows the vault name. Use
  that name the next time.
- Only one vault command can run at a time.

> **WARNING:** `/download confirm` replaces the complete vault on the server:
> the configuration, the sessions and the notes. Changes that are not in the
> copy in R2 are lost. Only the administrator can get them back from a backup
> file. The stored audio of the vault is lost.

> **CAUTION:** `/open` and `/download confirm` stop all answers and all work
> in progress, in all browsers that use this ChaWeb. `/open` does not ask for
> a confirmation.

During and after a command:

- ChaWeb shows a notice when the command starts, and a notice with the
  result. Each vault has its own Welcome conversation. The start notice is
  in the Welcome of the old vault, and the result notice is in the Welcome
  of the new vault.
- During a transfer from R2, ChaWeb does not answer. This can take some
  minutes. If the page shows an error, wait. The page recovers after the
  transfer.
- After the vault changes, each open ChaWeb page goes to Welcome. Text that
  you typed but did not send is lost. Audio playback and voice input stop.

### If the connection stops

ChaWeb shows **Reconnecting** and tries again by itself. If that fails, an
error message and a **Retry** button show.

If ChaWeb shows **Send status unknown. Check the conversation before sending
again.**, look at the conversation first. Send the message again only if it is
not there.

### What ChaWeb does not have

- Rename and download of sessions, session summaries.
- Delete and cover for single answers.
- The target list, and the **Model** and **Effort** lists.
- Text files as chat material.
- Questions about the note that you read. See
  [Ask about the note that you read](#ask-about-the-note-that-you-read).
- Creation and change of notes.
- **Summarize**, audio and export of notes.
- Pictures.
- Settings, and the **Vault** list. To change the vault, use the commands in
  [Change and refresh the vault](#change-and-refresh-the-vault).
- The upload of a vault to R2.

---

# Part 3 — Reference

## 14. Commands and addressing

Type these at the start of a chat message.

| Input | Result |
| --- | --- |
| `text` | Goes to the current target. |
| `@Name text` | Goes to one character, for this message only. |
| `@- text` | Saves a self-note. No character answers. |
| `@@text` | Sends `@text` to the current target. |
| `/mcast text` | Goes to all characters of the forum. |
| `/mcast @A, @B text` | Goes to the listed characters. |
| `/clear` | Deletes all messages of the session, with no confirmation. |
| `##xxxxxx` (a Ref ID, as the only text) | Adds that note to the chat. |
| A full file path (as the only text) | Adds that text file to the chat. |
| `/open <name>` | ChaWeb, Welcome only: opens a vault. See [Change and refresh the vault](#change-and-refresh-the-vault). |
| `/download`, `/download confirm` | ChaWeb, Welcome only: replaces the active vault with its copy in R2. See [Change and refresh the vault](#change-and-refresh-the-vault). |

Rules for `@Name`:

- The name ends at the first space. A punctuation mark directly after the name
  is ignored: `@Anna, hello` works.
- CHA looks for the name in this order: the full name; one full word of the
  name; the start of the name or of one of its words.
- Capital letters are not important.
- Nicknames work only with automatic recipient detection, not with `@`.

## 15. Keyboard

| Where | Keys | Function |
| --- | --- | --- |
| Message editor, notes prompt | Enter | Send |
| Message editor, notes prompt | Ctrl+Enter | New line |
| Prompt areas | Ctrl+Shift+Y | Latin to Russian input on and off |
| Conversation | Space | Pause and continue the audio |
| Message editor | Shift+Enter | Send (not a new line) |
| Note | Cmd+B, Cmd+I, Cmd+U (macOS) | Bold, italic, underline |
| Note | Cmd+Z (macOS) | Undo |
| Note | Cmd+F or Ctrl+F | Open the find bar |
| Find bar | Enter, Shift+Enter | Next match, previous match |
| Find bar | Escape | Close the bar |
| Note with audio | Space | Pause and continue the audio |
| Notes tree | Arrow keys, Home, End | Move |
| Notes tree | Enter | Open the note or folder |
| Notes tree | Shift+F10 | Open the menu of the row |
| Menus | Up, Down, Home, End, Escape | Move, close |
| Dialogs | Escape | Cancel |

## 16. View menu

| Command | Shows or hides |
| --- | --- |
| **Split view** | The note and the chat side by side. |
| **Sidebar** | The sidebar. |
| **Pictures** | The character picture panel in the chat. |
| **Toolbar** | The toolbar of the Notes screen. |
| **Chat toolbar** | The toolbar of the chat screen. |
| **Notes prompt** | The prompt below a note. |
| **Russian** | The **A→Я** button in the prompt areas. |

## 17. Limits

| Item | Limit |
| --- | --- |
| Chat message | 32,768 bytes |
| Text file for a chat or a note | 256 KiB, UTF-8 text |
| Note as chat material | 256 KiB of text |
| Note size | 12 MiB |
| Picture in a note | 1 MiB; PNG, JPEG, WebP or GIF |
| EPUB book | 16 MiB |
| Note for **Summarize** | A minimum of 4 KiB of text |
| **Summarize** on a folder | Three notes at the same time |
| Font size in notes | 10 px to 30 px, in steps of 2 px |
| Note audio | 1,000 paragraphs; 5,000 characters in one paragraph |
| Name of a note, folder or session | 200 characters |
| Description of an OpenAI store | 512 characters |
| Playback speed | 0.5× to 2.0× |
| Events | The newest 1,000 |

## 18. Messages

| Message | What to do |
| --- | --- |
| **Unknown character @x. Characters in this forum: …** | Use one of the names in the message. |
| **Ambiguous character @x: matches …** | Type more of the name. |
| **Generation in progress; use the Stop button** | Wait for the answer, or click **Stop generation**. |
| **Recipient detection failed. Sent to …** | No action is necessary. CHA used the current target. |
| **Close other CHA processes using this vault, then try again.** | A different CHA process, for example ChaWeb, has the vault open. Close that process, or select a different vault. |
| **Session unavailable** | Click **Retry**, or click **Browse sessions** and open the session again. |
| **Waiting for another session to close** | Wait. CHA keeps a limited number of sessions open at the same time. |
| **This file is too large to add to a chat. The limit is 256 KiB of text.** | Use a smaller file. |
| **Choose a UTF-8 text file.** | The file is not text, or it has a different encoding. Convert it to UTF-8. |
| **That file was not found.** | Make sure that the path is complete and correct. |
| **That note was not found.** | Copy the Ref ID again from the menu of the note. |
| **This note is too large to add to a chat. The limit is 256 KiB of text.** | Use **Summarize** to make a compressed version, or divide the note. |
| **Cached audio is no longer available. Generate it again.** | Click the speaker button again. |
| **Microphone access was denied. Allow it in System Settings and try again.** | Give CHA access to the microphone in the system settings. |
| **This note is read-only. Make it editable first.** | Open the menu of the note and click **Make editable**. |
| **This folder is managed by CHA.** | You cannot change the **Forums** folders. Use a different folder. |
| **An item with this name already exists here.** | Use a different name. |
| **This note is under 4 KiB and does not need summaries.** | No action is necessary. |
| **No eligible notes to summarize.** | No note directly in the folder has 4 KiB of text, or all such notes have **Exclude from summaries**. |
| **All selected outputs already exist.** | Turn on **Overwrite**, or select different versions. |
| **Summarization is already running for “**_name_**”.** | Wait until [Events](#events) shows **Summarization finished for** _name_. |
| **Could not start summarization. Try again.** | Click **Start** again. |
| **This folder has no notes to export.** | The folder and its subfolders have no notes. |
| **This folder has no L1 text to export.** (or **L2**, **L3**) | No note of the folder has that version. Select **Full text** mode, or use **Summarize** first. |
| **Could not export the note “**_name_**”.** | The note was deleted, or it is damaged. |
| **This folder is too large to export as one EPUB.** | Export the subfolders one at a time. |
| **Select a compression provider in Notes settings.** | Setup is necessary. See the administrator guide. |
| **Page reading is not configured. …** | Setup is necessary. See the administrator guide. |
| **Save summary is only available for configured forums.** | You cannot save a summary of the Welcome session. |
| **The summary could not be saved. Check Settings → Events and try again.** | Read the newest entries in [Events](#events). |
| **A store with this name already exists.** | Rename the folder, or ask an administrator to delete the old store. |
| **This folder has no notes to upload.** | Add notes with text to the folder. |
| **Vault commands are available in ChaWeb Welcome.** | In the desktop application, use the **Vault** list. |
| **Type /download or /download confirm.** | Type the command again, in this form. |
| `Type /open <name>.` | Type the name of the vault after `/open`. |
| **A vault command is running. Try again when it finishes.** | Wait for the result notice. |
| **Open failed for vault '…'. R2 database object '…' was not found. Remote lookup requires the R2 file name without .sqlite3, in its exact case.** | Type the name of the R2 file with the correct capital letters. Ask the administrator for the name. |
| **Download failed for vault '…'.** … / **Open failed for vault '…'.** … | Read the reason in the message. If you cannot correct it, ask the administrator. |
| **The application is unavailable. Restart the daemon.** | Ask the administrator to start ChaWeb again. |
