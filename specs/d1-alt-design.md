# D1 queue synchronization: alternative design

Date: November 9, 2026

Status: technical design. It replaces the approach in
`specs/d1-sync-design.md`. Nothing is implemented. This version includes the
changes from the review in `specs/d1-alt-review.md`.

## 1. Purpose and scenario

One person keeps copies of a vault on several machines. The person works on
one machine at a time, never on the same vault on two machines at once. When
the person starts to work on the next machine, the vault must show the state
that the person left on the previous machine (R33).

Each application writes change records into queues in Cloudflare D1. Each
other application reads its own queue and applies the records to its local
database (R1 to R7).

CHA is a personal toy application. Simplicity has priority over completeness.
Bad results in corner cases are acceptable (R33). Section 13 lists them.

Terms:

- **D1**: the Cloudflare SQL database that holds the queues.
- **The application**: one running CHA instance on one machine: the macOS
  application, the Windows application, or `cha-daemon` with ChaWeb on Linux.
- **The Worker**: an object inside the application, with its own thread, that
  sends and receives change records.
- **Change record** (or record): a JSON text that describes one change.
- **Piece**: one part of a record that is too large for one D1 row.
- **Queue table**: a D1 table that holds the records for one vault on one
  machine.
- **Local queue**: a table in the local vault database that holds the records
  that this machine did not send yet.
- **Connected vault**: a vault that has D1 credentials, on a machine that has a
  machine name.

## 2. Requirements

These are the user's requirements. "Approved proposal" means that the user
approved a proposal during the requirements discussion. "Review answer" means
that the user's answer is recorded in `specs/d1-alt-review.md`, section 8.

| ID | Requirement | Source |
|---|---|---|
| R1 | Multiple instances of the application work with their local copies of SQLite databases. | User |
| R2 | Each instance of the application is registered in D1. | User |
| R3 | Each instance has its own queue in D1. | User |
| R4 | Each instance periodically polls its queue for change records. | User |
| R5 | When there are change records in the queue, the instance pulls them and applies the changes to the local database. | User |
| R6 | After a change is applied, its record is deleted from the queue. R40 changes this to groups. | User |
| R7 | When a user executes operations that change data, the instance writes change records to the queue of each other registered instance. | User |
| R8 | The Worker inside each application reads change records from its queue and applies them to the local database, as they come. | User |
| R9 | No data versions, no checks, and no comparisons. The database holds no states or versions of the data. | User |
| R10 | An application puts a record directly into the queues of the other applications. It does not read the records that it wrote. | User |
| R11 | If the HTTP request that writes a record succeeds, that is enough. D1 does not track what each application sent. | User |
| R12 | A configuration change sends only the changed rows. | User |
| R13 | If a note is updated, its audio is removed, whatever the text content is. | User |
| R14 | An invalid configuration record is skipped, and the application writes a warning. | User |
| R15 | If an incoming item has a name that already exists in the folder, it gets a name suffix. | User |
| R16 | A record for an item in a folder that does not exist is skipped, with a warning. | User |
| R17 | The macOS, Windows, and Linux applications all take part. | User |
| R18 | All content moves, except audio and event history. | User |
| R19 | The Worker is an object with its own thread or threads in the process of the application. | User |
| R20 | The open vault synchronizes with the copies of the same vault on other machines. Records for a vault wait in D1 until the application opens that vault. | User |
| R21 | A new instance registers itself in D1 and creates its queue table. Its first copy of the vault comes from R2. After that, it gets change records. R36, R46, and R51 change the order. | User |
| R22 | If a write to D1 fails, local changes stay, and the application writes a log message. A record goes first to a local queue. The Worker writes it to the queues in D1 and then removes it from the local queue. | User |
| R23 | Duplicated records and partial writes are accepted. No sequence IDs and no cross-queue machinery. One REST request writes one record to all queues. The Worker tries again at its next loop, with no limit. | User, approved proposal |
| R24 | Bad timing at registration is accepted. | User |
| R25 | Items are named by name or path. R48 and R54 change this: only notes are named by path. | User |
| R26 | R2 upload is blocked while the local queue has records. There is no removal step at R2 download. R47 adds the "changed" flag to the block. | User |
| R27 | A prompt and its response move together. The Worker writes the pair after the response is complete. | User |
| R28 | Incoming chat entries go at the end, with the next free numbers. | User |
| R29 | Nothing moves for a chat until it has its name and its first response. At that moment, the application writes records for all that the chat has. After that, each change writes its own record. A receiver creates a chat when a record names a chat that it does not have. | User, approved proposal |
| R30 | Replaced by R53. | User |
| R31 | API keys and R2 keys move through D1 like other configuration rows. | User |
| R32 | The note save operation does not change. An incoming change can override local editing. | User |
| R33 | One person, one machine at a time. Simplicity wins over completeness. | User |
| R34 | Protected vaults synchronize like other vaults. Their records are plain text in D1. | User |
| R35 | When a record changes a chat that is open on the screen, the screen shows the change. | User |
| R36 | A machine name is in `app.toml`. The D1 credentials are in the vault, next to the R2 credentials. The Worker finds the recipients from the list of queue tables of the vault. There is no registry table. | Approved proposal |
| R37 | The Worker loop runs every 30 seconds. At vault open, the loop runs once before the screens load. At application close, the Worker tries once to send. | User, approved proposal |
| R38 | Changes that the Worker applies are not user operations, so they write no records. | Approved reading of R7 |
| R39 | When a record changes the open note, the note on the screen updates. The notes tree also shows incoming changes at once. ChaWeb shows a change when its note page opens again. | User, approved reading |
| R40 | The Worker reads up to 50 records in one request, applies them in order, and deletes them in one request. | Approved proposal |
| R41 | An incoming record that cannot be applied is skipped. The Worker writes a warning and deletes the record from the queue. | User |
| R42 | Note create, rename, delete, read-only, and compress write their record at once. A text change moves through the "changed" flag (R47). | Approved reading |
| R43 | Settings has a D1 sync screen. "Save D1 credentials" connects the vault. "Disconnect from D1" deletes the queue table of this machine. The UI is approved as shown in the discussion. | User, approved UI |
| R44 | A vault with no D1 credentials writes no records. A change of the D1 credentials writes no record. | Approved proposal |
| R45 | Disconnect removes the D1 credentials on this machine, deletes its queue table, and empties the local queue. | Approved proposal |
| R46 | When D1 is full, the Worker deletes the largest queue table and tries the write again. The application makes a queue table only at "Save D1 credentials" and after an R2 download. If the queue table of this machine is missing at vault open, the Worker stops and writes an error. The user must download the vault from R2 again. R51 adds an exception for `cha-daemon`. | User, approved proposal |
| R47 | A "changed" flag on notes. Save and content load set it. In each loop, the Worker writes a record for each flagged note to the local queue and clears the flag. A scroll change alone writes no record. The R2 upload block includes the flag. | User, approved proposal |
| R48 | Forums, characters, personas, providers, styles, voices, and API keys keep their IDs as stored. Records carry these IDs. If two machines make an item with the same ID before they synchronize, the result is accepted. | User decision |
| R49 | When a `note.update` record names a note that does not exist, the receiver creates the note from the record. | User decision |
| R50 | The rows that one configuration action changes move together in one record. The whole configuration is never sent. | Review answer 3 |
| R51 | `cha-daemon` makes its queue table at start when the table is missing. | Review answer 1 |
| R52 | A turn delete names the turn by its position in the chat. | Review answer 5 |
| R53 | No save is refused because of D1. A record larger than the D1 limit moves as pieces, for all record kinds. The receiver builds the record from the pieces before it applies it. | User decision, review answer 4 |
| R54 | Records use IDs, except for notes. Notes are named by path. A chat is named by forum ID and session ID. | User decision, review answer 2 |
| R55 | A chat note copy whose source note no longer exists does not move. | User decision |

## 3. Facts

### 3.1 CHA code

| ID | Fact | Source |
|---|---|---|
| F1 | The products are macOS and Windows. The Linux package has `cha-daemon` and ChaWeb. | [README.md:4](../README.md), [README.md:401](../README.md) |
| F2 | Each vault has a `.toml` file and its own database file. A vault can be protected. | [README.md:85](../README.md), [README.md:108](../README.md) |
| F3 | `cha-daemon` opens only its startup vault. A vault gets to Linux as a copy of its files. ChaWeb has no Settings and no R2 transfer. | [main.cpp:132](../src/daemon/main.cpp), [packaging/linux/README.md:113](../packaging/linux/README.md), [client.ts:55](../webapp/src/chaweb/client.ts) |
| F4 | R2 upload and download move the whole database and its vault `.toml`. R2 credentials are per vault and travel with the database. | [README.md:306](../README.md), [README.md:311](../README.md) |
| F5 | API keys are stored in the vault database. | [README.md:241](../README.md) |
| F6 | Content tables: `config`, `forums`, `sessions`, `turns`, `entries`, `session_character_settings`, `session_folders`, `note_items`, `note_summaries`, `session_notes`, `session_files`. Local-only tables: `entry_audio`, `note_audio`, `note_audio_attempts`, `event_history`. | [workspace_session_database.cpp:38](../src/storage/workspace_session_database.cpp), [note_storage.cpp:989](../src/storage/note_storage.cpp), [folder_store_storage.cpp:100](../src/storage/folder_store_storage.cpp), [event_storage.cpp:38](../src/storage/event_storage.cpp) |
| F7 | `note_items` has no change timestamp. A name is unique among the items of one parent. | [note_storage.cpp:989](../src/storage/note_storage.cpp), [note_storage.cpp:1009](../src/storage/note_storage.cpp) |
| F8 | Note content is stored compressed. The note limit is 12 MiB before compression. | [note_storage.cpp:212](../src/storage/note_storage.cpp), [note_storage.h:24](../src/storage/note_storage.h) |
| F9 | A text save writes `content_json`. A scroll save writes only `scroll_top`. A text save of a read-only note is refused. | [note_storage.cpp:1232](../src/storage/note_storage.cpp), [note_storage.cpp:1256](../src/storage/note_storage.cpp), [note_storage.cpp:1224](../src/storage/note_storage.cpp) |
| F10 | There is no note move operation. Note operations are create, save, compress, scroll, read-only, rename, delete, book import, folder import, and save summary. | [note_storage.h:128](../src/storage/note_storage.h), [note_storage.cpp:1135](../src/storage/note_storage.cpp), [note_storage.cpp:1784](../src/storage/note_storage.cpp), [note_storage.cpp:1347](../src/storage/note_storage.cpp) |
| F11 | A `session_id` is made from the time and is unique in a forum. Each new chat starts with the label "New session". A label is not unique. | [session_repository.cpp:571](../src/storage/session_repository.cpp), [workspace_session_database.cpp:419](../src/storage/workspace_session_database.cpp), [session_label.h:9](../src/storage/session_label.h) |
| F12 | The automatic chat name can arrive before or after the first response. | [session_controller.cpp:1444](../src/session/session_controller.cpp), [session_controller.cpp:1493](../src/session/session_controller.cpp) |
| F13 | Chat operations: start, complete, cancel, and fail a turn; add a note, a file, or a folder; detach a folder; delete a turn; clear the history; set character settings; rename. A message to `@-` and a configuration notice are entries with no turn. | [session_database.h:92](../src/storage/session_database.h), [session_database.cpp:660](../src/storage/session_database.cpp), [session_controller.cpp:509](../src/session/session_controller.cpp), [session_controller.cpp:1096](../src/session/session_controller.cpp) |
| F14 | Entry numbers must strictly increase. An open chat holds its next entry number in memory. | [session_database.cpp:295](../src/storage/session_database.cpp), [session_controller.cpp:501](../src/session/session_controller.cpp) |
| F15 | `reload_session` only asks one open chat to stop, with the reason `reloading`. It does not wait. The screen reconnects after a stop with this reason. | [live_session_manager.cpp:818](../src/runtime/live_session_manager.cpp), [useLiveSession.ts:307](../webapp/src/useLiveSession.ts) |
| F16 | The application already sends note events to the screen, for example `notes.compressionChanged`. | [bridge_router.cpp:2059](../src/bridge/bridge_router.cpp) |
| F17 | Settings edits and vault merge save through one function, `commit_and_publish`. It has the old rows, the new rows, and the ID of a deleted forum. It writes only the rows that changed, and it deletes the chats of a deleted forum. Configuration import does not use this function. It replaces the rows in its own transaction, and it deletes the chats of each forum that the new configuration does not have, with `prune_orphan_sessions`. | [workspace_config_store.cpp:1472](../src/workspace/workspace_config_store.cpp), [workspace_config_store.cpp:333](../src/workspace/workspace_config_store.cpp), [workspace_config_store.cpp:1495](../src/workspace/workspace_config_store.cpp), [workspace_config_store.cpp:2902](../src/workspace/workspace_config_store.cpp), [vault_operations.cpp:1115](../src/app/vault_operations.cpp), [workspace_config_store.cpp:925](../src/workspace/workspace_config_store.cpp), [workspace_config_store.cpp:866](../src/workspace/workspace_config_store.cpp) |
| F18 | The configuration save makes, renames, and deletes the forum folders in the notes tree. | [workspace_config_store.cpp:1493](../src/workspace/workspace_config_store.cpp), [note_storage.cpp:1468](../src/storage/note_storage.cpp) |
| F19 | A chat file or chat note is limited to 256 KiB. A prompt is limited to 32,768 bytes by default. A model response has no limit. | [session_controller.cpp:579](../src/session/session_controller.cpp), [runtime_settings.h:24](../src/runtime/runtime_settings.h) |
| F20 | R2 transfer uses curl with a connect timeout of 10 seconds and a low-speed timeout of 30 seconds. | [r2_database_transfer.cpp:380](../src/app/r2_database_transfer.cpp) |
| F21 | ChaWeb only reads notes. | [client.ts:168](../webapp/src/chaweb/client.ts) |
| F22 | The R2 storage screen is the model for the D1 sync screen. | [R2StorageSettings.tsx:130](../webapp/src/components/R2StorageSettings.tsx) |
| F23 | The ANNOTATIONS note of a folder is built again from the other notes of the folder by one function. Its content holds the local IDs of the source notes. | [note_storage.cpp:432](../src/storage/note_storage.cpp), [note_storage.cpp:458](../src/storage/note_storage.cpp) |
| F24 | A turn delete needs the number of the response entry. | [session_database.cpp:838](../src/storage/session_database.cpp) |
| F25 | A vault name can be any text. Vault names ignore ASCII letter case. A vault can be made as a copy, and a new vault can be downloaded from R2. | [application_config.cpp:245](../src/app/application_config.cpp), [path_name.cpp:158](../src/util/path_name.cpp), [vault_operations.cpp:468](../src/app/vault_operations.cpp), [vault_operations.cpp:719](../src/app/vault_operations.cpp) |
| F26 | A new chat starts with `next_request_id = 1`. The counter only grows. A clear and a turn delete do not reset it. Only one turn of a chat can run at a time. | [session_repository.cpp:603](../src/storage/session_repository.cpp), [session_database.cpp:301](../src/storage/session_database.cpp), [session_database.cpp:899](../src/storage/session_database.cpp), [workspace_session_database.cpp:435](../src/storage/workspace_session_database.cpp) |
| F27 | A session delete first reserves the chat. The reservation stops the open chat and waits until it is released. A stop cancels a running reply and saves it as cancelled. | [application.cpp:768](../src/app/application.cpp), [live_session_manager.cpp:910](../src/runtime/live_session_manager.cpp), [session_controller.cpp:1835](../src/session/session_controller.cpp) |

### 3.2 Cloudflare

| ID | Fact | Source |
|---|---|---|
| C1 | A program outside Cloudflare can run SQL with `POST https://api.cloudflare.com/client/v4/accounts/{account_id}/d1/database/{database_id}/query` and an API token. The body is one statement, or a `batch`: a list of statements, each with its own `sql` and `params`. Parameters are strings. | [D1 query API](https://developers.cloudflare.com/api/resources/d1/subresources/database/methods/query/) |
| C2 | The REST page does not say whether a `batch` is all-or-nothing. | [D1 query API](https://developers.cloudflare.com/api/resources/d1/subresources/database/methods/query/) |
| C3 | A row or string is limited to 2,000,000 bytes. A database is limited to 500 MB (free plan) or 10 GB (paid plan). Rows per table have no limit apart from this. | [D1 limits](https://developers.cloudflare.com/d1/platform/limits/) |
| C4 | The Cloudflare API allows 1,200 requests per 5 minutes per user. Above that, all requests fail with HTTP 429 for 5 minutes. | [API limits](https://developers.cloudflare.com/fundamentals/api/reference/limits/) |
| C5 | The free plan allows 5 million rows read and 100,000 rows written per day. `INSERT`, `UPDATE`, and `DELETE` count as rows written. | [D1 pricing](https://developers.cloudflare.com/d1/platform/pricing/) |
| C6 | An SQL statement is limited to 100,000 bytes, and a query can have at most 100 bound parameters. So a record must be a bound parameter, not part of the SQL text. A query is limited to 30 seconds. | [D1 limits](https://developers.cloudflare.com/d1/platform/limits/) |
| C7 | A query result is limited to 50 MB since November 4, 2026. A larger result fails with the error `D1 query result exceeded the maximum response size limit of 50 MB`. | [D1 limits](https://developers.cloudflare.com/d1/platform/limits/), [D1 errors](https://developers.cloudflare.com/d1/observability/debug-d1/) |
| C8 | A full database gives the error `Exceeded maximum DB size.` The daily limits of the free plan give different error texts. | [D1 errors](https://developers.cloudflare.com/d1/observability/debug-d1/) |
| C9 | A query on `sqlite_master` is permitted. | [D1 SQL statements](https://developers.cloudflare.com/d1/sql-api/sql-statements/) |

## 4. Overview

```text
Machine A (the user works here)          D1                         Machine B
-------------------------------          --                         ---------
user operation
  -> local change
  -> record in local queue  ---Worker--> queue table "q/Vault/B" ---Worker--> apply
                                         queue table "q/Vault/C"             to local
                                                                             database
```

Each application does two jobs in one Worker loop:

1. It sends the records of its local queue to the queue tables of all other
   machines for the open vault (R7, R10, R22).
2. It reads its own queue table, applies the records, and deletes them (R4,
   R5, R40).

## 5. Configuration

### 5.1 Machine name

`app.toml` gets one new key (R36):

```toml
machine = "mac"
```

- The name must not contain `/`, because the queue table name uses `/` as a
  separator (R36, section 6.1).
- Each machine must have a different name, and letter case does not make a
  name different (O8).
- `app.toml` is outside the vault, so an R2 copy does not bring it (R36).

### 5.2 D1 credentials

The vault stores three values next to the R2 credentials (R36): account ID,
database ID, and API token. They use the same storage as the R2 credentials,
so they come with an R2 copy (R36, F4).

A change of these values writes no record (R44).

### 5.3 Connected vault

The application treats a vault as connected when the vault has D1 credentials
and `app.toml` has a machine name. Only a connected vault writes records, sets
the "changed" flag, and runs the Worker (R44, O3).

### 5.4 D1 sync screen

The screen is as approved (R43). It uses the R2 storage screen as its model
(F22):

- Settings, API Keys: a new row "D1 sync".
- Fields: Account ID, Database ID, API token.
- Buttons: Reset, "Save D1 credentials", and "Disconnect from D1" when
  connected.
- A confirmation before Disconnect.

"Save D1 credentials" (R43, R46):

1. The application makes the queue table of this machine in D1, if it is
   missing (section 6.1). This also works on a vault that already has
   credentials, so it is the way to make a missing table (section 12.3).
2. If D1 cannot be reached, the screen shows the error, and nothing is saved.
3. Otherwise, the application saves the credentials and starts the Worker.

"Disconnect from D1" (R45):

1. The application removes the D1 credentials, empties the local queue, and
   clears all "changed" flags (O3). This part does not need D1.
2. The application tries once to delete the queue table of this machine. If
   the request fails, it writes a warning. A table that stays in D1 is deleted
   later by the steps in section 11 when D1 is full.

### 5.5 Linux

`cha-daemon` has no Settings and no R2 transfer (F3). A vault gets to Linux as
a copy of its files (F3). So (R17, R51):

- At start, if the vault is connected and the queue table of this machine is
  missing, `cha-daemon` makes the table. Then the Worker starts.
- If the table goes missing while the daemon runs, the Worker stops and writes
  an error, as on the other machines. The next start of the daemon makes the
  table again.

## 6. Data

### 6.1 Queue tables in D1

One table for each vault on each machine (R3, R36):

```sql
CREATE TABLE IF NOT EXISTS "q/<vault name>/<machine name>" (
    piece INTEGER NOT NULL,
    pieces INTEGER NOT NULL,
    record TEXT NOT NULL
);
```

- SQLite `rowid` gives the order, so records apply as they come (R8, R9).
- `piece` and `pieces` give the number of the piece and the number of pieces.
  A record that fits in one row has `piece = 1` and `pieces = 1` (R53).
- Each `"` in a name is written as `""` in the SQL text (R36).
- The Worker finds the recipients with one query on `sqlite_master` (R36, R10,
  C9). A table belongs to vault V when its name is `q/` + V + `/` + M, and M
  has no `/`. M is then the machine name. The comparison ignores ASCII letter
  case, as SQLite does for table names (F25). This rule does not use `LIKE`,
  and it works also when a vault name contains `/`.
- The tables of this machine are not recipients (R10).
- There is no registry table (R36).

### 6.2 Local queue

One new table in the vault database (R22):

```sql
CREATE TABLE IF NOT EXISTS sync_queue (
    record TEXT NOT NULL
) STRICT;
```

The application adds a whole record here. The Worker sends it and then deletes
it (R22). The local queue does not split records (R53).

### 6.3 "Changed" flag on notes

One new column on `note_items` (R47):

```sql
ALTER TABLE note_items ADD COLUMN changed INTEGER NOT NULL DEFAULT 0
    CHECK (changed IN (0, 1));
```

All existing notes start with 0 (R47).

## 7. Change records

### 7.1 Format

A record is one JSON object. `kind` names the change.

- A note item is named by its path: a JSON array of names from the top level,
  for example `["Books", "Notes on Kafka"]` (R54).
- A chat is named by forum ID and session ID:
  `["forum_3", "20261109-142233-session"]` (R54). Each chat record also
  carries the current `label`, so that a receiver can create a missing chat
  (R29).
- A configuration row is named by its row name, for example
  `system/keys/api_key_1/config.toml` (R48).
- IDs inside configuration content and chat entries stay as stored, for
  example a `participant_id` or the key ID of a provider (R48).

Note content is the compressed note (F8), as base64 text (R53).

### 7.2 Record kinds

| Kind | Fields | Source |
|---|---|---|
| `note.create` | `path`, `item` (`folder` or `note`), `content`, `read_only`, `scroll_top`, `compressed_text`, `summary_of` (chat, only for a chat summary note) | R18, R42 |
| `note.update` | `path`, `content`, `scroll_top`, `read_only`, `compressed_text` | R47, R49 |
| `note.rename` | `path`, `name` | R42 |
| `note.delete` | `path` | R42 |
| `note.read_only` | `path`, `read_only` | R42 |
| `note.compress` | `path`, `compressed_text` | R42 |
| `chat.entries` | `chat`, `label`, `state` and `entries` of one finished turn, or one entry with no turn. Entries have no numbers. | R18, R27, R28 |
| `chat.rename` | `chat`, `label` | R18 |
| `chat.delete` | `chat`, `label` | R18 |
| `chat.clear` | `chat`, `label` | R18 |
| `chat.delete_turn` | `chat`, `label`, `turn` (its position among the turns of the chat, from 1) | R52 |
| `chat.add_note` | `chat`, `label`, `path`, `name`, `text` | R18 |
| `chat.add_file` | `chat`, `label`, `name`, `text` | R18 |
| `chat.add_folder` | `chat`, `label`, `path` | R18 |
| `chat.detach_folder` | `chat`, `label`, `path` | R18 |
| `chat.character` | `chat`, `label`, `character_id`, `provider_id`, `reasoning_effort` | R18 |
| `config.edit` | `set` (a list of rows: `name` and `content`), `delete` (a list of row names) | R12, R31, R50 |

No record carries audio or event history (R18). No record carries an
ANNOTATIONS note, because its content holds local IDs (R54, F23).

### 7.3 Pieces

No save is refused because of D1. The note limit stays 12 MiB before
compression (R53, F8).

- The Worker splits a record when it sends it, not when the record is made.
  So the rule is the same for all record kinds (R53).
- A piece is at most 1,900,000 bytes of the record text, cut at a UTF-8
  character boundary. With the two numbers, the row stays under the D1 limit
  of 2,000,000 bytes (C3).
- A note of 12 MiB needs at most a few pieces. A model response or a
  configuration import with pictures has no limit, so it can need many pieces
  (F19).

## 8. Writing records

### 8.1 General rules

- Only a connected vault writes records (R44, section 5.3).
- The code of each user operation adds its record to the local queue, in the
  same SQLite transaction when the operation already has one (R7, R22).
- The Worker applies incoming records below the level that writes records. The
  configuration save writes a record and is also used by the Worker (F17), so
  it gets one argument that turns the record off. The Worker sets it (R38).
- A change of the D1 credentials writes no record (R44).

### 8.2 Notes

- Create, rename, delete, read-only, and compress add their record at once
  (R42).
- Book import and folder import add one `note.create` record for each new
  item, with content and the read-only setting, in the transaction of the
  import. A parent comes before its children (R18, F10).
- Save summary adds a `note.create` with `summary_of` for a new summary note.
  When it replaces the content of an existing summary note, it sets the flag,
  like a content load (R47, F10).
- The save code and the code that loads content into an existing note set
  `changed = 1` (R47). This is the only change to the save code.
- A scroll save writes no record and does not set the flag (R47, F9).
- In each loop, the Worker does one transaction for each flagged note: it adds
  a `note.update` record with the current content, scroll position, read-only
  setting, and compressed text to the local queue, and sets `changed = 0`. A
  later save sets the flag again (R47).
- An ANNOTATIONS note writes no record and never gets the flag. Its build
  (F23) is not a save (R54).
- Folders that the configuration save makes, the forum folders, write no
  records. Each receiver makes them when it applies the configuration record
  (O5, F18).

### 8.3 Chats

A chat is **shared** when its label is not a temporary label (F11) and its
first turn has finished (R29, O2). The first turn has finished when
`next_request_id` is larger than 1 and no turn with request ID 1 is running.
A clear or a turn delete does not reset this counter (F26), so a chat stays
shared after it loses its turns (R29: "after that, each change writes its own
record"). The application finds this from existing data. It stores nothing
new (R9).

Each chat operation checks the rule before and after the operation. A chat
delete uses only the state before, because after it the chat does not exist.

- If the chat was not shared before and is shared after, the operation adds
  records for all that the chat has: finished turns, `@-` messages,
  attachments, and character settings. The records follow the order of the
  entries on the sender (R29). This operation is usually the end of the first
  turn or the rename from the temporary label (F12).
- If the chat is shared after the operation (or before it, for a delete), the
  operation adds its own record (R29, F13):
  - The end of a turn adds one `chat.entries` record with the prompt, the
    response, and the turn state (R27).
  - A message to `@-` adds one `chat.entries` record with that entry (R18,
    F13).
- Otherwise the operation adds no record (R29).
- A chat note copy keeps its name and text after its source note is deleted,
  but it has no path ([note_storage.cpp:1038](../src/storage/note_storage.cpp)).
  If the source note no longer exists when the application makes the
  `chat.add_note` record, it adds no record for that copy (R55).
- Notices and errors with no turn add no record (O10).
- Welcome is private to the running application and is not stored
  ([README.md:22](../README.md)). So it writes no records (R18).

### 8.4 Configuration

Each configuration save adds one `config.edit` record, in the transaction of
the save (R50). The record holds only the rows that the save changed (R12).
The rows of the D1 credentials are left out (R44). There are two save paths
(F17):

- A Settings action and a vault merge save through `commit_and_publish`. It
  already has the old rows and the new rows.
- A configuration import replaces the rows in its own transaction. It reads
  the old rows before the replace.

One small helper finds the changed rows and adds the record. Both paths call
it. If the save fails, its transaction adds no record.

## 9. The Worker

### 9.1 Life cycle

- One Worker object with one thread runs in the application process (R19).
- It runs only for the open vault, and only when the vault is connected (R20,
  R44).
- The loop runs every 30 seconds (R37).
- At vault open, the loop runs once before the screens load. If D1 cannot be
  reached, the application writes a warning and continues (R37).
- At application close, at vault close, and at a vault switch, the Worker
  turns flagged notes into records and tries once to send the local queue.
  Then it stops (R33, R37).
- It also stops during maintenance such as R2 download, and starts again after
  that (R20).
- It uses curl with the timeouts of the R2 transfer (F20) and the REST API
  (C1), because the application writes to D1 directly (R10, R11).

### 9.2 One loop

1. **Flagged notes.** Turn each flagged note into a local record (section
   8.2).
2. **Send.** If the local queue is empty, skip this step. Otherwise, get the
   list of the queue tables of this vault (one request). If the queue table of
   this machine is not in the list, stop the Worker before any send, and write
   the error of step 3 (R46). Otherwise, for each local record in `rowid`
   order:
   - With no other queue tables, the record needs no request. Delete it (R11).
   - If the record fits in one piece, send one REST request in `batch` form,
     with one `INSERT` for each other queue table. The record is a bound
     parameter (R23, C1, C6).
   - If the record needs several pieces, send one such request for each
     piece, in order (R53).
   - When all requests for the record succeed, delete the local record (R11,
     R22).
   - If a request fails with `Exceeded maximum DB size.`, do the steps in
     section 11. Then stop sending in this loop (R46, C8).
   - If a request fails for another reason, write a warning and stop sending
     in this loop. The next loop sends the record again from its first piece
     (R22, R23).
3. **Receive.** Read up to 50 rows from the queue table of this machine in
   `rowid` order (R40). Apply them in order (section 10). Delete the handled
   rows with one request: `DELETE ... WHERE rowid <= <last handled rowid>`.
   Repeat until the queue has no more rows to handle (R5, R40).
   - If the read fails with the result-size error (C7), read again with half
     as many rows, down to one row. The next loop starts again with 50.
   - If the queue table of this machine is missing, stop the Worker and write
     an error to the log. The error says: download the vault from R2, or save
     the D1 credentials again (R46, section 12.3). `cha-daemon` makes the
     table at its next start (R51).

### 9.3 Pieces on the receiver

The Worker builds a record from its pieces before it applies it (R53):

- A row with `pieces = 1` is a whole record.
- A row with `piece = 1` and `pieces > 1` starts a set. The Worker keeps its
  text in memory and adds the next pieces in order, also from the next groups
  in the same loop.
- When the last piece arrives, the Worker joins the text and applies the
  record. The rows of the set are handled.
- If a row does not continue the open set, the set is incomplete. The Worker
  skips it with a warning, and its rows are handled (R41). Then the Worker
  treats that row as usual. This occurs when the sender sends a record again
  from its first piece (section 9.2, R23).
- If the queue ends while a set is open, the Worker does not apply the set and
  does not delete its rows. The next loop reads the set again. This adds no
  state to the local database.

## 10. Applying records

### 10.1 General rules

- Records are applied as they come, in order, with no versions, checks of
  versions, or comparisons (R8, R9).
- Each record is applied in its own local transaction, so that a failed
  record does not undo the others (R41).
- If a record cannot be applied, the Worker skips it and writes a warning. The
  record is deleted with its group (R41).
- An applied change writes no record and sets no flag (R38).

### 10.2 Notes

- The Worker finds an item by its path (R54).
- If the parent folder does not exist, skip with a warning (R16).
- If a new or renamed item has a name that already exists in its folder, the
  Worker adds a suffix: `Name (1)`, `Name (2)` (R15, F7).
- `note.update` writes the content, the scroll position, the read-only
  setting, and the compressed text directly, not through the save function. So
  the read-only check of the save does not refuse it (F9). It removes the audio
  of the note (R13).
- If no note exists at the path of a `note.update`, the Worker creates the
  note there from the record (R49). If the name is taken by a folder, the
  suffix rule applies (R15).
- A `note.create` with `summary_of` links the note to that chat. If the
  receiver does not have the chat, the note has no link (R41).
- After the Worker applies a note record, it builds the ANNOTATIONS note of
  that folder again with the existing function (F23, R54).

### 10.3 Chats

- If the forum of a chat record does not exist, skip with a warning (R16,
  R41).
- Before the Worker applies a chat record, it reserves that chat in the same
  way as a session delete does (F27). The shutdown reason is `reloading`, so
  the screen of an open chat reconnects after the release (R35, F15). If the
  chat is open, the reservation stops it and waits. A running reply stops and
  is saved as cancelled. Then the Worker applies the record and releases the
  chat. So the open chat cannot write with old entry numbers while the Worker
  writes (F14).
- If the reservation fails, the Worker stops the receive step in this loop.
  The rows stay in D1, and the next loop tries again.
- If a chat record names a chat that this machine does not have, the Worker
  creates the chat with that session ID and the label of the record (R29,
  R54).
- `chat.entries` appends the entries with the next free numbers, and for a
  turn it stores the state of the record (R28, F14).
- `chat.delete_turn` deletes the turn at that position (R52, F24). If there is
  no such turn, skip with a warning (R41).
- A `chat.add_note`, `chat.add_folder`, or `chat.detach_folder` record finds
  the note or folder by its path. If it is missing, skip with a warning (R41).
- When the screen reconnects, the chat opens again from the database, with
  the new entries and the new next entry number (R35, F14).

### 10.4 Configuration

The Worker puts the rows of `set` and `delete` on its current rows, validates
the result one time, and saves it one time through `commit_and_publish` (R50,
F17). In the same transaction, it deletes the chats of each forum that the new
configuration does not have, with the existing `prune_orphan_sessions` (F17).
So one record covers a forum delete in Settings and an import that removes
several forums. It turns the record off (section 8.1, R38). An invalid
result is skipped with a warning (R14). The new configuration then becomes
active, as after a Settings edit (R12).

### 10.5 Screens

- After a group of records changes notes, the application sends one
  `notes.changed` event to the screen, in the same way as other note events
  (F16). The desktop notes screen then reloads the tree and the open note
  (R39). Unsaved text in the open note is lost (R32).
- ChaWeb only reads notes (F21). Its note page shows the change when it opens
  again (R39).

## 11. Full D1 database

These steps run only for the error `Exceeded maximum DB size.` (R46, C8):

1. The Worker counts the rows of each queue table in the D1 database, except
   the tables of this machine. It uses one request.
2. It deletes the table with the most rows (`DROP TABLE`).
3. It stops sending in this loop. The next loop sends again. The Worker
   deletes at most one table in one loop.

A desktop machine that lost its table finds this at its next vault open,
because its queue table is missing (section 9.2). The Worker stops and writes
an error to the log. The user must download the vault from R2. The download
makes a new queue table (section 12.2). This is not automatic recovery (R46).

`cha-daemon` makes a new table at its next start and continues (R51). The
records in the deleted table are lost. To get a correct vault on Linux, the
operator copies the vault files again (F3).

This needs no catalog table and no status (R46). The missing table is the
mark.

## 12. R2 transfer and new vaults

### 12.1 Upload

Before the check, the Worker runs steps 1 and 2 of its loop one time. Then R2
upload is refused while the local queue has records or a note has
`changed = 1` (R26, R47). So a refusal means that D1 did not accept the
records. The error text is in O6.

A vault that is not connected has no records and no flags (section 5.3), so
its upload is never refused.

### 12.2 Download that replaces the open vault

There is no removal step at download (R26). After the download replaces the
database, if the vault is connected, the application makes the queue table of
this machine (R21, R46). Then the vault opens, and the first loop runs (R37).

The order on a new desktop machine (R21, R24, R36):

1. Write the machine name in `app.toml`.
2. Download the vault from R2. The download brings the D1 credentials.
3. The application makes the queue table of this machine.
4. The vault opens, and the Worker starts.

A change made on another machine between its R2 upload and step 3 never
reaches the new machine (R24).

### 12.3 Other ways to a missing queue table

In these cases a connected vault has no queue table for this machine (F25):

- A new vault that is downloaded from R2 and is not open. For a protected
  vault, the application cannot read the credentials before the password is
  given.
- A vault made as a copy. It has a different name, so it has no table.
- A machine name that is added or changed in `app.toml`. The table of the old
  name stays in D1 and gets records until section 11 deletes it.

In each case the Worker stops with the error of section 9.2. The user opens
the D1 sync screen and selects "Save D1 credentials", which makes the table
(section 5.4, R43, R46).

## 13. Accepted risks

| Risk | Example | Source |
|---|---|---|
| A record arrives two times. | D1 writes the record, but the response does not arrive. The Worker sends it again. A `chat.entries` record adds the turn again, and a `note.create` record makes a second note `Name (1)`. | R23 |
| A partial write. | Only some queue tables get the record. The next try sends it again to all. | R23, C2 |
| A change misses a new machine. | You edit a note between the R2 upload and the first open on the new machine. | R24 |
| Two configured items become one. | The Mac makes `forum_3` "One two three", and Windows makes `forum_3` "Five six seven", before they synchronize. Their records overwrite each other, and the chats of both forums go into one forum. | R48 |
| Two chats become one. | Two machines make a chat in the same forum in the same second, before they synchronize. Both get the same session ID. | R54 |
| Local editing is overridden. | An incoming update replaces unsaved text in the open note. | R32 |
| A deleted note comes back. | You delete X on Windows while the Mac is offline, and you edit X on the Mac. The Mac update creates X again on Windows. | R49 |
| A chat with only `@-` messages never moves. | It has a name but no finished turn, so it is never shared. | R29 |
| Synchronization stops until recovery. | The desktop machine whose queue table was deleted needs a new R2 download. On Linux, the records in the deleted table are lost. | R46, R51 |
| The last changes wait on a sleeping machine. | You close the laptop lid. The application does not close, so the changes of the last 30 seconds stay on the machine until it wakes. | R37 |
| A chat note copy is missing on the other machines. | You attach note X to a new chat and delete X before the first response. The copy stays in the chat on this machine, but it does not move, so the model context differs between the machines. | R55 |
| An incoming chat record stops a running reply. | D1 cannot be reached at vault open, and you start a reply in a chat. D1 comes back during the reply with an older record for that chat. The reservation stops the reply, and it is saved as cancelled. | R35, F27 |
| A chat stops moving after a rename to a temporary label. | You rename a shared chat to "New session". Its later changes add no record. A rename to a real name after that sends all that the chat has again, so the other machines get its turns two times. | R29 |
| A stopped Worker has no visible sign. | The only sign is an error in the log. A sign on the screen is a UI change and needs approval. | R33 |

## 14. Limits and cost

- Requests: each loop uses at least one receive request, every 30 seconds.
  The list request is sent only when there is a record to send. With three
  machines this is about 30 requests per 5 minutes, far below 1,200 (C4).
- Many records at one time, for example from a book import or from a chat that
  becomes shared, need one request for each record or piece. Above 1,200
  requests in 5 minutes, Cloudflare refuses all API requests for 5 minutes.
  The rule "stop sending in this loop" handles this, and the next loops send
  the rest (C4).
- A backlog of 1,000 small records needs about 40 receive requests (R40).
- Rows written: each record writes one row in each other queue table, and its
  delete counts as rows written too. The free plan allows 100,000 rows written
  per day (C5).
- Storage: applied records are deleted. A machine that you forget to
  disconnect fills the database. Section 11 handles that case (R46).

## 15. Not tested

Do these tests before implementation. The first two decide whether sections
9.2 and 11 work at all.

- Whether D1 allows `DROP TABLE` when the database is full.
- Whether the REST API accepts a `batch` body with one copy of a 1,900,000-byte
  piece for each other queue table.
- Whether a `batch` is all-or-nothing (C2).
- How many rows read D1 counts for a query on an empty table.
- Whether the screen of an open chat reconnects after the reservation of
  section 10.3. The screen reconnects after the reason `reloading` (F15), but
  the reservation refuses a reconnect while it is held.

## 16. Approved points

These items came from my reading of the requirements. The user approved them
on November 9, 2026.

| ID | Point |
|---|---|
| O2 | A "first response" is any finished turn: complete, stopped, or failed. |
| O3 | The "changed" flag follows R44: only a connected vault sets it. Disconnect clears all flags, like it empties the local queue (R45). Without this, an old flag blocks R2 upload forever. |
| O5 | Folders that the application makes from the configuration write no records, because each receiver makes them from the configuration record. The review confirmed this code path (F18). |
| O6 | New UI texts need visual approval before implementation. The user approved these texts on November 9, 2026. Each appears where the existing R2 errors appear. Vault settings, refused R2 upload: "Changes are not sent to D1 yet. Try again when D1 can be reached." D1 sync screen: "D1 could not be reached. Check the account ID, database ID, and API token." (Save, D1 request fails); "This machine has no name. Add machine to app.toml, then save again." (Save, no machine name); "D1 credentials could not be saved." (Save, local error); "D1 credentials could not be loaded." (screen open, local error); "D1 credentials could not be removed." (Disconnect, local error). |
| O8 | Each machine name must be different, and letter case does not make a name different. Two machines with the same name use one queue table, and each takes the records of the other. |
| O10 | Notices and errors with no turn do not move. A configuration notice goes into a chat that is open on the machine where the configuration changed ([session_controller.cpp:1115](../src/session/session_controller.cpp)). It describes a local event, not user content. I did not check whether a receiver adds the same notice when it applies the configuration record. A message to `@-` is user content and moves (section 8.3). Without this point, R18 sends the notices too. |

O4 and O7 were replaced by R53.
