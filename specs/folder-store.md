# Folder stores: upload without a link

Date: November 3, 2026

Status: **Draft for review.** Do not implement it before the user approves this
document and the UI changes in section 7.

## 1. Why

Today CHA links a folder to the store that it created from the folder's notes:

- A folder can have only one store. To upload again, you must first use
  **Remove from store**.
- When you delete a folder, CHA also deletes its stores at the provider.
- CHA keeps the link in the database, in the tables `folder_stores` and
  `folder_store_files`.

We want a simpler model. **CHA keeps no link between a folder and a store.** An
upload sends the folder's notes to a new store at the provider. After the
upload, CHA forgets the store. The provider keeps it.

## 2. Upload

- The folder menu shows **Upload to store** for every normal folder.
- Each upload creates a new store with the name and description from the Upload
  dialog. You can upload the same folder many times.
- Only one upload can run for a folder at a time. CHA keeps this rule in memory.
- When the upload succeeds, CHA writes an Event with the folder, the provider,
  the saved key, the store ID, and for OpenAI the file IDs. This Event is the
  only record of the store in CHA.

## 3. Failed upload and cleanup

- If an upload fails or you cancel it, CHA deletes the new store and its files at
  the provider, as today.
- CHA keeps the IDs of the new store and files in memory only. It also writes
  them to the log and to Events.
- **Retry cleanup** works while the Upload dialog stays open and CHA runs. After
  you close the dialog or quit CHA, use the IDs in Events to clean up by hand.
- If CHA cannot tell whether the provider created a store, it writes a warning
  Event, as today.

## 4. Delete a folder

- Delete is the same for all folders. CHA uses the normal folder delete
  confirmation and never calls a store provider.
- CHA does not delete a folder while an upload runs in that folder or in a
  folder inside it.

## 5. Chat

- You can still attach a folder to a chat, and `/detach` still works.
- Until the next stage, an attached folder has no store. The chat search tool
  reports "no ready store" for it and finds nothing. The next stage will change
  how a chat gets a store.

## 6. What CHA removes

- The tables `folder_stores` and `folder_store_files`.
- The `store` field in the notes list.
- The **Remove from store** menu item and the Remove operation.
- The folder delete operation that deleted stores together with the folder, and
  its dialog with progress, **Retry**, and **Delete anyway**.
- The "one store per folder" rule and the delete blocks for folders with stores.
- Store data in folder export files.
- Folder stores in the "Used by" list of an API key in Settings.
- The `stores` topic of the assistant's inspect tool.

**What stays:**

- The Upload dialog, progress, Cancel, and **Retry cleanup**.
- The code that deletes a store and its files at the provider. CHA uses it now
  only to clean up a failed upload. The next stage will use it to delete stores.
- Chat folder attachment (the `session_folders` table).

## 7. UI changes that need approval

- The folder menu shows **Upload to store** for every normal folder and never
  shows **Remove from store**.
- Delete of any folder uses the normal folder delete confirmation.
- The API key "Used by" list in Settings no longer shows folder stores.

## 8. Existing vaults and files

- When CHA opens a vault, it drops the tables `folder_stores` and
  `folder_store_files` if they exist. It writes no Events for the old stores.
  The old stores stay at the provider.
- When CHA imports an older folder export that contains store data, it ignores
  that data and writes a warning to the log. The import does not fail.
- Test the upgrade on a copy of a real vault that has stores.
