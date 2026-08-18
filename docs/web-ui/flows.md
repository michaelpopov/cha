# Navigation flows

## Main navigation

Sidebar navigation changes the Main area but preserves the sidebar's current open/closed state. The two-line button is the only sidebar visibility control.

```mermaid
flowchart TD
    Main["Any Main-area state"] -->|"Two-line button"| Toggle["Toggle sidebar; keep Main state"]
    Sidebar["Sidebar"] -->|"Personas"| Personas["Personas"]
    Sidebar -->|"Characters"| Characters["Characters"]
    Sidebar -->|"Forums"| Forums["Forums"]
    Sidebar -->|"Recent session"| Open["Open or reattach selected session"]
    Sidebar -->|"Recent actions / right-click"| Maintain["Rename or confirm delete"]
    Sidebar -->|"Reload workspace"| Reload["Publish a new workspace generation;<br/>restart live sessions"]
    Personas -->|"Select persona"| PersonaDetail["Persona detail"]
    PersonaDetail -->|"Personas back row"| Personas
    Characters -->|"Select character"| Detail["Character detail"]
    Detail -->|"Characters back row"| Characters
    Detail -->|"Writable chevron"| Settings["Character settings"]
    Settings -->|"Back or Cancel"| Detail
    Forums -->|"Select forum"| Sessions["Sessions"]
    Open --> Chat["Chat"]
```

## Persona and character browsing

Both rosters browse the same way: a workspace-wide list whose rows open one
read-only Markdown description.

```mermaid
flowchart LR
    Sidebar["Sidebar"] -->|"Personas"| Personas["Workspace persona list<br/>Guest + configured, name + description"]
    Personas -->|"Select row"| PersonaDetail["Persona detail<br/>render PERSONA.md"]
    PersonaDetail -->|"Back"| Personas
    Sidebar -->|"Characters"| List["Workspace character list<br/>name + short description"]
    List -->|"Select row"| Detail["Character detail<br/>render CHARACTER.md"]
    Detail -->|"Back"| List
    Detail -->|"Writable chevron"| Settings["Character settings<br/>provider and style"]
    Settings -->|"Back or Cancel"| Detail
    PersonaDetail -.-> Unchanged["Persona, forum, session, and default character unchanged"]
    Detail -.-> Unchanged
```

Persona detail is informational. Character detail is too, except for the
chevron into Settings. Neither contains a forum list, and neither changes chat
routing or who a submission is attributed to.

## Character settings

```mermaid
flowchart TD
    Detail["Character detail"] -->|"Top-right chevron"| Form["Settings: provider and style"]
    Form -->|"Cancel or back"| Detail
    Form -->|"Save"| Patch["PATCH character"]
    Patch -->|"Write succeeds"| Reload["Affected live sessions shut down as reloading"]
    Reload --> Ladder["Existing stream recovery reopens them"]
    Ladder --> Form
```

Save stays on Settings. Recovery reports `session-snapshot`, so it does not
force Chat. A provider-only save skips forums that override the provider.

## Forum and session navigation

```mermaid
flowchart TD
    Sidebar["Sidebar"] -->|"Forums"| Forums["Forums<br/>title + description or member names"]
    Forums -->|"Select forum"| Sessions["Sessions"]
    Sessions -->|"Select forum header"| ForumDetail["Forum detail<br/>FORUM.md as Markdown"]
    ForumDetail -->|"Back"| Sessions
    Sessions -->|"Select existing session"| OpenExisting["Open or reattach session"]
    OpenExisting --> Chat["Chat"]
    Sessions -->|"New session"| Name["New session: required name field"]
    Name -->|"Cancel"| Sessions
    Name -->|"Start session"| Create["Create stored session"]
    Create --> OpenNew["Open new session and add it to Recent"]
    OpenNew --> Chat
```

Stored-session rows contain a name and compact time metadata only. They do not contain descriptions.
Recent rows additionally expose Rename and Delete. Delete returns an active
conversation to Welcome after its runtime has stopped. Copy is a Chat top-bar
icon action paired with the sidebar control and never calls the server.

## Persona attribution

```mermaid
flowchart LR
    Sidebar["Sidebar"] -->|"Personas"| Personas["Personas: workspace catalog, read-only"]
    Personas -.-> Nothing["Selects nothing; changes no attribution"]
    Config["Forum config.toml default_persona"] --> Open["Session opens on that persona"]
    Open --> Current["Controller holds the current persona"]
    Command["Chat command /!Name"] --> Current
    Command --> Reload["Forum live sessions shut down as reloading"]
    Reload --> Open
    Current --> Status["Chat status: From"]
    Current --> Save["Saved back to forum config.toml"]
    Submit["Submitted message carries text only"] --> Resolve["Session resolves its current persona"]
    Resolve --> Session["Session records author"]
```

Attribution is server-side, never per submission: a session opens on its forum's
`default_persona` and keeps that author until `/!Name` selects another, which
also saves the choice for the next session in that forum. The browser supplies
neither the ID nor the display name that is persisted with the message, and
browsing the persona catalog changes nothing: the chat status line is the only
place the browser reports who the active conversation speaks as.

## Chat routing status

```mermaid
flowchart LR
    Forum["Active forum"] --> Status["Forum · From: Persona · To: Default character"]
    Persona["Forum persona from the session snapshot"] --> Status
    Default["Live session default character"] --> Status
    Snapshot["Live session snapshot or event"] --> Default
```

The status line is read-only and appears below the prompt composer. It is the only persistent display of the forum, its persona, and the target character in Chat.

## Chat controls

```mermaid
flowchart TD
    Target["Target chooser"] -->|"Select character"| Request["Request default-character change"]
    Request --> Snapshot["Snapshot or event confirms default character"]
    Snapshot --> Status["Update read-only To status"]
    Idle["Generation inactive"] --> Send["Send draft; the current persona authors it"]
    Active["Generation active"] --> Stop["Same button becomes Stop"]
    Stop --> Stopping["Request stop; wait for authoritative inactive state"]
```

The target chooser replaces the unsupported attachment action in the original
mockup. Send and Stop share one composer location; they are never shown at the
same time.
