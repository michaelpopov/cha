# CHA Administrator Manual

This manual describes how to install and configure CHA: the desktop
application for macOS and Windows, and the ChaWeb browser client.

It is the companion of the [CHA User Manual](UserManual.md). The User Manual
describes daily use: conversations, notes, voice and the Library. Where the
User Manual has a line that starts with **Setup:**, this manual gives the
procedure. [Chapter 33](#33-setup-items-of-the-user-manual) lists these lines
and the sections that answer them.

## Contents

**Part 1 — Installation and first setup**

1. [About this manual](#1-about-this-manual)
2. [Install CHA on macOS](#2-install-cha-on-macos)
3. [Install CHA on Windows](#3-install-cha-on-windows)
4. [First start](#4-first-start)
5. [Minimum setup for the first conversation](#5-minimum-setup-for-the-first-conversation)

**Part 2 — Configuration tasks**

6. [The Settings screen](#6-the-settings-screen)
7. [API keys](#7-api-keys)
8. [Providers and the ChatGPT connection](#8-providers-and-the-chatgpt-connection)
9. [Personas](#9-personas)
10. [Characters](#10-characters)
11. [Forums](#11-forums)
12. [Styles](#12-styles)
13. [Web search and page reading](#13-web-search-and-page-reading)
14. [Session settings](#14-session-settings)
15. [Notes settings](#15-notes-settings)
16. [Voices](#16-voices)
17. [Cloud stores and the Library](#17-cloud-stores-and-the-library)
18. [Vaults](#18-vaults)
19. [R2 storage](#19-r2-storage)
20. [D1 synchronization](#20-d1-synchronization)
21. [Configuration files: export, edit, import](#21-configuration-files-export-edit-import)
22. [The Assistant as a configuration tool](#22-the-assistant-as-a-configuration-tool)
23. [Diagnostics: Events and the log](#23-diagnostics-events-and-the-log)
24. [Backup and restore](#24-backup-and-restore)
25. [ChaWeb](#25-chaweb)

**Part 3 — Reference**

26. [Files in the configuration directory](#26-files-in-the-configuration-directory)
27. [app.toml](#27-apptoml)
28. [Vault definition files](#28-vault-definition-files)
29. [Configuration file layout and keys](#29-configuration-file-layout-and-keys)
30. [Prompt files and the template language](#30-prompt-files-and-the-template-language)
31. [Limits](#31-limits)
32. [Messages](#32-messages)
33. [Setup items of the User Manual](#33-setup-items-of-the-user-manual)

- [Appendix A — Make the packages](#appendix-a--make-the-packages)
- [Appendix B — Obsolete settings](#appendix-b--obsolete-settings)

---

# Part 1 — Installation and first setup

## 1. About this manual

### The reader

This manual is for the person who installs CHA and keeps it in operation. You
must know how to:

- Extract an archive and start an application.
- Edit a text file.
- Create an account and an API key at an external service.

Only the chapter [ChaWeb](#25-chaweb) needs more: work in a Linux terminal,
systemd, nginx and TLS certificates.

### Terms

| Term | Meaning |
| --- | --- |
| Configuration directory | The directory where CHA keeps `app.toml`, the vaults, and the log. See [First start](#4-first-start). |
| Vault | One complete set of data: configuration, sessions and notes. A vault is one database file and one definition file. |
| Configuration | The personas, characters, forums, providers, styles, voices, saved keys and service settings of one vault. CHA keeps the configuration in the vault database, not in files. |
| Active vault | The vault that is open. The Settings screens show and change the active vault. |
| Main process | The first CHA process that opens a vault. Only the main process can change settings. See [More than one CHA process](#more-than-one-cha-process). |
| ID | The internal name of an item, for example `character_3`. An ID does not change when you rename the item. |

### Conventions

- **Bold** text is a label that the application shows.
- `Monospace` text is a file name, a key, a value or a command.
- Text in angle brackets is a placeholder. Replace `<vault name>` with your
  value. Do not type the brackets.
- **Settings → Vaults** means: click **Settings**, then click **Vaults**.

### External services

CHA uses external services for models, voices, web search, cloud stores, and
the transfer of vaults. For each service, this manual tells you what CHA
needs: the fields, the form of an address, and the objects that you must
create. It does not describe the web sites of the services. Each service has
its own prices and its own terms.

## 2. Install CHA on macOS

### Requirements

- A Mac with Apple Silicon. The package does not run on a Mac with an Intel
  processor.
- The macOS version of the computer that made the package, or a later
  version. The oldest possible version is macOS 13.3.

### Install

The package is the file `CHA-macos-<version>.tar.gz`. It contains only
`CHA.app`. There is no installer.

1. Extract the archive.
2. Move `CHA.app` to the **Applications** folder, or to a different folder.
3. Start CHA.

The application has an ad hoc signature and is not notarized. macOS can refuse
to open it until you approve it.

### Where CHA keeps its data

The configuration directory is:

```
~/Library/Application Support/CHA
```

You cannot select a different directory. CHA does not accept command-line
arguments.

### Microphone

The first time you use voice input, macOS asks for access to the microphone.
If you refuse, voice input shows **Microphone access was denied. Allow it in
System Settings and try again.**

### Update and remove

- To update, close CHA and replace `CHA.app` with the new one. The
  configuration directory does not change.
- To remove the application, delete `CHA.app`. To remove all data too, delete
  the configuration directory.

## 3. Install CHA on Windows

### Requirements

- A 64-bit (x64) version of Windows 10 or Windows 11.
- The Microsoft Edge WebView2 Runtime (the Evergreen version). If it is not
  installed, CHA shows **Failed to initialize WebView2** or **The Microsoft
  Edge WebView2 Runtime could not start**.

### Install

The package is the file `CHA-windows-<version>-x64.zip`. It contains the folder
`CHA-windows-x64` with `CHA.exe` and the folder `web`. There is no installer.
CHA makes no shortcuts and no registry entries.

1. Extract the archive to a folder of your choice.
2. Keep `CHA.exe` and the folder `web` together. CHA reads its screens from
   `web`.
3. Start `CHA.exe`.

The package is not signed. Windows can show a warning when you start it.

CHA does not need administrator rights.

### Where CHA keeps its data

| Directory | Content |
| --- | --- |
| `%LOCALAPPDATA%\CHA\config` | The configuration directory. |
| `%LOCALAPPDATA%\CHA\webview` | Data of the WebView2 component. |

You cannot select a different directory. CHA does not accept command-line
arguments.

### Update and remove

- To update, close CHA and replace the folder `CHA-windows-x64` with the new
  one.
- To remove the application, delete that folder. To remove all data too,
  delete `%LOCALAPPDATA%\CHA`.

## 4. First start

### What CHA creates

When the configuration directory is empty, CHA creates these items in it:

| Item | Content |
| --- | --- |
| `app.toml` | The application settings. See [app.toml](#27-apptoml). |
| `default.toml` | The definition of the vault **Default**. |
| `default.sqlite3` | The database of the vault **Default**. |
| `mirror/` | The base directory for readable copies of sessions. |
| `modify/` | The base directory for the export and import of configuration. |
| `logs/cha.log` | The log. |

The first `app.toml` has this content:

```toml
vault = "Default"

mirror = "mirror"
modify = "modify"

[logging]
file = "logs/cha.log"
level = "info"
```

The vault **Default** contains:

- The built-in character **Assistant**.
- One provider with the ID `chatgpt`. It uses a ChatGPT subscription. It works
  only after you connect a ChatGPT account. See
  [Connect a ChatGPT account](#connect-a-chatgpt-account).
- The default texts of the prompts for the note tasks. See
  [Prompts](#prompts).

It contains no forums, no other characters and no API keys.

CHA then shows the Welcome session. See the User Manual for the built-in forum
**Entrance** and the session **Welcome**.

### If CHA cannot start

A start error shows in a dialog with the title **CHA cannot continue**. The
most frequent causes are:

- The configuration directory is not empty, but it has no `app.toml`. CHA
  creates the first configuration only in an empty directory. The message is
  **Failed to read application config** with the path of the file.
- `app.toml` or a vault definition file has an error. See
  [app.toml](#27-apptoml) and [Vault definition files](#28-vault-definition-files).
- A `.toml` file that is not a vault definition is in the configuration
  directory. CHA reads each `.toml` file in that directory, other than
  `app.toml`, as a vault definition.

### Start with a protected vault

If the vault in `app.toml` has a password, a dialog with the title **Open
vault** and the name of the vault shows before the screens of the application
load. Type the password and click **Open vault**. **Quit** stops the start. CHA does not
store the password.

### More than one CHA process

On macOS, **Window → New Instance** starts a second CHA process. On Windows, a
second start of `CHA.exe` does the same. ChaWeb is one more process when it
uses the same vault on the same computer.

For each vault, the first process that opens it is the **main** process. Each
later process is a **secondary** process. A secondary process keeps this role
until it opens a vault again.

A secondary process has these limits:

- It cannot change settings. The message is **Settings can be changed only in
  the main CHA process.** This includes all Settings screens, the vault
  commands, and the upload to a store.
- It cannot generate note audio. The message is **Close other CHA processes
  using this vault, then try again.**
- It does not run D1 synchronization.
- It does not write the session mirror.
- It does not save the vault selection for the next start.

Two processes cannot have the same session open. The message is **This session
is open in another CHA process.**

Some vault commands need all other processes to close the vault first. The
message is **Close other CHA processes using this vault, then try again.**

## 5. Minimum setup for the first conversation

Before the first conversation, the vault must have a forum with a minimum of
one character, and that character must have a provider that works.

A new forum has the Assistant as its first member. Thus the shortest path has
two steps: make the provider of the Assistant work, then create a forum.

### Step 1 — Make a provider work

Use one of these methods.

**Method A: a ChatGPT subscription**

1. Click **Settings**.
2. On the card **OpenAI**, click **Connect ChatGPT** and follow
   [Connect a ChatGPT account](#connect-a-chatgpt-account).

The provider `chatgpt` of the first vault then works. The Assistant already
uses it.

**Method B: an API key**

1. Save the API key of your model service. See [API keys](#7-api-keys).
2. Create a provider that uses the key. See
   [Create a provider](#create-a-provider).
3. Click **Test** on the provider screen. The result must be **Provider
   responded successfully.**
4. Select the new provider for the Assistant: **Settings → Characters →
   Assistant → Character settings → Provider**, then **Save**.

### Step 2 — Create a forum

1. Click **Settings → Forums → New forum**.
2. Type a **Name**. In **Persona**, select **Guest** or a different persona.
3. Click **Create forum**.

You can now start a session in this forum and talk to the Assistant. See
“First steps” in the User Manual.

### Step 3 — Add your own characters

1. Create a character. See [Create a character](#create-a-character).
2. Select a provider for the character in **Character settings**.
3. Write the profile of the character.
4. Add the character to the forum on the **Members** screen. See
   [Members](#members).

You can also ask the Assistant in the Welcome session to do these tasks. See
[The Assistant as a configuration tool](#22-the-assistant-as-a-configuration-tool).

---

# Part 2 — Configuration tasks

## 6. The Settings screen

To open the Settings screen, click the **Settings** button at the bottom of
the sidebar. The button shows a gear.

### Rows

| Row | Function | Chapter |
| --- | --- | --- |
| **Personas** | The identities that you speak as. | [9](#9-personas) |
| **Characters** | The AI participants. | [10](#10-characters) |
| **Forums** | Groups of characters, their members and their Library. | [11](#11-forums) |
| **Vaults** | Create and manage vaults; transfer, import and export. | [18](#18-vaults) |
| **Providers** | Connections to model services. | [8](#8-providers-and-the-chatgpt-connection) |
| **Styles** | Text styles for characters and personas. | [12](#12-styles) |
| **Session settings** | Session naming and recipient detection. | [14](#14-session-settings) |
| **Notes settings** | Compression provider and note audio service. | [15](#15-notes-settings) |
| **Events** | Results of background tasks. | [23](#23-diagnostics-events-and-the-log) |
| **Search API** | Web search and page reading. | [13](#13-web-search-and-page-reading) |
| **Voices** | Voices, voice input and voice output. | [16](#16-voices) |
| **API Keys** | Saved keys, **R2 storage** and **D1 sync**. | [7](#7-api-keys), [19](#19-r2-storage), [20](#20-d1-synchronization) |
| **Cloud stores** | The stores of the Library. | [17](#17-cloud-stores-and-the-library) |
| **Prompts** | The texts that CHA sends to the model for the note tasks and session summaries. | [15](#prompts) |

Below the rows there is the card **OpenAI**. It shows the ChatGPT connection.
See [Connect a ChatGPT account](#connect-a-chatgpt-account).

### Rules that apply to all Settings screens

- The screens show and change the **active vault**. Each vault has its own
  configuration. The ChatGPT connection is the exception: it applies to all
  vaults of the computer.
- Only the main process can change settings.
- CHA makes a check of the full configuration before it saves a change. If the
  check fails, nothing changes, and the screen shows the error.
- A change applies immediately. You do not have to restart CHA. Open sessions
  get the new configuration. Some changes, for example a new name, a new
  profile text or new members, start the open sessions of the forums that use
  the item again. An answer that is in work there is lost.
- To rename an item, click its title, type the new name, and click the save
  button. A new name does not change the ID of the item. Thus all references
  to the item stay correct.
- To delete an item, click the trash button or the **Delete** button on its
  screen and confirm. You cannot undo a deletion. CHA refuses to delete an
  item that is in use. Saved API keys are the exception.
- The IDs that CHA makes have the form `persona_<N>`, `character_<N>`,
  `forum_<N>`, `provider_<N>`, `style_<N>`, `voice_<N>` and `api_key_<N>`.

## 7. API keys

An API key is a named secret for an external service. You save a key one time.
Then you select it by its name in each place that needs it: a provider, the
Search API, voice input, voice output, recipient detection, the note audio
service, or a Library.

CHA has no field for the kind of service. The place that uses the key decides
to which service CHA sends it.

### Where CHA stores the keys

CHA stores each key in the database of the active vault. Each vault has its
own keys.

> **CAUTION:** In a vault without a password, the keys are in the database
> file as plain text. To encrypt them, protect the vault. See
> [Protect a vault with a password](#protect-a-vault-with-a-password).
> CHA does not use the macOS Keychain or the Windows credential store.

The keys go with the vault:

- An R2 upload sends the full database, the keys included.
- D1 synchronization sends the saved keys to the other computers. The D1
  credentials are the exception.
- **Export** writes all keys to files as plain text. See
  [Configuration files](#21-configuration-files-export-edit-import).

After you save a key, no screen shows its value again.

### Save a key

1. Click **Settings → API Keys → New API key**.
2. Type a **Name**, for example the name of the service.
3. Paste the key into **API key**.
4. Select **Cloud store** only if the key is for a Library. Select **OpenAI**
   or **Gemini**. For all other keys, keep **None**.
5. Click **Save API key**.

Rules: the name has 1 to 100 bytes. The key has a maximum of 16,384 bytes.
Neither can contain a line break. CHA does not refuse two keys with the same
name. Use different names.

### Change or remove a key

Click the key in the list.

- To rename it, click the title.
- To replace the secret, paste the new key into **New API key** and click
  **Save**.
- To change **Cloud store**, select the value and click **Save**. You do not
  have to type the secret again.
- To remove it, click **Remove API key** and confirm.

The list **Used by** shows the providers and functions that use the key. A
forum Library and the note audio service do not show in this list.

> **CAUTION:** CHA does not stop you when you remove a key that is in use. The
> functions that use it stop: a provider gives the error that the API key does
> not exist.

### R2 storage and D1 sync

The first two rows of the **API Keys** screen are **R2 storage** and **D1
sync**. These are the credentials for the transfer of vaults. See
[R2 storage](#19-r2-storage) and [D1 synchronization](#20-d1-synchronization).

## 8. Providers and the ChatGPT connection

A provider is a connection to one model at one model service. A character
gets its model from its provider. To let two characters use different models
of the same service, create two providers.

### Supported API formats

| **API format** | Use it for | Address that CHA calls |
| --- | --- | --- |
| **Responses** | The OpenAI Responses API and services that are compatible with it. The ChatGPT subscription. | `<Base URL>/v1/responses` |
| **Chat completions** | Services with an OpenAI-compatible chat completions API, local model servers included. | `<Base URL>/v1/chat/completions` |
| **Google (Gemini)** | The Gemini API of Google. | `<Base URL>/models/<model>:streamGenerateContent` |
| **Anthropic** | The Anthropic Messages API. | `<Base URL>/v1/messages` |

Exceptions:

- For the ChatGPT subscription, CHA always calls
  `https://chatgpt.com/backend-api/codex/responses`.
- For **Chat completions**, when the **Base URL** ends with `/v1beta/openai`,
  CHA calls `<Base URL>/chat/completions`.

Examples of **Base URL** values:

| Service | **API format** | **Base URL** |
| --- | --- | --- |
| OpenAI API | **Responses** | `https://api.openai.com` |
| ChatGPT subscription | **Responses** | `https://chatgpt.com/backend-api/codex` |
| Anthropic API | **Anthropic** | `https://api.anthropic.com` |
| Gemini API | **Google (Gemini)** | `https://generativelanguage.googleapis.com/v1beta` |
| OpenRouter | **Responses** or **Chat completions** | `https://openrouter.ai/api` |
| A local model server | **Chat completions** | `http://<host>:<port>` |

### Create a provider

1. Click **Settings → Providers → New provider**.
2. Type a **Name**.
3. In **Initial settings**, select **Default provider settings**, or select
   **Copy** *provider name* to start from an existing provider. A copy also
   takes the key selection of that provider.
4. Click **Create provider**. The provider screen opens.
5. Set the fields:

   | Field | Value |
   | --- | --- |
   | **Model** | The model name, as the service defines it. |
   | **Reasoning effort** | **None**, **Low**, **Medium**, **High** or **Extra high**. The field does not show for **Google (Gemini)** and **Anthropic**. |
   | **Base URL** | The address of the service, without the API path. See the table above. |
   | **API format** | See the table above. |
   | **Credentials** | **No credentials**, **OpenAI OAuth** (the ChatGPT connection), or a saved API key. |
   | **Inference targets** | Shows only when the host of the **Base URL** is `openrouter.ai` and the format is **Responses** or **Chat completions**. Type one OpenRouter provider name on each line to send requests only to those providers. |

6. Click **Test**. CHA sends one short request with the values on the screen.
   The result is **Provider responded successfully.** or **Provider test
   failed:** with the reason.
7. Click **Save changes**.

A new provider with the default settings has the address
`https://api.openai.com`, the model `gpt-5`, the format **Responses**, and no
credentials.

A provider can have more settings than the screen shows, for example a
temperature, an output limit and time limits. You can change them only in the
configuration files. **Save changes** sets some of them again. See
[system/providers](#systemprovidersidconfigtoml).

### Rules for the ChatGPT subscription

A provider with **Credentials** = **OpenAI OAuth** must have the **Base URL**
`https://chatgpt.com/backend-api/codex` and the **API format** **Responses**.
CHA refuses other values. This selection is not available for **Google
(Gemini)** and **Anthropic**.

### Reasoning effort, where it comes from

The provider gives the start value. A character can replace it
(**Character settings → Reasoning effort**). A user can replace it for one
session with the **Effort** list of the chat toolbar.

Providers with the format **Google (Gemini)** or **Anthropic** ignore all
reasoning effort settings.

### Rename and delete a provider

A new name is safe. All references use the ID.

CHA refuses to delete a provider with the message **This provider is still in
use.** when one of these uses it:

- A character. The list **Used by** shows the characters.
- **Session naming** in Session settings.
- The **Compression provider** in Notes settings.

CHA does not refuse the deletion when only a forum Library uses the provider
as its **Default provider**. The Library then ignores that selection, and the
log gets a warning.

### Connect a ChatGPT account

This connection lets providers with **OpenAI OAuth** use a ChatGPT
subscription. CHA can have one connected account. The connection applies to
all vaults of this computer.

1. Click **Settings**. The card **OpenAI** shows **Not connected**.
2. Click **Connect ChatGPT**. The card shows **Waiting for approval**, a link
   to the ChatGPT device page, and a code.
3. Open the link in a web browser. Sign in, and type the code.
4. Approve the access. The card shows **Connected to ChatGPT**.

A connection attempt stops after 15 minutes. To stop it earlier, click
**Cancel**.

To remove the connection, click **Disconnect**.

CHA stores the connection in the file `openai-auth.json` in the configuration
directory. The file is not part of a vault. A vault transfer and an export do
not include it. Treat this file as a secret.

CHA renews the connection automatically. If the service refuses the renewal,
CHA removes the connection, and requests fail with **Sign in to ChatGPT before
using this provider.** Connect again.

For a server without a web browser, see
[Connect a ChatGPT account on the server](#connect-a-chatgpt-account-on-the-server).

## 9. Personas

A persona is the identity that the user speaks as in a forum. Each forum has
one persona. The characters get the name and the profile of the persona as a
description of the person that they talk to.

### The persona Guest

**Guest** is built in. You cannot change or delete it. Select **Guest** for a
forum that needs no special persona.

### Create a persona

1. Click **Settings → Personas → New persona**.
2. Type the **Persona name**.
3. Click **Create persona**. The persona screen opens.
4. Click **Edit persona profile** and write the profile: who this person is,
   and what the characters must know about the person. Click **Save**. The
   upload button next to it replaces the profile with the content of a
   Markdown or text file.

### Rules for names

The names of personas and characters must be different from each other.
Capital letters do not make a name different. A name:

- Cannot be empty, and cannot start or end with a space.
- Cannot start with `@` or `/`.
- Cannot be one of these words: `persona`, `system`, `error`, `human`,
  `assistant`, `agent`, `character`, `you`, `guest`.

When a name is not permitted, the screen shows only **Invalid persona.** or
**Invalid character.**

### Persona settings

On the persona screen, click **Settings**. The only field is **Style**. It
sets how the messages of this persona look in the conversation. See
[Styles](#12-styles).

### Delete a persona

CHA refuses to delete a persona that a forum uses: **This persona is still
used by one or more forums.** Select a different persona for those forums
first.

### Select the persona of a forum

- For a new forum: the **Persona** list on the **New forum** screen.
- For an existing forum: the **Persona** list on the **Members** screen. See
  [Members](#members).

## 10. Characters

A character is an AI participant. It has:

- A name and a short description.
- **Character settings**: provider, reasoning effort, web search, style and
  voice.
- A profile: the text that tells the model who the character is.
- Optional files: more text, nicknames and pictures.

### Create a character

1. Click **Settings → Characters → New character**.
2. Type a **Name** and a **Description**. The description is a short line for
   the lists. You cannot change it later on the screens.
3. Click **Create character**. The character screen opens.
4. Click **Character settings**, select a **Provider**, and click **Save**.
5. Go back to the character screen. Click **Edit character profile**, write
   the profile, and click **Save**.

A character without a provider is a draft. You cannot add a draft to a forum.

The rules for names are the same as for personas. See
[Rules for names](#rules-for-names).

CHA has no command to copy a character.

### Character settings

| Field | Values | Function |
| --- | --- | --- |
| **Provider** | A provider | The model service and the model. |
| **Reasoning effort** | **Provider default**, **None**, **Low**, **Medium**, **High**, **Extra high** | Replaces the reasoning effort of the provider. |
| **Provider web search** | **Provider default**, **Off**, **Automatic** | The web search function of the model service. See [Two kinds of web search](#two-kinds-of-web-search). |
| **On-demand web search** | **Workspace default**, **On**, **Off** | The web search and page reading that CHA does. **Off** also stops page reading for this character. |
| **Style** | **No style**, or a style | How the text of the character looks. See [Styles](#12-styles). |
| **Voice** | **Application default**, or a voice | The voice for audio. See [Voices](#16-voices). |

Below **Voice** there is a text field for a voice test. The button **Play
preview** shows only when voice output is configured.

If a saved provider, style or voice does not exist any more, the list shows
its ID with **(not available)**.

### The profile and the instructions

The text that a model gets for a character has these parts, in this order:

1. A protocol that CHA generates: the name and the nicknames of the character,
   the name of the persona, and the other members of the forum.
2. The instructions that all characters share.
3. The profile of the character.
4. The instructions of the forum. See [Forum instructions](#forum-instructions).
5. The profile of the persona.

**The shared instructions.** Click **Settings → Characters → Instructions**.
This text applies to all characters of the vault. Use it for rules on how
characters write and behave. When you save it, the open sessions start again
with the new text.

**The profile.** Click **Edit character profile** on the character screen.

**See the result.** The **Members** screen of a forum shows the complete text
for each character. See [See the system prompt](#see-the-system-prompt).

The profile and the instructions can include other files and use variables.
See [Prompt files and the template language](#30-prompt-files-and-the-template-language).

### Files

On the character screen, click **Character settings**, then **Files**. The
list shows the text files and the pictures of the character.

To add a file:

1. Click **New file**.
2. Type the **Filename**.
3. Click **Upload file** and select a file, or click **Write text** and type
   the content.
4. Click **Add file**.

Text files have the extension `.md` or `.txt`. If you type no extension, CHA
adds `.md`. A text file has an effect only in these cases:

- The profile or the instructions include it.
- It is the nicknames file.
- Its name has the form `_<number>_<number>.md`, for example `_1_2.md`. Such
  a file is a variant. For each session, CHA selects some of the variant
  files of the character and adds their text to the profile. The first number
  is a group. CHA uses a maximum of one file of each group in one session.
  Do not give this form of name to other files.

On the screen of a file you can rename it, edit it, replace its content from a
file, and delete it.

The Settings screens do not show the file `CHARACTER.md`, which is the frame
of the character text. To change it, use the configuration files. See
[Configuration files](#21-configuration-files-export-edit-import).

### Nicknames

A nickname is a second name that users can call the character.

1. Open **Character settings → Files → New file**.
2. Type the file name `nicknames.txt`.
3. Click **Write text**. Type one nickname on each line. A line that starts
   with `#` is a comment.
4. Click **Add file**.

CHA uses nicknames in two places: the character knows its nicknames, and
automatic recipient detection accepts them. The `@` addressing in a message
does not use nicknames.

### Pictures

The picture panel of the chat shows the pictures of a character.

1. Open **Character settings → Files → New file**.
2. Click **Upload file** and select the picture.
3. Click **Add file**.

The formats are PNG, JPEG, WebP and GIF. The limit is 8 MB for each picture.
A character can have more than one picture. CHA stores the pictures in the
vault database.

### The Assistant

**Assistant** is a built-in character. It is the only member of the built-in
forum **Entrance**, and it is the first member of each new forum.

- You can change its **Character settings** and its pictures.
- You cannot change its name or its text, and you cannot delete it.
- It must always have a provider that exists. When no provider is selected
  for session naming, CHA uses the provider of the Assistant for that task.

### Delete a character

CHA refuses to delete a character that is a member of a forum: **This
character is still used by one or more forums.** Remove it from those forums
first. The deletion removes the definition, the settings and the files of the
character. The conversations stay.

## 11. Forums

A forum is a group of characters with one persona. Each forum has its own
sessions and its own folder in Notes.

### The forum Entrance

**Entrance** is built in. Its member is the Assistant, and its persona is
Guest. You cannot change or delete it.

### Create a forum

1. Click **Settings → Forums → New forum**.
2. Type a **Name**.
3. Select a **Persona**.
4. Click **Create forum**. The forum screen opens.

The new forum has the Assistant as its only member.

Rules for the name: a maximum of 200 characters; no space at the start or the
end; different from all other forums. The names **Entrance** and
**ANNOTATIONS** are reserved.

### Open the forum screen

1. Click **Settings → Forums** and then the forum, or click the forum in the
   sidebar. The **Sessions** screen of the forum shows.
2. Click the row with the forum name and the member names.

The forum screen shows the files of the forum. It has the link **Members**.

### Members

On the forum screen, click **Members**.

- **Persona** selects the persona of the forum.
- The list shows all characters of the vault. Select the check box of each
  character that is a member.
- Click **Save**.

Rules:

- A forum must have a minimum of one member.
- Each member must have a provider. If not, the message is **Select a
  configured persona and at least one configured character.**
- The members have no order that you can set.

### See the system prompt

On the **Members** screen, click **…** on the row of a character. A dialog
shows the text that the model gets for this character in this forum. It does
not show the parts that CHA adds for one session, for example the list of the
Library stores. **Copy** puts the text on the clipboard. The dialog also works
for a character that is not a member, if the character has a provider.

Use this dialog to make sure that your profiles, instructions and variables
give the result that you want.

### Forum instructions

The file `FORUM.md` holds the instructions of the forum. All members get this
text. A new forum has an empty `FORUM.md`.

1. On the forum screen, click `FORUM.md` in the list of files.
2. Click **Edit forum file**, write the text, and click **Save**.

You cannot delete `FORUM.md`. With **New file** you can add more Markdown
files. A file has an effect only when `FORUM.md` includes it. See
[Prompt files and the template language](#30-prompt-files-and-the-template-language).

### Library

On the **Members** screen, click **Library**. See
[Select the Library of a forum](#select-the-library-of-a-forum).

### Rename and delete a forum

When you rename a forum, CHA also renames its folder in Notes.

> **WARNING:** When you delete a forum, CHA deletes all its sessions, its
> folder in Notes, and all notes in that folder. You cannot undo this.

### Settings that have no screen

These settings of a forum exist only in the configuration files:

- The description of the forum.
- Text that only one member gets in this forum.
- Variables for the texts of this forum.

See [forums](#forumsforum-idconfigtoml).

## 12. Styles

A style sets how the text of a character or a persona looks in the
conversation.

### Create a style

1. Click **Settings → Styles → New style**.
2. Type a **Name** and click **Create style**.
3. Set the fields and click **Save style**.

| Field | Values |
| --- | --- |
| **Font** | **Sans**, **Serif**, **Mono** |
| **Slant** | **Normal**, **Italic** |
| **Weight** | **Light**, **Normal**, **Medium**, **Semibold**, **Bold** |
| **Size** | **Small**, **Normal**, **Large** |
| **Text color** | **Normal**, **Muted**, **Accent** |

The screen shows a sample text with the style.

### Use a style

- For a character: **Character settings → Style**.
- For a persona: the persona screen, **Settings → Style**.

### Delete a style

The list **Used by** shows the characters and personas that use the style. CHA
refuses to delete a style that is in use: **This style is still used by one or
more characters.**

## 13. Web search and page reading

### Two kinds of web search

CHA has two functions that are independent of each other.

| | Provider web search | On-demand web search and page reading |
| --- | --- | --- |
| Who does the search | The model service | CHA, with the services of the **Search API** screen |
| Key | None | A key for the search service, and one for the page reader |
| Works with | Providers with the format **Responses**, and providers at `openrouter.ai` | All API formats. The model must support function calls. |
| Where you set it | **Character settings → Provider web search** | **Settings → Search API**, and **Character settings → On-demand web search** |

**Provider web search.** Select **Automatic** for the character. The model
service then decides when to search. If the provider of the character cannot
do this, CHA ignores the setting.

**On-demand web search.** CHA gives the model two tools: one to search the
web, and one to read a page. The character decides when to use them.

### Configure the Search API

1. Save the API keys of the services. See [API keys](#7-api-keys).
2. Click **Settings → Search API**.
3. Set the fields of the search:

   | Field | Value |
   | --- | --- |
   | **On-demand web search** | Select the check box to let all characters search. This is the **Workspace default** of the character setting. |
   | **API provider** | **Brave Search API** or **Tavily**. |
   | **API key** | The key of that service. Necessary when the check box is selected. |

4. Set the fields of page reading:

   | Field | Value |
   | --- | --- |
   | **Page reading provider** | **Off** or **Firecrawl**. |
   | **Firecrawl API key** | The key of Firecrawl. Shows only for **Firecrawl**. |
   | **Use web_reader** | Select the check box to use a web_reader service. |
   | **web_reader URL** | The base address of the web_reader service, for example `http://<host>:<port>`. |
   | **web_reader API key** | CHA saves this selection but does not send the key. |

5. Click **Save**.

### How the settings work together

- A character can search when **On-demand web search** is on for it (by the
  check box, or by **On** in its settings) and the search key exists.
- A character can read pages when page reading is configured and its own
  **On-demand web search** setting is not **Off**. Page reading does not
  depend on the check box of the Search API screen.
- When **Use web_reader** is on, CHA calls web_reader first. If that fails and
  Firecrawl is selected with a key, CHA calls Firecrawl.

### web_reader

web_reader is a service that you supply. CHA sends a `POST` request to
`<web_reader URL>/extract`, with a JSON body that contains the address of the
page. The service must answer with the status `200` and with JSON that has
only the three text fields `url`, `title` and `markdown`. CHA does not send a
key to it. CHA is not supplied with such a service.

### Page import into Notes

The import of a web page into a note uses the same page reading settings. If
neither web_reader nor Firecrawl is configured, the message is **Page reading
is not configured. Enable web_reader or Firecrawl in Settings → Search API.**

For a YouTube address, and for a page with very long paragraphs, CHA also
sends the text to the compression provider to format it. Thus the import of a
video transcript also needs a compression provider. See
[Notes settings](#15-notes-settings). The prompts **Page formatting** and
**YouTube transcript formatting** tell the model how to format the text. See
[Prompts](#prompts). CHA gets the transcript text from the page reading
service. It has no connection of its own to YouTube.

## 14. Session settings

Click **Settings → Session settings**. The screen has two forms.

### Session naming

After the first message of a session, CHA asks a model for a short name.

| Field | Value |
| --- | --- |
| **Provider** | The provider that makes the names. |
| **Effort** | The reasoning effort for this task. The default is **Low**. |

Click **Save session naming**.

Until you save a provider here, CHA uses the provider of the Assistant. If the
model does not give a valid name in 10 seconds, the session gets the date and
time as its name.

A fast, low-cost model is sufficient for this task.

### Recipient detection

Recipient detection reads each message that has no `@` address, and finds the
character that the message is for. The User Manual describes the results.

This function does not use a provider. CHA sends the message and the list of
characters, with their nicknames, to a decision service.

| Field | Value |
| --- | --- |
| **URL** | The address of the decision service. |
| **Model** | The model name at that service. |
| **API key** | A saved key for that service. |

1. Fill the three fields. Before the first save, the fields show start values.
   Nothing is active until you save.
2. Click **Save recipient detection**.

To stop the function, click **Disable recipient detection**.

The service must accept a `POST` request with a bearer key and a JSON body
that has the fields `model`, `state` and `questions`, and it must return the
selected choice. CHA waits a maximum of 5 seconds. If the service fails, the
message goes to the current target, and the conversation shows **Recipient
detection failed.**

When recipient detection is not configured, each message without `@` goes to
the current target.

## 15. Notes settings

Click **Settings → Notes settings**.

### Compression provider

| Field | Value |
| --- | --- |
| **Compression provider** | The provider for the tasks below. |
| **Effort** | The reasoning effort. The default is **High**. |

Click **Save**.

CHA uses this provider for four tasks:

- **Compress** on a note or a folder.
- **Summarize** on a note or a folder. It makes L1, L2 and the **Summary**
  note of a folder.
- **Save summary** on a session.
- The formatting of imported video transcripts and of pages with very long
  paragraphs.

Without it, these functions show **Select a compression provider in Notes
settings.** Each task sends the text of its prompt to the model. See
[Prompts](#prompts).

These tasks send long texts. Select a provider whose model accepts long input.
If the provider uses the ChatGPT subscription and no account is connected, the
message is **Connect your OpenAI account in Settings before compressing
notes.**

Some models do not accept a reasoning effort. With such a model, each task
fails with the message **This compression provider's model does not support
high reasoning effort.** Set **Effort** to **None**.

### Flex processing for Summarize

For **Summarize**, CHA requests Flex processing when the compression provider
meets all these conditions:

- It uses an API key, not the ChatGPT subscription.
- Its host is `api.openai.com` (API format Responses or Chat Completions), or
  `generativelanguage.googleapis.com` (API format Google).

Flex costs less, but the answers can come much later. The other tasks, and
providers at other hosts, do not use Flex.

There is no setting for Flex and no fallback to the standard processing. The
model must accept Flex. If it does not, each **Summarize** request fails, and
Events records the failure. Then select a provider with a different model.
This also changes the provider of the other tasks above.

If the service has no capacity, CHA tries a request three more times, after
10, 20 and 40 seconds. A request has no time limit. The user can stop it with
**Cancel**.

### Note audio service

These two fields show only in the desktop application.

| Field | Value |
| --- | --- |
| **Audio service URL** | The origin of the service: `https://<host>` or `https://<host>:<port>`, with no path. `http://` is also permitted. |
| **Audio API key** | A saved key for the service. |

Click **Save**. CHA examines the address only when a user starts **Generate
audio**.

The service must have the batch interface of the Gemini API below this
address, and it must accept the key in the header `x-goog-api-key`. The
Google service `https://generativelanguage.googleapis.com` is permitted.

CHA uses the model `gemini-3.8-flash-tts`. You cannot change it.

The dialog **Generate audio** gets the list of voices from the service, at
`<Audio service URL>/v1beta/voices`. It shows only English and Russian voices.
If the service does not give the list, the dialog shows **Voices
unavailable** and the reason.

With the Google service, CHA also asks for the style of an audiobook narrator.

The service must return WAV audio.

Note audio generation works only in the desktop application on macOS and
Windows. ChaWeb cannot start it.

### Prompts

A prompt is the text that CHA sends to the model as the instructions for one
task. The model gets the note, the page or the session as a separate message.
Each vault has its own prompts.

| Prompt | Task |
| --- | --- |
| **Note compression** | **Compress**: the compressed version of a note. |
| **Summary L1** | **Summarize**: the L1 of a note. Its default text is a copy of the default text of **Note compression**. |
| **Summary L2** | **Summarize**: the L2 of a note. |
| **Summary L3 section** | **Summarize** on a folder: the section of one note in the **Summary** note. |
| **Summary L3 overview** | **Summarize** on a folder: the overview at the start of the **Summary** note. |
| **Short session summary** | **Save summary**: the short summary of a session. |
| **Long session summary** | **Save summary**: the long summary of a session. |
| **Page formatting** | The formatting of an imported page with very long paragraphs. |
| **YouTube transcript formatting** | The formatting of an imported video transcript. |

You cannot add, rename or delete a prompt.

To change a prompt:

1. Click **Settings → Prompts** and click the prompt. The screen shows its
   text as plain text. Markdown marks show as they are.
2. Click **Edit text**. To start from a file, click **Upload text** and select
   a UTF-8 `.txt` or `.md` file. The file replaces the text in the dialog. It
   does not save it.
3. Change the text and click **Save**. **Cancel** or Escape discards the
   change.

The change applies to the tasks that start after the save. A task that is in
work keeps the old text. Saving makes no request to the model and does not
start the sessions again.

Rules for the text:

- It is plain UTF-8 text of a maximum of 256 KiB. Includes and variables do
  not work. CHA sends the text as it is. See
  [Prompt files and the template language](#30-prompt-files-and-the-template-language).
- An empty text is permitted. The model then gets no instructions.
- CHA has no command to go back to the default text. To get the default text
  of one prompt, delete its file with the configuration files and import. See
  [The files of the prompts](#the-files-of-the-prompts).

Rules for the summary prompts:

- **Save summary** makes two model requests with the same session text: one
  with **Short session summary** and one with **Long session summary**. CHA
  adds the headings *Short summary* and *Long summary* itself. Thus these
  prompts must ask for text without a level-1 heading. If the answer has a
  level-1 heading, the summary fails.
- For **Summary L2**, **Summary L3 section** and **Summary L3 overview**, CHA
  adds the name of the note or of the folder as a level-1 heading. Thus these
  prompts must not ask for a title.
- The results of **Summarize** are in Markdown. CHA changes tables into lines
  of text.

A new version of CHA does not change the prompts of a vault. A vault that has
no prompts gets the default texts when the main process opens it.

## 16. Voices

Voice has three parts that you configure independently:

- **Voices**: the list of voices that characters can have.
- **Voice output**: the service that makes audio from the answers.
- **Voice input**: the service that makes text from speech.

Do the tasks in this order: save the API keys, register a minimum of one
voice, configure voice output, then configure voice input.

### Register a voice

A voice in CHA is a name for a voice of the output service.

1. Click **Settings → Voices → New voice**.
2. Set the fields:

   | Field | Value |
   | --- | --- |
   | **Name** | The name that the lists in CHA show. |
   | **Description** | A text for you. CHA does not send it to the service. |
   | **Provider** | **FishAudio** or **ElevenLabs**. |
   | **Voice ID** | The identifier of the voice at that service. |

3. Click **Register voice**. The button is disabled until all four fields
   have a value.

On the screen of a voice you can also set **Speed**, from 0.7 to 1.2. The
field does not show for an ElevenLabs voice when the name of the output model
starts with `eleven_v3` or `eleven_v4`. With
**Play preview** you can listen to the voice. The button shows only when voice
output is configured. Each preview is a request to the service.

CHA refuses to delete a voice that a character uses, or that is the default
voice: **This voice is still in use.**

### Configure voice output

1. Click **Settings → Voices → Voice settings**.
2. In the second form, select the **Output provider**: **FishAudio** or
   **ElevenLabs**. You can configure the two services, one after the other.
   The list only selects which settings the form shows.
3. Set the fields:

   | Field | FishAudio | ElevenLabs |
   | --- | --- | --- |
   | **Output URL endpoint** | `https://api.fish.audio/v1/tts` | `https://api.elevenlabs.io/v1/text-to-speech` |
   | **Output model name** | Default `s2.1-pro` | Default `eleven_multilingual_v2` |
   | **Connection** | **HTTP** or **WebSocket** | **HTTP** or **WebSocket** |
   | **Output API key name** | A saved key | A saved key |
   | **Output format** | `mp3`, `wav` or `opus` | One of the `mp3_…` and `opus_…` format names of the service, for example `mp3_44100_128` |

4. Select the **Default voice**. Characters with **Application default** use
   this voice.
5. Click **Save voice output**.

Rules:

- The address must use HTTPS and the host of the service. CHA refuses other
  hosts.
- With **HTTP**, CHA makes the audio of an answer after the answer is
  complete. With **WebSocket**, CHA can also speak an answer while the model
  writes it.
- The service of a voice is the **Provider** of that voice. The service of the
  default voice must be configured.

The speaker buttons show when voice output is configured, the default voice
exists, and its key exists. A user can always play audio that CHA has already
stored.

### Configure voice input

1. Click **Settings → Voices → Voice settings**.
2. In the first form, set the fields:

   | Field | OpenAI | xAI |
   | --- | --- | --- |
   | **Input provider** | **OpenAI** | **xAI** |
   | **Input URL endpoint** | Default `https://api.openai.com/v1/realtime/calls` | Default `wss://api.x.ai/v1/stt` |
   | **Input model name** | Default `gpt-live-transcribe` | Default `grok-voice-transcribe-2.0` |
   | **Input API key name** | A saved key | A saved key |
   | **Hands-free send phrase** | See below | See below |
   | **Input delay** | **Low**, **Medium**, **High**, **Extra high** | Not available |
   | **Input prompt** | An optional text for the transcription model | Not available |

3. Click **Save voice input**.

When you change the **Input provider**, the form sets the address and the
model to the defaults of that service and removes the key selection.

The microphone button shows when voice input is configured and its key
exists.

With **OpenAI**, the computer that shows CHA sends the audio directly to the
service. With **xAI**, CHA sends the audio from the application, or from the
server for ChaWeb.

### The send phrase

**Hands-free send phrase** is the phrase that sends a dictated message. The
default is `over to you`. Each vault has its own phrase.

- Capital letters and punctuation are not important.
- The phrase must be at the end of the dictated text.
- If the field is empty, the function is off.

You can save the phrase only together with a complete voice input
configuration.

### Give a character a voice

Open **Character settings** of the character, select the **Voice**, and click
**Save**. With **Application default**, the character uses the **Default
voice** of the voice output settings.

A persona has no voice.

### Stored audio

CHA stores the audio of answers and of notes in the vault database. The
commands to remove the audio of one session are in the User Manual. An R2
transfer does not include audio. See [R2 storage](#19-r2-storage).

## 17. Cloud stores and the Library

A cloud store is a searchable copy of notes at a provider: a vector store at
OpenAI, or a File Search store at Google. A Library is the set of stores that
you select for one forum.

### Prepare a key

A cloud store needs an API key of the OpenAI API or of the Gemini API.

1. Click **Settings → API Keys**, and create or open the key.
2. Set **Cloud store** to **OpenAI** or **Gemini**.
3. Save.

Only keys with this setting show in the store dialogs.

### Create a store

A user creates a store with **Upload to store** in the menu of a Notes folder.
The User Manual describes this command.

CHA keeps no connection between the folder and the store. The only record of
the upload in CHA is an entry in [Events](#events). It contains the IDs of the
store and its files.

### Manage stores

1. Click **Settings → Cloud stores**. The list shows the keys that have a
   **Cloud store** setting.
2. Click a key. The list shows all stores that this key can see, also the
   stores that CHA did not make. **Refresh** reads the list again.
3. Click a store.

On the store screen you can change the **Name** and the **Description**, then
click **Save**. A good description helps the characters to decide when to
search the store. For an OpenAI store, the description has a maximum of 512
characters.

To delete the store, click **Delete** and confirm.

> **WARNING:** The deletion removes the store and all its files at the
> provider. For an OpenAI store, CHA deletes each file of the store. If a
> different vector store of the same account uses one of these files, that
> store loses the file too.

To update a store with new versions of the notes, delete the store and upload
the folder again.

### Select the Library of a forum

1. Open the forum screen and click **Members**, then **Library**.
2. Select the **API key**.
3. In **Cloud stores**, select the stores.
4. If necessary, select a **Default provider**. See below.
5. Click **Save**.

Rules:

- A Library uses one key. Thus all its stores are at one service.
- With an OpenAI key, a Library can have a maximum of two stores.
- To remove the Library, select **None** in **API key** and click **Save**.

CHA copies the name and the description of each selected store into the forum
when you save. If you change them later on the store screen, open the Library
again and click **Refresh**.

### How the characters search

CHA selects the method for each character:

- **The service searches.** This occurs when the provider of the character
  connects directly to the service of the Library with the same key: an
  OpenAI provider with the format **Responses** at `https://api.openai.com`,
  or a Gemini provider at
  `https://generativelanguage.googleapis.com/v1beta`.
- **CHA searches.** For all other providers, CHA gives the model a search tool
  and does the search with the key of the Library. The model must support
  function calls. For a Gemini Library, CHA uses the model `gemini-3.8-flash`
  for this search, at the cost of the Library key.

When the service searches and the model of the provider is a `gemini-2.5`
model, the character cannot use on-demand web search, page reading and the
summaries of other sessions in that forum.

### Default provider

The **Default provider** list shows only the providers that connect directly
to the service of the Library with the Library key.

When you select one, **all** characters of the forum use this provider in
place of their own, and in place of a provider that a user selected for a
session. The **Model** list of the chat toolbar is then disabled in this
forum.

## 18. Vaults

### What a vault is

A vault is two files in the configuration directory:

- A definition file, for example `vault-1.toml`. It contains the name of the
  vault and the path of the database.
- A database file, for example `<vault name>.sqlite3`. It contains the
  configuration, the saved keys, the sessions, the notes, the stored audio,
  and the Events.

See [Files in the configuration directory](#26-files-in-the-configuration-directory)
for the other files that CHA makes next to a database.

To select the active vault, use the **Vault** list at the bottom of the
sidebar. See the User Manual. CHA writes the selection into `app.toml`, and
starts with that vault the next time.

### Create a vault

1. Click **Settings → Vaults → New vault**.
2. Type the **Display name**.
3. Select the **Initial database**:
   - **New empty vault** — the new vault gets a copy of the configuration of
     the active vault: characters, forums, providers, saved keys and settings.
     It gets no sessions and no notes. It gets the default texts of the
     [prompts](#prompts), not the texts of the active vault.
   - **Copy** *vault name* — the new vault gets a full copy of that vault,
     with its prompts. To copy a protected vault, make it the active vault
     first.
4. To give the vault a password, select **Protected vault** and type the
   **Password**.
5. Click **Create vault**.

The screen of the new vault opens. The new vault is not active. To use it,
select it in the **Vault** list.

Rules for the name: no characters `/ \ < > : " | ? *`; no space at the start
or the end; no period at the end; different from all other vaults. Capital
letters do not make a name different. The Windows device names, for example
`CON`, `NUL` and `COM1`, are not permitted.

> **CAUTION:** With the two selections, the new vault gets the saved keys, the
> R2 credentials and the D1 credentials of its source. With D1 credentials,
> the new vault does not synchronize until you select it and click **Save D1
> credentials**. Until you do this, or click **Disconnect from D1**, CHA
> refuses an R2 upload of the new vault after its first change. See
> [D1 synchronization](#20-d1-synchronization).

### Rename a vault

Open **Settings → Vaults**, click the vault, and click its title.

CHA changes the name in the definition file and renames the directories of the
vault in the `mirror` and `modify` directories. The name of the database file
does not change.

> **CAUTION:** Do not rename a vault that uses D1 synchronization. The vault
> name is part of the synchronization. See
> [D1 synchronization](#20-d1-synchronization).

If the vault has a copy in R2, upload it again after you rename it. Until
then, **Download** fails, because the copy in R2 has the old name.

### Protect a vault with a password

A protected vault has an encrypted database. CHA asks for the password each
time it opens the vault.

To protect an existing vault:

1. Click **Settings → Vaults** and click the vault.
2. Select **Protected vault**.
3. Type the **Password**.
4. Click **Protect vault**.

> **WARNING:** You cannot change the password, and you cannot remove it. If
> you lose the password, you lose the vault. CHA has no recovery function.
> Keep the password in a safe place before you click **Protect vault**.

What the password does and does not protect:

- The database file is encrypted: configuration, saved keys, sessions, notes.
- The definition file and `app.toml` are not encrypted.
- CHA writes no session mirror for a protected vault.
- The files that **Export** writes are not encrypted.
- Files that CHA made before the protection stay on the disk as they are: the
  session mirror of the vault, an export in the `modify` directory, and a
  `.bac` file. Delete them yourself.

To get a vault with a different password, or with no password, create a new
vault with **Copy** *vault name* and set the password that you want. The copy
is a new vault with a new name.

### Merge the configuration of a different vault

**Merge** copies the configuration of a different vault into the active vault.
It does not copy sessions or notes.

1. Click **Settings → Vaults**, click the active vault, and click **Merge**.
2. Select the **Source vault** and click **Merge**.
3. Confirm.

When the two vaults have the same item, the item of the source replaces the
item of the active vault. This includes the saved keys, the R2 credentials
and the prompts. You cannot undo a merge. The D1 credentials of the active
vault do not change.

If the source vault is protected, CHA asks for its password.

### Delete a vault

1. Make a different vault active. You cannot delete the active vault or the
   last vault.
2. Click **Settings → Vaults**, click the vault, and click **Delete vault**.
3. Confirm.

CHA deletes only the definition file. The database file and the directories of
the vault stay on the disk. To remove the data, delete the database file
yourself.

Because the database file stays, you cannot create a new vault with the same
name until you move or delete that file.

### The commands of the active vault

The screen of the active vault has these rows:

| Row | Function | Chapter |
| --- | --- | --- |
| **Merge** | Copies the configuration of a different vault into this one. | This chapter |
| **Upload** | Sends the vault to R2. | [19](#19-r2-storage) |
| **Download** | Replaces the vault with the copy in R2. | [19](#19-r2-storage) |
| **Import** | Replaces the configuration with the configuration files. | [21](#21-configuration-files-export-edit-import) |
| **Export** | Writes the configuration to files. | [21](#21-configuration-files-export-edit-import) |

## 19. R2 storage

R2 storage copies a complete vault to a bucket in Cloudflare R2, and gets it
back. Use it to:

- Bring a vault to a second computer.
- Keep a copy of a vault outside the computer.

R2 storage is a manual function. Each upload sends the full database. For the
automatic transfer of changes, see [D1 synchronization](#20-d1-synchronization).

### What you need in Cloudflare

- One R2 bucket.
- One R2 API token for the S3 interface: an access key ID and a secret key.
  The token must permit CHA to read, write and list the objects of the
  bucket.
- The S3 address of your account.

CHA writes its objects at the top level of the bucket. For a vault with the
database file `<file>.sqlite3`, the objects are:

| Object | Content |
| --- | --- |
| `<file>.sqlite3` | The database. |
| `<file>.sqlite3.toml` | The definition of the vault. |
| `<file>.sqlite3.uploader` | The registration of the computer that uploads. |

### Save the R2 credentials

1. Click **Settings → API Keys → R2 storage**.
2. Set the fields:

   | Field | Value |
   | --- | --- |
   | **R2 URL** | `https://<account ID>.r2.cloudflarestorage.com/<bucket>` — the S3 address with the bucket name as the only path part. |
   | **Access key ID** | The access key ID of the token. |
   | **Secret key** | The secret key of the token. |

3. Click **Save R2 credentials**.

The credentials belong to the active vault. CHA examines the address only when
you use it.

To remove them, click **Remove R2 credentials**.

### Upload a vault

1. Click **Settings → Vaults** and click the active vault.
2. Click **Upload**.
3. If CHA asks, confirm. CHA asks when it has no record of an earlier
   transfer, when the copy in R2 is different from the last transfer of this
   computer, or when the copy in R2 does not exist any more.

The row shows **Uploading…**. You can continue to work. The result shows as
**Uploaded** with the size.

Important points:

- The upload does not include stored audio: not the audio of answers, and not
  the audio of notes. The local vault keeps its audio.
- A protected vault stays encrypted in R2.
- If the vault uses D1 synchronization and has changes that are not sent, CHA
  refuses the upload: **Changes are not sent to D1 yet. Try again when D1 can
  be reached.**

### The uploading computer

The first upload of a vault registers your computer as the uploader of that
vault. A different computer cannot upload the same vault. The message is:

```
Only the registered uploader machine can upload this vault to R2. To change it, delete '<file>.sqlite3.uploader' from the bucket.
```

To move the upload function to a different computer, delete that object in the
bucket with the tools of Cloudflare. The next upload registers the new
computer. On Windows, you must also do this after you install Windows again.

Downloads have no such limit.

### Replace the active vault with the copy in R2

1. Click **Settings → Vaults** and click the active vault.
2. Click **Download**.
3. Read the dialog **Replace the local database?** and click **Download**.

> **WARNING:** The download replaces the complete local database: the
> configuration, all sessions and all notes. The changes that you made after
> the upload are lost. All stored audio of the vault is lost, because the copy
> in R2 has none.

CHA keeps the replaced database and definition as files with the extension
`.bac`. It keeps only one such copy. The next download replaces it. See
[Restore from a .bac file](#restore-from-a-bac-file).

### Get a vault that is not on this computer

1. Make sure that the active vault has R2 credentials for the bucket. On a
   new installation, save them in the vault **Default**.
2. Click **Settings → Vaults → Download vault**. The list shows the databases
   of the bucket that are not on this computer. Each row shows the name of
   the database file without `.sqlite3`. For a vault that was renamed, this is
   not the name of the vault.
3. Click a row. CHA downloads the vault without a confirmation.
4. Select the new vault in the **Vault** list of the sidebar.

CHA cannot download a protected vault with this command. Protected vaults show
in the list, but the download fails. To move a protected vault, copy its
files. See [Move a vault to a different computer](#move-a-vault-to-a-different-computer).

### If a transfer fails

The screen shows a short message, for example **Upload failed.** or **The
request could not be completed.** The reason is in the log. See
[The log](#the-log). Frequent causes are a wrong address, a token without the
necessary permissions, and no network connection.

## 20. D1 synchronization

D1 synchronization sends the changes of a vault to the other computers that
use the same vault. It uses one database in Cloudflare D1 as the transport.

### How it works

- Each computer has a name, the `machine` name.
- For each vault and each computer, CHA makes one table in the D1 database.
  The name of the table is `q/<vault name>/<machine name>`.
- When you change something, CHA puts a record into the tables of the other
  computers. Each computer reads its own table, applies the records, and
  deletes them.
- CHA does this when it opens the vault, and then every 30 seconds. When it
  closes, it sends its changes one more time. There is no command to start
  it.

CHA applies the records in the order in which they come. It does not compare
versions. If you change the same item on two computers at the same time, the
change that comes last replaces the other.

Thus a change is on the other computers after a maximum of approximately one
minute, if the two computers are in operation and connected.

### What is synchronized

| Synchronized | Not synchronized |
| --- | --- |
| Notes and folders, with the compressed version, L1, L2 and the **Exclude from summaries** setting of each note | Stored audio of answers and notes |
| Configuration: personas, characters, forums, providers, styles, voices, settings, prompts, pictures | Events |
| Saved API keys and R2 credentials | D1 credentials |
| Sessions that have a name and whose first message is complete (answered, failed or stopped) | The `machine` name, `app.toml`, the vault password |
| | The ChatGPT connection |

When a note changes through synchronization, CHA deletes the stored audio of
that note. A new compressed version, L1 or L2 from **Compress** or
**Summarize** on a different computer does not delete the audio of the full
text. A new L1 deletes the audio of the L1.

### Rules

- Each computer must have a different `machine` name. Capital letters do not
  make a name different.
- The vault must have the same name on all computers.
- Only the main process synchronizes.
- D1 moves changes. It does not move a complete vault. A new computer first
  gets the vault through R2.

### What you need in Cloudflare

- The account ID.
- One D1 database, and its database ID. The database can be empty. CHA makes
  its tables.
- An API token that permits CHA to read and to change that D1 database.

### Set up the first computer

1. Close CHA.
2. Open `app.toml` in the configuration directory and add this line above the
   line `[logging]`:

   ```toml
   machine = "<machine name>"
   ```

   The name cannot be empty and cannot contain `/`.
3. Start CHA and select the vault.
4. Click **Settings → API Keys → D1 sync**.
5. Type the **Account ID**, the **Database ID** and the **API token**.
6. Click **Save D1 credentials**.

CHA makes the table of this computer and starts the synchronization. If the
`machine` name is missing, the message is **This machine has no name. Add
machine to app.toml, then save again.**

### Add a second computer

Do not change the vault on a different computer during this procedure. Changes
that are made between step 1 and step 6 do not come to the new computer.

1. On the first computer, upload the vault to R2. See
   [Upload a vault](#upload-a-vault).
2. On the second computer, set the `machine` name in `app.toml` to a name
   that no other computer uses, and start CHA.
3. Save the R2 credentials in the active vault. See
   [Save the R2 credentials](#save-the-r2-credentials).
4. Get the vault with **Download vault**, and select it in the **Vault** list.
   The vault contains the D1 credentials. For a protected vault, copy the
   files in place of this step. See
   [Move a vault to a different computer](#move-a-vault-to-a-different-computer).
5. Click **Settings → API Keys → D1 sync**. The fields show the saved values.
6. Click **Save D1 credentials**. You can keep **API token** empty. This step
   makes the table of the second computer and starts the synchronization.

Until step 6, the second computer does not synchronize, and no screen tells
you so. The log contains **D1 sync stopped: download the vault from R2, or
save the D1 credentials again.**

### Stop the synchronization

1. Click **Settings → API Keys → D1 sync**.
2. Click **Disconnect from D1** and confirm.

CHA removes the D1 credentials from this vault on this computer, deletes the
changes that are not sent, and deletes the table of this computer in D1.

Disconnect each computer that stops using a vault. If you do not, its table
stays in D1 and continues to collect records.

### Synchronization and R2 together

- An R2 upload is possible only when all changes are sent to D1.
- After **Download** on the active vault, CHA makes the table of this computer
  again, if it is missing, and synchronizes.
- **Import** and **Merge** keep the D1 credentials of the target vault. A new
  vault gets the D1 credentials of its source. See
  [Create a vault](#create-a-vault).

### Problems

The synchronization writes its problems only into the log. No screen shows
them. See [The log](#the-log). Search the log for `D1` and for `Skipped`.

| Log message | Cause and action |
| --- | --- |
| `D1 sync stopped: download the vault from R2, or save the D1 credentials again.` | The table of this computer does not exist in D1. Click **Save D1 credentials** again, or use **Download** on the vault. |
| `D1 rejected the API token.` | The token is wrong or has no permission. Save a correct token. |
| `D1 request was rate limited (HTTP 429).` | CHA tries again in the next cycle. |
| `D1 daily row read limit exceeded.` / `D1 daily row write limit exceeded.` | A limit of the Cloudflare account. CHA tries again later. |
| `D1 sync dropped the largest queue table to free space.` | The D1 database was full. This computer deleted the table with the most waiting records. That table can belong to a different computer of each vault in this D1 database. The computer that lost its table did not get those changes. Its log shows `D1 sync stopped`. To give it a complete copy: upload the vault to R2 from the uploading computer, then use **Download** on the computer that lost its table. |
| `Skipped incoming sync record…` | CHA could not apply one record and ignored it. |

A computer that does not start CHA for a long time collects many records in
its table. If the D1 database becomes full, a computer that sends changes
deletes the largest table of a different computer, as the table shows.

## 21. Configuration files: export, edit, import

The Settings screens are the usual way to change the configuration. Some
settings have no screen. For these, and for large changes, CHA can write the
complete configuration of a vault to files and read it back.

### Principles

- CHA keeps the configuration in the vault database. It does not read the
  files while it operates.
- **Export** writes the configuration to a directory.
- **Import** replaces the configuration with the content of that directory.
- A change in the files has no effect until you import. A change on the
  Settings screens does not change the files. Thus always export immediately
  before you edit.

### The directory

The key `modify` in `app.toml` sets a base directory. Each vault uses the
directory `<modify>/<vault name>`. With the first `app.toml`, this is the
directory `modify/<vault name>` in the configuration directory.

If `app.toml` has no `modify` key, the rows **Import** and **Export** are
disabled and show **Modify directory is not configured**.

### Change the configuration with files

1. Click **Settings → Vaults** and click the active vault.
2. Click **Export**. The result shows as **Exported** with the number of
   files.
3. Edit the files in `<modify>/<vault name>`. See
   [Configuration file layout and keys](#29-configuration-file-layout-and-keys).
4. Click **Import**, and confirm in the dialog **Import workspace
   configuration?**
5. If the screen shows an error, correct the file that the error names, and
   click **Import** again. The vault does not change when an import fails.

You do not have to restart CHA. The open sessions get the new configuration.

> **WARNING:** **Export** writes all saved keys to files as plain text: the
> API keys, the R2 credentials and the D1 credentials. The files are in
> `system/keys/`. This is also true for a protected vault. Delete the export
> directory when you do not need it, and do not put it into a backup or a
> shared directory.

> **WARNING:** **Export** first deletes the complete directory
> `<modify>/<vault name>`, without a confirmation. Changes in that directory
> that you did not import are lost.

> **WARNING:** **Import** replaces the complete configuration. Each item that
> is not in the files is removed from the vault: characters, forums,
> providers, styles, voices and saved keys. When a forum is not in the files,
> CHA also deletes the sessions of that forum and its folder in Notes with all
> its notes.

### What Import reads

- Files with the extension `.toml` or `.md`.
- Files with the extension `.txt`, but only below `characters/`.
- Pictures, but only in the directory of a character and in
  `system/assistant/`.

Import ignores all other files. It stops with an error when one of the files
that it reads is a symbolic link.

Import removes a forum member whose character is not in the files. If a forum
then has no member, Import removes the forum, as the warning above describes.
It gives no message for this.

### What Import keeps

- The sessions and the notes, with the exception in the warning above.
- The D1 credentials of the vault. Import ignores D1 credentials in the files.

Import replaces the prompts with the files in `system/prompts/`. A prompt
without a file gets its default text. If the files contain no prompt at all,
CHA writes the default texts of all prompts into the vault.

### Errors and warnings

- An error in a file stops the import, and the screen shows the text. The
  text names the file and, for a syntax error, the line.
- Some problems are only warnings. The import continues, and the warning goes
  to the log: an unknown key, an obsolete key, a picture in a wrong place.
- If the screen shows only **Import failed.** or a general message, the reason
  is in the log. An example is a missing directory.

### What an export does not contain

Sessions, notes, stored audio, `app.toml`, the vault definition, and the
ChatGPT connection.

## 22. The Assistant as a configuration tool

In the session **Welcome**, the built-in character **Assistant** can examine
and change the configuration when you ask it to. You type what you want in
normal words. It has this function only in the Welcome session. As a member of
a different forum, the Assistant is a normal character.

### What it needs

The Assistant needs a provider that works, and a model that supports function
calls. In a new vault, its provider is `chatgpt`. Thus you must first connect
a ChatGPT account, or select a different provider for it. See
[Minimum setup](#5-minimum-setup-for-the-first-conversation).

To change its provider: **Settings → Characters → Assistant → Character
settings**. The Assistant cannot change its own settings.

### What it can do

| Area | Functions |
| --- | --- |
| Examine | The version, the vaults, the providers, characters, personas, forums, styles, voices and service settings. Which keys exist. The list of sessions and notes. |
| Change the configuration | Create and change characters, personas, forums and their members, providers, styles, voices. Change the session, notes, search and voice settings. |
| Diagnose | Read the log: the current file and `cha.1.log`. Read the Events and the state of background tasks. Change the log level until the next start. |
| Application files | Read and change `app.toml` and the vault definition files. Such a change needs a restart of CHA. |
| ChatGPT | Start the connection of a ChatGPT account and complete it. |
| Web | Search the web and read pages, if [the Search API](#13-web-search-and-page-reading) is configured for it. |

### What it cannot do

- Delete an item, or remove a member from a forum.
- Read or write the value of a key. It sees only which keys exist. It cannot
  save a new key.
- Make a key go to an address that this key does not already use. Thus it
  cannot change the address of a provider or a service that has a key, and it
  cannot select a key for a provider or a service at a new address.
- Change its own settings, or the provider that it uses.
- Change the [prompts](#prompts).
- Read the text of sessions or notes.
- Change pictures.
- Create the database of a vault. Select, protect or delete a vault. Import,
  export, upload or download.
- Start, stop or repeat a background task.
- Run commands on the computer.

### Rules of operation

- A change applies immediately. CHA has no confirmation dialog for it. The
  Assistant is instructed to change something only when you ask for a change,
  and only to examine when you ask a question.
- After a change, the conversation shows **Configuration was saved.** with the
  list of changed files, and tells you if an undo is available.
- You can ask the Assistant to undo its last change. This is possible only
  when the change did not create a new item. An undo is not available after a
  restart of CHA, after a later change of the configuration, or for
  `app.toml` and the vault definition files.
- After a change of `app.toml` or a vault definition file, the conversation
  shows **Host configuration was saved. Restart is required.**
- The other open sessions start again with the new configuration.
- CHA does not keep the Welcome conversation after you close the application.
  The changes stay.
- The Assistant sends the configuration texts and the log lines that it reads
  to the model service of its provider. CHA never sends the value of a key.
  In log lines and Events, CHA replaces key values and private paths before
  it sends them. It sends the configuration files as they are, `app.toml` and
  the vault definition files with their paths included.

### Examples of requests

- “Why does the character Anna not answer?”
- “Create a character with the name Boris for the forum Workshop. Use the same
  provider as Anna.”
- “Change the model of the provider Main to `<model name>`.”
- “Read the Events and tell me why the audio of my note failed.”
- “Set the log level to debug.” Then repeat the problem and ask: “What does
  the log show?”
- “Undo the last change.”

### The Assistant in ChaWeb

In ChaWeb, the Assistant has the same functions. It works on the vault, the
configuration directory and the log of the server. ChaWeb has no Settings
screens, so there the Assistant is the tool to change the configuration and to
read the Events.

### Its knowledge

The knowledge of the Assistant about CHA is part of the application. You
cannot change it.

## 23. Diagnostics: Events and the log

CHA has two records. Events are for the results of background tasks. The log
is for technical diagnosis.

| | Events | Log |
| --- | --- | --- |
| Where | In the vault database | A file on the computer |
| Scope | One vault | All vaults, all sessions |
| How to read | **Settings → Events** | A text editor |
| Size | The newest 1,000 entries | Four files of a maximum of 10 MiB each |

### Events

To read the Events, click **Settings → Events**. The newest entry is first.
You cannot delete entries. ChaWeb has no Events screen. There, ask the
Assistant.

These tasks write Events:

| Task | Entries |
| --- | --- |
| Note compression | Start, result, cancellation, failure with a short reason. |
| **Summarize** | The result of each L1, each L2 and each **Summary** note. Each failure with a short reason, and each cancelled request. |
| **Save summary** | Start, result, cancellation, failure. |
| Note audio | Start with the number of clips and the voice, result, each failed step, and the reason when CHA stops an audio task. |
| **Upload to store** | The result, with the IDs of the store and its files. Problems with the removal of data at the provider. |

A failure entry contains a short reason, for example **Provider rate limit
exceeded.** The full technical message is in the log.

These functions write no Events: conversations, provider requests, R2
transfers, D1 synchronization, and changes of settings. For these, use the
log.

**If an upload to a store fails**, CHA tries to delete the data that it
already sent. If it cannot confirm the deletion, an Event tells you that data
can remain at the provider, and gives the IDs. Delete those items with
**Settings → Cloud stores** or with the tools of the provider.

### Background tasks when CHA closes

| Task | Result |
| --- | --- |
| Note compression, **Save summary** | Cancelled. Start it again. |
| **Summarize** | Stopped. The L1 and L2 that CHA saved stay. The **Summary** note does not change. Start it again. |
| Note audio | CHA continues the task at the next start of the desktop application. |
| **Upload to store** | Stopped. Data can remain at the provider. |
| R2 upload | Stopped. Upload again. |

### The log

The key `[logging]` in `app.toml` sets the file and the level. With the first
`app.toml`, the log is `logs/cha.log` in the configuration directory.

- When the file has 10 MiB, CHA starts a new one and keeps three older files:
  `cha.1.log`, `cha.2.log`, `cha.3.log`.
- Each entry is one line with the date and time, the thread, the level and the
  message.

The levels are `trace`, `debug`, `info`, `warn`, `error`, `critical` and
`off`.

To change the level:

1. Close CHA.
2. Change `level` in `app.toml`.
3. Start CHA.

Or ask the Assistant in the Welcome session to change the level. That change
applies immediately and ends when CHA closes.

> **CAUTION:** At the level `debug`, the log contains the full requests and
> answers of the model services: your messages and the answers. It also
> contains the web searches, the pages that CHA reads, and the requests of
> recipient detection. CHA removes the key of the request from these entries.
> Use `debug` only to find a problem, and then set `info` again.

What you find in the log, and not on a screen:

- Warnings about `app.toml` and the vault definition files, for example an
  unknown key.
- Warnings from an import of configuration files.
- Each problem of D1 synchronization.
- The reason of a failed R2 transfer.
- The reason when a character cannot search the web or the Library, for
  example a missing key.

## 24. Backup and restore

CHA has no backup command. A backup is a copy of files.

### What to copy

For one vault:

- The definition file of the vault (a `.toml` file in the configuration
  directory).
- The database file that the definition names in `data`.
- If they exist, the files with the same name as the database and the endings
  `-wal` and `-shm`.

For the complete installation, copy the complete configuration directory. It
also contains `app.toml`, the ChatGPT connection (`openai-auth.json`), and the
state of the View menu and the font size of notes (`view.json`).

### Make a backup

1. Close all CHA processes that use the vault: each CHA window, and ChaWeb if
   it uses the same files.
2. Copy the files to the backup location.

Do not copy the database while CHA is in operation. The copy can be damaged.

For a protected vault, the backup is encrypted. You need the password to use
it.

> **CAUTION:** The backup of a vault without a password contains all saved
> keys as plain text. The complete configuration directory also contains the
> ChatGPT connection. Keep backups in a safe location.

### Restore a vault

1. Close all CHA processes.
2. If you replace a database, delete the files with the same name and the
   endings `-wal` and `-shm` in the configuration directory.
3. Copy the definition file and the database file back into the configuration
   directory. If the backup contains `-wal` and `-shm` files, copy them too.
4. Make sure that the key `data` in the definition file gives the correct
   path of the database. For a vault that you created or downloaded, CHA
   writes a full path there. If the directory is different now, change the
   value. A file name without a directory is sufficient when the database is
   in the configuration directory.
5. Start CHA.

If the vault uses D1 synchronization, the restored vault does not get the
changes that the other computers sent after the backup. Changes that waited
for D1 at the time of the backup are sent again.

### Restore from a .bac file

When **Download** replaces a vault, CHA keeps the replaced files:
`<database file>.bac` and `<definition file>.bac`. CHA has no command to
restore them. To go back to the replaced vault:

1. Close all CHA processes.
2. In the configuration directory, move the current database file and
   definition file to a different location. Also move the files with the
   endings `-wal` and `-shm`, if they exist.
3. Remove the ending `.bac` from the two `.bac` files.
4. Start CHA.

### Move a vault to a different computer

For a vault without a password, use R2. See
[Get a vault that is not on this computer](#get-a-vault-that-is-not-on-this-computer).

For a protected vault, or without R2:

1. Make a backup of the vault, as above.
2. On the other computer, close CHA. Make sure that its configuration
   directory has no vault with the same name, no definition file with the
   same file name, and no database file with the same file name. Each
   installation has `default.toml` and `default.sqlite3`, and each
   installation starts its file names at `vault-1.toml`. If necessary, give
   the definition file that you copy a different file name. If two vaults
   have the same name or the same database file name, CHA does not start.
3. Restore the vault, as above. Set `data` to the file name of the database.
4. Start CHA and select the vault.

If the vault uses D1 synchronization:

- Before the copy, upload the vault to R2 on the source computer. The upload
  is possible only when no changes wait for D1. Then close CHA and make the
  copy. A copy with changes that are not sent sends them a second time.
- The `machine` name is in `app.toml`, not in the vault. If you copy the
  complete configuration directory, change `machine` on the new computer to a
  name that no other computer uses.
- On the new computer, click **Save D1 credentials**. See
  [Add a second computer](#add-a-second-computer).

### R2 as a second copy

An R2 upload is a copy outside the computer. It does not contain stored
audio. It is one copy: each upload replaces the copy before it.

### The session mirror

With the key `mirror` in `app.toml`, CHA writes each session as a Markdown
file into `<mirror>/<vault name>/<forum name>/`. These files are for reading
with other programs. CHA never reads them back. They are not a backup that you
can restore.

CHA writes no mirror for a protected vault and none for the Welcome session.

## 25. ChaWeb

ChaWeb is the browser client of CHA. It needs a server with Linux.

### Structure

```
browser  →  nginx (HTTPS, one port for each user)  →  socket  →  cha-daemon  →  vault
```

- `cha-daemon` is the CHA program for the server. It has no window.
- For each user, one `cha-daemon` process operates on one configuration
  directory with one vault. The key `vault` in `app.toml` selects the vault.
- systemd starts the process for a user when the first request comes
  (socket activation). The process then stays in operation.
- nginx gives the ChaWeb files to the browser, does the HTTPS encryption, and
  sends the requests to the socket of the user.
- All processes operate with one Linux account: the account that installs the
  package.

> **WARNING:** ChaWeb has no login. Each person who can connect to the port of
> a user can read and change all data of that user, and can use the model
> services at the cost of the saved keys. Make each ChaWeb port available only
> on a private network that you trust. If you add an access control in nginx,
> for example a password or client certificates, this is not a function of
> CHA, and you must make sure yourself that ChaWeb operates correctly with it.

### Requirements

- A Linux server with systemd and nginx.
- A regular user account with `sudo`. Do not do the installation as `root`.
- The package `cha-linux-<version>.tar.gz`. See
  [Appendix A](#appendix-a--make-the-packages). The package must come from a
  build computer with a compatible Linux version.
- For each user: a free port, and a TLS certificate for the host name of the
  server.

ChaWeb must be at the top of an address: `https://<host name>:<port>/`. You
cannot put it below a path such as `/chaweb/`.

### Install the package

1. Copy the package to the server and extract it:

   ```sh
   tar -xzf cha-linux-<version>.tar.gz
   cd cha-linux-<version>
   ```

2. Set the two directories and start the installer:

   ```sh
   export CHA_DEPLOY_PATH=<program directory>
   export CHA_DATA_PATH=<data directory>
   ./install.sh
   ```

   - The two paths must be full paths. They can contain only letters, digits
     and the characters `_ . / + -`.
   - `CHA_DATA_PATH` must not be in `$CHA_DEPLOY_PATH/chaweb`.
   - Use a program directory that contains only CHA. The installer makes
     your account the owner of all files in it.
   - The installer uses `sudo` for the system changes.

The installer does these tasks:

- It copies `cha-daemon`, `add_user.sh`, the nginx example and the example
  vault to `CHA_DEPLOY_PATH`.
- It replaces `$CHA_DEPLOY_PATH/chaweb` with the ChaWeb files.
- It writes the systemd units `/etc/systemd/system/cha@.service` and
  `/etc/systemd/system/cha@.socket`, if they do not exist.
- It makes a test that nginx can read the ChaWeb files, and shows a warning
  if it cannot.

The installer starts no service and does not change the nginx configuration.

**The socket group.** The socket of each user is `/run/cha/<user>.sock` with
the group `www-data`. If nginx on your server uses a different user, change
`SocketGroup` in `/etc/systemd/system/cha@.socket` to a group of that user.
Then do `sudo systemctl daemon-reload`. Do this before you add the first
user. The procedure to add a user fails if the group does not exist.

**Access to the ChaWeb files.** nginx must be able to read
`$CHA_DEPLOY_PATH/chaweb` and to go through each directory above it. If the
installer shows a warning, use a program directory that is not in a private
home directory. Do not make the directories in `CHA_DATA_PATH` readable for
nginx.

### Prepare the vault of a user

Each user has the configuration directory `$CHA_DATA_PATH/<user>/config`.
`<user>` is a name that you select for ChaWeb. It is not a Linux account. It
starts with a small letter and contains only small letters, digits, `_` and
`-`.

You have two possibilities.

**Use the example vault.** If the directory does not exist, the next procedure
creates it with the example vault. This vault has the name **Personal**, one
forum with three characters, and the provider `chatgpt` for a ChatGPT
subscription. It has no API keys.

**Use a vault that you made on the desktop.** Configure the vault in the
desktop application first: providers, keys, characters, forums and voices.
Then, before you add the user:

1. Close CHA on the desktop.
2. Copy the files to the server into `$CHA_DATA_PATH/<user>/config`:
   `app.toml`, the definition file of the vault, and the database file. See
   [Backup and restore](#24-backup-and-restore).
3. In `app.toml`, set `vault` to the name of the vault. Keep the section
   `[logging]` with `file` and `level`. The process does not start without
   it. Remove the keys `mirror` and `modify`, or set them to directories of
   the server. With `mirror`, the server writes readable copies of the
   sessions of a vault without a password.
4. In the definition file, set `data` to the file name of the database.
5. If the vault uses D1 synchronization, set `machine` in `app.toml` to a
   name that no other computer uses. Read the rules in
   [Move a vault to a different computer](#move-a-vault-to-a-different-computer).

The server does not create a first configuration. The directory must contain
`app.toml`, a vault definition, and the database before the process starts.
Each other `.toml` file in the directory must be a correct vault definition.

### Add a user

1. Set the two variables again, and run:

   ```sh
   export CHA_DEPLOY_PATH=<program directory>
   export CHA_DATA_PATH=<data directory>
   "$CHA_DEPLOY_PATH/add_user.sh" <user>
   ```

   The script creates the configuration directory with the example vault, if
   the directory does not exist. It sets the permissions, and it enables the
   socket `cha@<user>.socket`.

2. Add a server block for the user to nginx. See the next section.

### Configure nginx

The file `$CHA_DEPLOY_PATH/nginx-chaweb.conf.example` is one server block.

1. Copy it into the `http` context of nginx, for example as a file in
   `/etc/nginx/conf.d/`.
2. Make one server block for each user, and change these lines:

   | Line | Change to |
   | --- | --- |
   | `listen` | The private address and the port of this user, with `ssl`. Each user has a different port. |
   | `server_name` | The host name that the browsers use. |
   | `ssl_certificate`, `ssl_certificate_key` | Add these two lines with the paths of your certificate and key. The example does not contain them. |
   | `root` | `<program directory>/chaweb`, the same for all users. |
   | `scgi_pass` | `unix:/run/cha/<user>.sock` |

3. Do not change the other lines of the block `location /api/cha/v1/`: the
   buffer settings, the limit of 256 KiB for a request, and the cache headers.
4. Test and load the configuration:

   ```sh
   sudo nginx -t
   sudo systemctl reload nginx
   ```

The `http` context must include `mime.types`, and nginx must have its standard
file `scgi_params`.

The browser of the user must find the host name, reach the port, and trust the
certificate. The microphone of ChaWeb works only with HTTPS.

### Test the installation

```sh
systemctl status cha@<user>.socket
curl -sI https://<host name>:<port>/
curl -s https://<host name>:<port>/api/cha/v1/bootstrap
```

- The socket must be active.
- The first request must give the status `200` and the content type
  `text/html`.
- The second request must give JSON text with the forums of this user. This
  request starts the process of the user.

Then open `https://<host name>:<port>/` in the browser and send a message.
With the example vault, connect a ChatGPT account first. See the next section.

### Connect a ChatGPT account on the server

Use this procedure when a provider of the vault uses the ChatGPT subscription.
The example vault does.

1. On the server, with the account that installed the package, run:

   ```sh
   "$CHA_DEPLOY_PATH/cha-daemon" --config "$CHA_DATA_PATH/<user>/config" --openai-login
   ```

2. The command shows an address and a code, and then **Waiting for
   approval...**
3. Open the address in a web browser on any device, sign in, type the code,
   and approve.
4. The command shows **Connected to ChatGPT.** and stops.

If an account is already connected, the command shows only **Connected to
ChatGPT.**

The command writes `openai-auth.json` into the configuration directory of the
user. Each user has a separate connection. You do not have to start the
process of the user again. It reads the connection with the next message.

When the Assistant of the vault already works, you can also ask it in ChaWeb
to connect the account. It shows the address and the code. Approve, and then
answer “Done”.

### The password of a protected vault

The server cannot ask for a password. For a protected vault, put the password
into the file `password` in the configuration directory of the user.

1. Create `$CHA_DATA_PATH/<user>/config/password` with the password as its
   only line.
2. Set its permissions:

   ```sh
   chmod 600 "$CHA_DATA_PATH/<user>/config/password"
   ```

The file must be a regular file with the permissions `600` exactly. The
account that installed the package must be its owner, because the process
must be able to read it. It cannot be a symbolic link, and it cannot be empty.
Its maximum size is 4,096 bytes. If not, the process does not start.

> **CAUTION:** The password is in this file as plain text. Each person who
> can read the files of the service account can open the vault.

### Change the configuration of a server vault

ChaWeb has no Settings screens. You have these possibilities:

- **The Assistant.** In ChaWeb, open the conversation with the Assistant and
  ask it. ChaWeb shows the built-in forum with the name **Welcome**. See
  [The Assistant as a configuration tool](#22-the-assistant-as-a-configuration-tool).
  The Assistant cannot save API keys or change the prompts. After the
  Assistant changes `app.toml`
  or a vault definition file, start the process again with
  `sudo systemctl restart cha@<user>.service`.
- **D1 synchronization.** If the server vault and a desktop vault synchronize,
  change the configuration on the desktop. The change, saved keys included,
  comes to the server.
- **A new copy.** Stop the process, replace the vault files with a new copy
  from the desktop, and start again.

To stop the process of a user for work on its files, stop the socket too. If
not, the next request starts the process again:

```sh
sudo systemctl stop cha@<user>.socket cha@<user>.service
# change the files, then:
sudo systemctl start cha@<user>.socket
```

### What does not work on the server

- The generation of note audio.
- Vault commands: create, select, R2 transfer, import and export.

D1 synchronization works on the server. Each time the process starts, it
makes the D1 table of the server if that table is missing.

### Operate the service

| Task | Command |
| --- | --- |
| See the state | `systemctl status cha@<user>.socket cha@<user>.service` |
| Start the process again | `sudo systemctl restart cha@<user>.service` |
| Stop | `sudo systemctl stop cha@<user>.socket cha@<user>.service` |
| Start | `sudo systemctl start cha@<user>.socket` |
| Read the start errors | `journalctl -u cha@<user>.service` |
| Read the log | `$CHA_DATA_PATH/<user>/config/logs/cha.log` |

The log path in the table is the path of the example vault. The key
`[logging]` `file` in `app.toml` sets it.

Errors that occur before the log starts, for example an error in `app.toml`
or a missing database file, are only in the journal of systemd. An error of
the password file is in the journal and in the log.

### Problems

| Symptom | Examine |
| --- | --- |
| nginx does not start or load | `sudo nginx -t`, and the error log of nginx. |
| Status `403` or `404` for `/` or `/assets/` | The `root` line, and the access of nginx to the ChaWeb files. |
| Status `502` for the API | The socket group and the group of nginx. Then the state of the two units and the journal. |
| The socket is in the state “failed” after start errors | Correct the cause. Then: `sudo systemctl reset-failed cha@<user>.socket cha@<user>.service` and `sudo systemctl start cha@<user>.socket`. |
| Status `413` | The request is larger than 256 KiB. |
| No microphone in ChaWeb | The address must use HTTPS, and voice input must be configured in the vault. |

### Update

Always install `cha-daemon` and the ChaWeb files from the same package.

1. Extract the new package and run `install.sh` with the same two paths.
2. Start the processes again:

   ```sh
   sudo systemctl try-restart 'cha@*.service'
   ```

3. Tell the users to load the page again in the browser.

The installer replaces the program, the scripts, the example vault and the
ChaWeb files. It keeps the systemd units, the nginx configuration and the
vaults of the users. Because it keeps the units, a change of the two paths
has no effect on them. If you change a path, correct `ExecStart` in
`/etc/systemd/system/cha@.service` yourself. Compare the new `nginx-chaweb.conf.example` with your
server blocks, and copy the changes that you need.

### Remove

The package has no command to remove the installation. Do the installation
steps in the opposite order:

1. For each user: `sudo systemctl disable --now cha@<user>.socket` and
   `sudo systemctl stop cha@<user>.service`.
2. Remove the server blocks from nginx and load its configuration again.
3. Delete `/etc/systemd/system/cha@.service` and
   `/etc/systemd/system/cha@.socket`. Then do `sudo systemctl daemon-reload`.
4. Delete the program directory.
5. Delete the data directory, if you do not need the vaults.

---

# Part 3 — Reference

## 26. Files in the configuration directory

`<db>` is the database file of a vault, for example `default.sqlite3`.

| Item | Content |
| --- | --- |
| `app.toml` | The application settings. CHA writes this file again when you select a different vault. Comments in the file are then lost. |
| `default.toml`, `vault-<N>.toml` | Vault definitions. Each `.toml` file in this directory, other than `app.toml`, is a vault definition. |
| `<db>` | The database of a vault. |
| `<db>-wal`, `<db>-shm` | Work files of the database. They belong to the database. |
| `<db>.cha-lock`, `<db>.cha-main-lock`, `<db>.cha-sessions/` | Lock files. CHA uses them to coordinate processes. You can ignore them. |
| `<db>.bac`, `<definition file>.bac` | The vault that the last **Download** replaced. |
| `mirror/<vault name>/` | Readable copies of the sessions. |
| `modify/<vault name>/` | The configuration files of **Export** and **Import**. |
| `logs/cha.log`, `logs/cha.1.log` … | The log. |
| `openai-auth.json` | The ChatGPT connection. A secret. |
| `openai-auth.json.cha-lock` | A lock file. |
| `view.json` | The state of the View menu and the font size of notes. |
| `password` | ChaWeb only: the password of a protected vault. |
| `api-keys.json` | Obsolete. See [Appendix B](#appendix-b--obsolete-settings). |

CHA makes its directories and its database files readable only for your user
account.

Not in the configuration directory:

- The Welcome conversation. CHA keeps it in a temporary directory of the
  system and removes it when it closes.
- On Windows, the data of the WebView2 component: `%LOCALAPPDATA%\CHA\webview`.

## 27. app.toml

CHA reads `app.toml` only when it starts. After a change, start CHA again. The
desktop application and `cha-daemon` use the same keys.

| Key | Necessary | Value |
| --- | --- | --- |
| `vault` | Yes | The name of the vault that CHA opens at the start. It must be the name of an existing vault. CHA changes this key when you select a vault. |
| `machine` | No | The name of this computer for [D1 synchronization](#20-d1-synchronization). Not empty, no `/`. Without it, CHA does not synchronize. |
| `mirror` | No | The base directory of the [session mirror](#the-session-mirror). Without it, CHA writes no mirror. |
| `modify` | No | The base directory for [Export and Import](#21-configuration-files-export-edit-import). Without it, these commands are disabled. |
| `[logging]` `file` | Yes | The path of the log file. |
| `[logging]` `level` | Yes | `trace`, `debug`, `info`, `warn`, `error`, `critical` or `off`. |

Rules:

- A path that is not a full path starts at the configuration directory.
- `mirror` and `modify` are base directories. CHA adds the vault name.
- The directory `<modify>/<vault name>` of a vault cannot be the
  configuration directory or contain it, and it cannot contain a vault
  database.
- A missing necessary key, a value of the wrong type, or a syntax error stops
  the start. The key `machine` is the exception: CHA ignores an incorrect
  value and writes a warning to the log.
- An unknown key does not stop the start. CHA ignores it and writes a warning
  to the log.

Example:

```toml
vault = "<vault name>"
machine = "<machine name>"

mirror = "mirror"
modify = "modify"

[logging]
file = "logs/cha.log"
level = "info"
```

## 28. Vault definition files

Each `.toml` file in the configuration directory, other than `app.toml`, is
the definition of one vault. The name of the file has no function. CHA gives
the files that it makes the names `vault-<N>.toml`.

| Key | Necessary | Value |
| --- | --- | --- |
| `vault_name` | Yes | The name of the vault. |
| `data` | Yes | The path of the database file. A path that is not a full path starts at the configuration directory. |
| `protected` | No | `true` when the database has a password. The default is `false`. |
| `r2_etag` | No | The version of the last R2 transfer. CHA writes it. Do not change it. |
| `parent` | No | The name of the vault that this vault is a copy of. CHA writes it. It has no function. |

Rules:

- Two vaults cannot have the same name. Capital letters do not make a name
  different.
- Two vaults cannot have a database file with the same file name, also when
  the files are in different directories.
- The configuration directory must contain a minimum of one vault definition.
- An unknown key gives a warning in the log.

Example:

```toml
vault_name = "<vault name>"
data = "<file name>.sqlite3"
protected = false
```

Messages for a database that CHA cannot open:

| Message | Cause |
| --- | --- |
| `Password required to open this vault` | The vault is protected. |
| `The vault password is incorrect, or its database is damaged` | A wrong password, or a damaged file. |
| `… is marked as protected, but its database is not encrypted` | `protected = true` is in the definition of a database without a password. |
| `… does not exist` | The `data` path is wrong. |
| `… is not a valid CHA database` | The file is damaged, or it is not a CHA database. |
| `… has an unsupported schema` | A different CHA version made the file. |

## 29. Configuration file layout and keys

This chapter describes the files that **Export** writes and **Import** reads.
See [Configuration files](#21-configuration-files-export-edit-import) for the
procedure.

### Directory layout

```
characters/
  character-voice.md              instructions for all characters
  <character-id>/
    character.toml                necessary
    CHARACTER.md                  necessary: the frame of the character text
    PROFILE.md                    the profile
    *.md, *.txt                   more text files
    nicknames.txt                 nicknames
    _<number>_<number>.md         variant files
    <picture files>
forums/
  forum-definition.md             optional text for all forums
  <forum-id>/
    config.toml                   necessary
    FORUM.md                      necessary: the forum instructions
    *.md                          more text files
    members/
      character_defaults.toml     variables for all members
      <character-id>/
        character.toml            makes the character a member
        CHARACTER.md              optional text for this member in this forum
personas/
  <persona-id>/
    persona.toml                  necessary
    PERSONA.md                    the profile
system/
  assistant/character.toml        the settings of the Assistant
  providers/<provider-id>/config.toml
  styles/<style-id>/config.toml
  voices/<voice-id>/config.toml
  keys/config.toml
  keys/api_key_<N>/config.toml    saved keys, as plain text
  session/config.toml             session naming
  jev/config.toml                 recipient detection
  notes/config.toml               notes settings
  prompts/<prompt-id>.md          the prompts
  web-search/config.toml          Search API
  voice-input/config.toml
  voice-output/config.toml
```

### IDs

- The ID of an item is the name of its directory. No key in a file sets it.
- References between files use IDs: `provider`, `style`, `voice`,
  `default_persona`, `default_character`, and the names of the member
  directories. In IDs, capital letters are important.
- The directories of characters and personas can be in subdirectories, for
  example `characters/<group>/<character-id>/`. The ID is the name of the last
  directory. Forums, providers, styles and voices cannot be in
  subdirectories.

| Item | Permitted characters in the ID |
| --- | --- |
| Character | Letters, digits, `_`, `-` |
| Persona | Letters, digits, `_`. The first character is not a digit. |
| Forum | Letters, digits, `-`, `.`, `_`, `~` |
| Provider, style, voice | A valid directory name |
| Key | `api_key_<N>`, where N is a number |

The IDs `builtin-guest`, `builtin-assistant`, `builtin-entrance` and
`builtin-welcome` are reserved. Do not use IDs that start with `builtin-`.

### General rules for keys

- An unknown key gives a warning in the log. It does not stop the import.
- A value of the wrong type, or a value that is not permitted, is an error.
  For a character, a persona, a forum, a voice or a saved key, the error
  stops the import.
- An empty text is not permitted for `display_name` and `description`. Remove
  the key `description` if you want no description.
- An error in a provider or a style removes only that item. An item that uses
  it then gives an error.
- Some incorrect values give only a warning, and CHA ignores them: an
  incorrect `[library]` table, a `web_search` setting of a character whose
  provider cannot search, and a `reasoning_effort` of a provider that is not
  permitted.
- An error in one of the service files below `system/` (`jev`, `session`,
  `notes`, `web-search`, `voice-input`, `voice-output`) turns that function
  off and gives a warning.

### `character.toml`

Used in `characters/…/<character-id>/` and in `system/assistant/`.

| Key | Type | Value | Default |
| --- | --- | --- | --- |
| `display_name` | Text | The name. Necessary. | — |
| `description` | Text | One line for the lists. | None |
| `provider` | Text | A provider ID. Necessary for the Assistant and for each forum member. | None |
| `reasoning_effort` | Text | `none`, `low`, `medium`, `high`, `xhigh` | The value of the provider |
| `web_search` | Text | Provider web search: `off`, `auto` | The value of the provider |
| `web_search_tool` | Yes/no | On-demand web search: `true`, `false` | The Search API setting |
| `style` | Text | A style ID. | None |
| `voice` | Text | A voice ID. | The default voice |
| `tags` | List of texts | Labels. The Assistant sees them in its list of characters. | Empty |
| `[prompt]` | Table | Variables for the texts. See [Variables](#variables). | Empty |

Example:

```toml
display_name = "<name>"
description = "<one line>"
provider = "<provider-id>"
style = "<style-id>"

[prompt]
city = "<value>"
```

### `persona.toml`

| Key | Type | Value |
| --- | --- | --- |
| `display_name` | Text | The name. Necessary. |
| `description` | Text | One line for the lists. This key has no screen. |
| `style` | Text | A style ID. |
| `[prompt]` | Table | Variables for `PERSONA.md`. |

### `forums/<forum-id>/config.toml`

| Key | Type | Value | Default |
| --- | --- | --- | --- |
| `display_name` | Text | The name. Necessary. | — |
| `description` | Text | One line for the lists. This key has no screen. | None |
| `default_persona` | Text | A persona ID. | Guest |
| `default_character` | Text | The ID of the member that is the first target of new sessions. CHA writes it when a user selects a target. | The member whose ID is first in alphabetical order |
| `[library]` | Table | The Library: `api_key` (a key ID), `stores` (a list of store IDs), `provider` (the default provider, optional). CHA also writes the names and descriptions of the stores into `[library.catalogue.…]`. Use the Library screen to change this table. | None |

### Forum members

A character is a member of a forum when the directory
`forums/<forum-id>/members/<character-id>/` exists. It usually contains a
`character.toml` that is empty or has only a comment.

Settings for members, which have no screen:

| File | Function |
| --- | --- |
| `members/character_defaults.toml` | A `[prompt]` table with variables for all members of this forum. |
| `members/<character-id>/character.toml` | A `[prompt]` table with variables for this member in this forum. CHA ignores all other keys in this file. |
| `members/<character-id>/CHARACTER.md` | Text that this character gets only in this forum. CHA adds it to the profile of the character. |

> **CAUTION:** When you remove a member on the **Members** screen, CHA deletes
> the member directory with these files.

### `system/providers/<id>/config.toml`

| Key | Type | Value | Default |
| --- | --- | --- | --- |
| `display_name` | Text | The name. | Made from the ID |
| `host` | Text | The host name. Necessary. | — |
| `port` | Number | 1 to 65535. Necessary. | — |
| `https` | Yes/no | Use HTTPS. | `false` |
| `base_path` | Text | The path before the API path. Starts with `/`, does not end with `/`. | Empty |
| `api` | Text | `responses`, `chat_completions`, `google`, `anthropic` | `responses` |
| `model` | Text | The model name. Necessary. | — |
| `mode` | Text | `net` sends requests. `test` sends nothing and returns the message text as the answer. | `test` |
| `auth` | Text | `none`, or `openai_subscription` for the ChatGPT connection. | `none` |
| `api_key` | Text | The ID of a saved key. | None |
| `stream` | Yes/no | Get the answer as a stream. | `true` |
| `reasoning_effort` | Text | `none`, `low`, `medium`, `high`, `xhigh` | `none` |
| `reasoning_format` | Text | For `chat_completions`, the field that holds the reasoning text: `auto`, `none`, `reasoning_content`, `reasoning` | `auto` |
| `temperature` | Number | 0 to 2. | Not sent |
| `max_tokens` | Number | The output limit. More than 0. | Not sent. For `anthropic`: 64000. |
| `timeout_s` | Number | The time limit of a request, in seconds. | 600 |
| `idle_timeout_s` | Number | The time limit without data, in seconds. | 60 |
| `web_search` | Text | Provider web search: `off`, `auto`. Only for `responses` and for the host `openrouter.ai`. | `off` |
| `cache_retention` | Text | Prompt cache: `off`, `short`, `long`. It has an effect only for the hosts `api.openai.com` and `openrouter.ai`. | `short` |
| `openrouter_targets` | List of texts | Only for the host `openrouter.ai`: the providers that get the requests. | Empty |

Rules:

- The defaults in this table apply when a key is not in the file. A provider
  that you create on the screen gets `mode = "net"` and `https = true`.
- **Save changes** on the provider screen always writes `mode = "net"`,
  `stream = true` and `web_search = "off"`. It keeps `temperature`,
  `max_tokens`, the time limits, `reasoning_format` and `cache_retention`,
  with two exceptions. With **OpenAI OAuth**, it removes `temperature` and
  `max_tokens` and writes `cache_retention = "off"`. With **Anthropic**, it
  removes `temperature` and writes `reasoning_format = "auto"` and
  `cache_retention = "short"`.
- Thus `web_search = "auto"` for a complete provider stays only until the
  next save on the provider screen. Use the character setting **Provider web
  search** in place of it.
- For `google` and `anthropic`, CHA ignores `reasoning_effort`,
  `reasoning_format`, `cache_retention`, `web_search` and
  `openrouter_targets`. For `anthropic`, it also ignores `temperature`.
- With `auth = "openai_subscription"`, these values are necessary:
  `host = "chatgpt.com"`, `port = 443`, `https = true`,
  `base_path = "/backend-api/codex"`, `mode = "net"`, `api = "responses"`,
  `stream = true`, `cache_retention = "off"`, and no `api_key`, `temperature`
  or `max_tokens`.

Example for a service with an API key:

```toml
display_name = "<name>"
host = "<host name>"
port = 443
https = true
api = "chat_completions"
model = "<model name>"
mode = "net"
api_key = "api_key_<N>"
```

### `system/styles/<id>/config.toml`

| Key | Values | Default |
| --- | --- | --- |
| `display_name` | The name. | Made from the ID |
| `font` | `sans`, `serif`, `mono` | `sans` |
| `style` | `normal`, `italic` | `normal` |
| `weight` | `light`, `normal`, `medium`, `semibold`, `bold` | `normal` |
| `size` | `small`, `normal`, `large` | `normal` |
| `text_color` | `normal`, `muted`, `accent` | `normal` |

### `system/voices/<id>/config.toml`

| Key | Value | Default |
| --- | --- | --- |
| `display_name` | The name. | Made from the ID |
| `description` | A text. | Empty |
| `provider` | `fishaudio`, `elevenlabs` | `fishaudio` |
| `elevenlabs_voice_id` | The voice identifier at the service. Necessary. The key has this name for the two services. | — |
| `speed` | 0.7 to 1.2 | Not set |

### `system/keys/`

`keys/config.toml` holds `next_id`, the number for the next key. It must be
larger than the number of each key directory. If not, the import fails.

Each `keys/api_key_<N>/config.toml` holds one saved secret as plain text:

| `type` | Other keys |
| --- | --- |
| `models` | `display_name`, `value` (the secret), `cloud_store` (`none`, `openai`, `google`) |
| `R2` | `display_name`, `url`, `access_key_id`, `secret_key` |
| `D1` | `display_name`, `account_id`, `database_id`, `api_token` |

A vault has a maximum of one `R2` key and one `D1` key. Import ignores a `D1`
key in the files.

### Service files

| File | Keys |
| --- | --- |
| `system/session/config.toml` | `naming_provider` (a provider ID), `naming_reasoning_effort` (default `low`) |
| `system/jev/config.toml` | `url`, `model`, `api_key` (a key ID). All three are necessary. Without this file, recipient detection is off. |
| `system/notes/config.toml` | `compression_provider` (a provider ID), `compression_reasoning_effort` (default `high`), `audio_url`, `audio_api_key` (a key ID) |
| `system/web-search/config.toml` | `tool_enabled` (default `false`), `provider` (`brave`, `tavily`; default `brave`), `api_key`, `read_provider` (`off`, `firecrawl`), `firecrawl_api_key`, `web_reader_enabled`, `web_reader_url`, `web_reader_api_key` |
| `system/voice-input/config.toml` | `provider` (`openai`, `xai`; default `openai`), `url`, `model`, `api_key`, `delay` (`low`, `medium`, `high`, `xhigh`), `prompt`, `send_phrase` (default `over to you`) |
| `system/voice-output/config.toml` | `default_voice` (the **name** of a voice, not its ID). For FishAudio: `url`, `model`, `api_key`, `output_format`, `connection` (`http`, `websocket`). For ElevenLabs: the table `[elevenlabs]` with the same five keys. |

All `api_key` values in these files are key IDs, not secrets.

For voice input, `url`, `model` and `api_key` are necessary. For voice output,
`default_voice` is necessary. Without them, the function is off.

The file `system/assistant/character.toml` is necessary. It must have
`display_name` and `provider`.

### The files of the prompts

Each file in `system/prompts/` holds the text of one prompt. See
[Prompts](#prompts).

| File | Prompt |
| --- | --- |
| `note-compression.md` | **Note compression** |
| `summary-l1.md` | **Summary L1** |
| `summary-l2.md` | **Summary L2** |
| `summary-l3-section.md` | **Summary L3 section** |
| `summary-l3-overview.md` | **Summary L3 overview** |
| `session-short-summary.md` | **Short session summary** |
| `session-long-summary.md` | **Long session summary** |
| `page-formatting.md` | **Page formatting** |
| `youtube-transcript.md` | **YouTube transcript formatting** |

Rules:

- The content is plain text. CHA does not apply the template language of
  [chapter 30](#30-prompt-files-and-the-template-language) to it.
- A missing file gives the default text of its prompt.
- A file that is not valid UTF-8 text, or that is larger than 256 KiB, gives
  the default text and a warning in the log. CHA keeps the file.
- CHA ignores a file with a different name and writes a warning to the log.
  You cannot add a prompt with a new file.

## 30. Prompt files and the template language

A prompt file is a Markdown file whose text goes to the model. This chapter
describes how CHA puts these files together. It does not tell you how to write
good texts for characters.

The template language applies only to the files of characters, forums and
personas. The prompts of **Settings → Prompts** are plain text. See
[Prompts](#prompts).

### The files

| File | Function | Where you edit it |
| --- | --- | --- |
| `characters/character-voice.md` | Instructions for all characters. | **Settings → Characters → Instructions** |
| `characters/…/CHARACTER.md` | The frame of the text of one character. | Configuration files only |
| `characters/…/PROFILE.md` | The profile of one character. | The character screen |
| Other `.md` and `.txt` files of a character | Text that a different file includes. | **Character settings → Files** |
| `forums/…/FORUM.md` | The instructions of one forum. | The forum screen |
| `forums/…/members/…/CHARACTER.md` | Text for one member in one forum. | Configuration files only |
| `personas/…/PERSONA.md` | The profile of one persona. | The persona screen |

A character that you create on the screen gets this `CHARACTER.md`:

```
$$(../character-voice.md)

<character_profile>
$$(PROFILE.md)
</character_profile>
```

Thus the text of the character is the shared instructions, then its profile.
If you write a different `CHARACTER.md` that does not include `PROFILE.md`,
the character screen shows **This character has no PROFILE.md file.** when
that file does not exist.

If you put the directory of a character into a subdirectory, the first line
does not find the shared file. Use `$${CHARACTER_VOICE}` in place of it.

A file of a character with a name of the form `_<number>_<number>.md` is a
variant file. See [Files](#files). CHA adds the text of the selected variant
files to the profile as it is. Includes and variables do not work in these
files.

### Include a file

```
$$(<relative path>)
```

CHA replaces this with the content of the file. The path starts at the
directory of the file that contains the line.

- A character file can include files below `characters/`.
- A forum file can include files in the directory of its forum.
- You cannot include a picture.
- An included file can include more files, to a depth of 16. One text can
  have a maximum of 256 includes.
- The complete text has a maximum of 1 MiB.

Two fixed names include the shared files from each location:

| Text | Includes | Available in |
| --- | --- | --- |
| `$${CHARACTER_VOICE}` | `characters/character-voice.md` | Character and forum files |
| `$${FORUM_DEFINITION}` | `forums/forum-definition.md` | `FORUM.md` and the files that it includes |

### Variables

```
$${<name>}
```

CHA replaces this with the value of the variable. A name can contain letters,
digits, `_`, `-` and `.`. An unknown variable is an error.

Variables that CHA supplies:

| Variable | Value | Available in |
| --- | --- | --- |
| `character.id`, `character.display_name` | The character that gets the text. | Character and forum files |
| `forum.id`, `forum.display_name` | The forum of the session. | Character and forum files |
| `persona.id`, `persona.display_name` | The persona. | `PERSONA.md` |

You define your own variables in `[prompt]` tables. The values can be texts,
numbers, and `true` or `false`. For a character in a forum, CHA reads the
tables in this order. A later table replaces the values of an earlier table:

1. `[prompt]` in the `character.toml` of the character.
2. `[prompt]` in `members/character_defaults.toml` of the forum.
3. `[prompt]` in `members/<character-id>/character.toml` of the forum.

For a persona, CHA reads `[prompt]` in `persona.toml`.

Because CHA makes the text of `FORUM.md` for each member separately,
`FORUM.md` can use the variables of the member. Each variable in `FORUM.md`
must have a value for each member of the forum, the Assistant included. If
not, the error `unknown variable` stops the save or the import.

Rules for `[prompt]` tables:

- They have no screen. Use the configuration files.
- Put them only in the files that this section names.
- A name with a period must be in quotation marks: `"a.b" = "<value>"`.
- A list or a table as a value is an error.

### Other rules

- To write the two characters `$$` as text before `(` or `{`, type `$$$`.
- An include or a variable must be complete on one line.
- The language has no conditions and no loops.
- An error message gives the file, the line and the column.

### Example

`forums/<forum-id>/members/character_defaults.toml`:

```toml
[prompt]
topic = "<topic of this forum>"
```

`forums/<forum-id>/FORUM.md`:

```
In this forum, $${character.display_name} talks only about $${topic}.
```

To see the result, use [See the system prompt](#see-the-system-prompt).

## 31. Limits

The User Manual lists the limits of daily use. These are the limits of
administration.

| Item | Limit |
| --- | --- |
| Name of an API key | 100 bytes |
| Value of an API key | 16,384 bytes |
| Name of a forum | 200 characters |
| Picture of a character | 8 MB |
| Speed of a voice | 0.7 to 1.2 |
| Description of an OpenAI store | 512 characters |
| Stores in a Library with an OpenAI key | 2 |
| Complete text for one character | 1 MiB |
| Includes in one text | 256, to a depth of 16 |
| Text of a prompt in **Settings → Prompts** | 256 KiB |
| **Summarize**: retries of one model request | 3, after 10, 20 and 40 seconds |
| Log | 10 MiB for each file, three older files |
| Events | The newest 1,000 |
| Provider test | 10 seconds |
| Session naming | 10 seconds |
| Recipient detection | 5 seconds |
| ChatGPT connection attempt | 15 minutes |
| D1 synchronization cycle | 30 seconds |
| ChaWeb: one request | 256 KiB |
| ChaWeb: password file | 4,096 bytes |
| Assistant: one file that it writes | 64 KiB |
| Assistant: tool calls for one answer | 24 |

## 32. Messages

| Message | Cause and action |
| --- | --- |
| **CHA cannot continue** | A start error. Read the text of the dialog. See [If CHA cannot start](#if-cha-cannot-start). |
| **Settings can be changed only in the main CHA process.** | A different CHA process opened the vault first. Close the other processes, then select the vault again or start CHA again. |
| **Close other CHA processes using this vault, then try again.** | The command needs the vault for itself. Close the other CHA windows, and stop ChaWeb if it uses the same files. |
| **This session is open in another CHA process.** | Close the session in the other process. |
| **Sign in to ChatGPT before using this provider.** | Connect a ChatGPT account. See [Connect a ChatGPT account](#connect-a-chatgpt-account). |
| **Provider test failed:** … | Read the reason. Examine the address, the model name, the API format and the key. |
| **Provider authentication or permission was rejected.** | The key is wrong, or it has no access to the model. |
| **Provider quota or billing limit exceeded.** | Examine the account at the model service. |
| **This provider is still in use.** | Select a different provider for the characters in **Used by**, for session naming and for the compression provider. |
| **This persona is still used by one or more forums.** | Select a different persona for those forums. |
| **This character is still used by one or more forums.** | Remove the character from those forums. |
| **This style is still used by one or more characters.** | Select a different style for the items in **Used by**. |
| **This voice is still in use.** | Select a different voice for the characters, and a different default voice. |
| **Select a configured persona and at least one configured character.** | No character is selected, or a member has no provider. Select a provider in its **Character settings**. |
| **Invalid persona.** / **Invalid character.** / **Invalid forum.** | A value is not permitted. Usually this is the name. See [Rules for names](#rules-for-names). |
| **Add an API key before configuring voice.** | Save a key first. See [API keys](#7-api-keys). |
| **Add a voice before configuring voice output.** | Register a voice first. |
| **Invalid voice output settings.** | A selected key does not exist, no service has a key, or the default voice does not exist. |
| **Select a compression provider in Notes settings.** | See [Notes settings](#15-notes-settings). |
| **Page reading is not configured.** … | See [Web search and page reading](#13-web-search-and-page-reading). |
| **Configure the audio service URL and API key in Notes settings.** | See [Note audio service](#note-audio-service). |
| **Could not load voices. Check the audio service in Notes settings.** | The dialog **Generate audio** did not get the list of voices. Examine the **Audio service URL** and the key. See [Note audio service](#note-audio-service). |
| **Prompt text is too large. The limit is 256 KiB.** | Make the text of the prompt shorter. See [Prompts](#prompts). |
| **That prompt was not found.** | Open **Settings → Prompts** again. |
| **Modify directory is not configured** | Add the key `modify` to `app.toml`. See [app.toml](#27-apptoml). |
| **This machine has no name. Add machine to app.toml, then save again.** | See [Set up the first computer](#set-up-the-first-computer). |
| **D1 could not be reached. Check the account ID, database ID, and API token.** | Examine the three values and the network connection. |
| **Changes are not sent to D1 yet. Try again when D1 can be reached.** | Wait until the synchronization sent all changes, then upload again. Read the log if it continues. |
| **R2 storage is not configured** | Save the R2 credentials. See [R2 storage](#19-r2-storage). |
| `Only the registered uploader machine can upload this vault to R2.` … | See [The uploading computer](#the-uploading-computer). |
| **The R2 vault changed since the check. Click Upload again to review it.** | A different upload occurred. Click **Upload** again. |
| **A vault upload is running. Try again when it finishes.** | Wait for the end of the upload. |
| **The database path already exists** | A database file with the name of the new vault is in the configuration directory. Use a different name, or move the file. |
| **This vault is already protected** | You cannot change the password of a vault. |
| **A store with this name already exists.** | Delete the old store in **Settings → Cloud stores**, or rename the folder. |
| **The saved API key was rejected.** | The key of the cloud store is wrong. |
| **Select cloud stores within the service limit.** | Select a minimum of one store, and a maximum of two for an OpenAI key. |

## 33. Setup items of the User Manual

| User Manual | Setup item | Answer in this manual |
| --- | --- | --- |
| Change the vault | How to create and manage vaults. | [Vaults](#18-vaults) |
| First steps | A forum with a minimum of one character that has a provider. | [Minimum setup](#5-minimum-setup-for-the-first-conversation) |
| Automatic recipient detection | Recipient detection must be configured. | [Recipient detection](#recipient-detection) |
| The web | Web search and page reading must be configured and enabled for the character. | [Web search and page reading](#13-web-search-and-page-reading), [Character settings](#character-settings) |
| The Library | An administrator selects the cloud stores of a forum Library. | [Select the Library of a forum](#select-the-library-of-a-forum) |
| Dictate a message | The microphone button shows only when voice input is configured. | [Configure voice input](#configure-voice-input) |
| Listen to an answer | The speaker buttons show only when voice output is configured. | [Configure voice output](#configure-voice-output) |
| Pictures | A character must have picture files. | [Pictures](#pictures) |
| Import a web page | Page reading must be configured. | [Configure the Search API](#configure-the-search-api), [Page import into Notes](#page-import-into-notes) |
| Compress a note | A compression provider must be selected. | [Compression provider](#compression-provider) |
| Audio for a note | The note audio service must be configured. | [Note audio service](#note-audio-service) |
| Summarize a note or a folder | A compression provider must be selected. | [Compression provider](#compression-provider), [Flex processing for Summarize](#flex-processing-for-summarize), [Prompts](#prompts) |
| Session summaries | A compression provider must be selected. | [Compression provider](#compression-provider) |
| Library | A cloud store API key is necessary. An administrator selects and manages the stores. | [Prepare a key](#prepare-a-key), [Manage stores](#manage-stores) |
| Upload a folder to a store | An administrator must delete a store before a new upload. | [Manage stores](#manage-stores) |
| ChaWeb | An administrator installs ChaWeb and gives the address. ChaWeb uses one vault. | [ChaWeb](#25-chaweb) |
| ChaWeb, voice mode | Voice input and voice output must be configured. The microphone needs HTTPS. | [Voices](#16-voices), [Configure nginx](#configure-nginx) |
| Concepts, Assistant | The Assistant can examine and change the configuration. | [The Assistant as a configuration tool](#22-the-assistant-as-a-configuration-tool) |

---

## Appendix A — Make the packages

The packages are made from the CHA source code. This appendix gives only the
commands. The source code contains the instructions for the necessary tools.
Run each command in the directory of the source code.

| Package | Where to run | Command | Result |
| --- | --- | --- | --- |
| macOS | A Mac with Apple Silicon | `make package-macos VERSION=<version>` | `packages/CHA-macos-<version>.tar.gz` |
| Windows | 64-bit Windows | `make package-windows VERSION=<version>` | `packages\CHA-windows-<version>-x64.zip` |
| ChaWeb | Linux | `make package-linux VERSION=<version>` | `packages/cha-linux-<version>.tar.gz` |

`<version>` can contain letters, digits, periods, `_` and `-`.

## Appendix B — Obsolete settings

Old installations can contain these items. CHA ignores them, or reads them in
the way that the table shows. Each ignored item gives a warning in the log.
For `CHA_LISTEN`, the warning shows in the terminal.

| Item | Where | What CHA does |
| --- | --- | --- |
| `[web]`, `[voice_input]`, `[text_to_speech]` | `app.toml` | Ignores them. |
| `mirror`, `modify` | A vault definition file | Ignores them. The keys are now in `app.toml`. |
| `api-keys.json` | Configuration directory | When the main process opens a vault that has no saved keys and no R2 credentials, CHA copies the keys of this file into the vault. CHA does not change the file. After the copy, you can delete it. If the file is damaged, CHA does not start with such a vault. |
| `web_search = "required"` | Provider and character files | Reads it as `auto`. |
| `reasoning_effort = "minimal"` | Provider and character files | Reads it as `low`. |
| `api_key_env` | Provider files | Reads it as the name of a saved key. It is not an environment variable. The provider screen writes `api_key` in place of it. |
| `voice` | `persona.toml` | Ignores it. |
| `default_agent` | Forum `config.toml` | Reads it as `default_character`. |
| `file_references` | Character and forum files | Ignores it. The Library replaces this function. |
| `stability`, `similarity_boost`, `style`, `use_speaker_boost` | Voice files | Ignores them. |
| `CHA_LISTEN` | Environment of `install.sh` | Ignores it. Each ChaWeb port is set in nginx. |
| `system/prompts/session-summary.md` | Configuration files and vault database | Ignores it. It keeps the file. **Short session summary** and **Long session summary** replace it. |

