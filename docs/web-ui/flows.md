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
    Personas -->|"Select persona"| PersonaDetail["Persona detail"]
    PersonaDetail -->|"Personas back row"| Personas
    Characters -->|"Select character"| Detail["Character detail"]
    Detail -->|"Characters back row"| Characters
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
    PersonaDetail -.-> Unchanged["Persona, forum, session, and default character unchanged"]
    Detail -.-> Unchanged
```

Both details are informational. Neither contains a forum list, and neither
changes chat routing or who a submission is attributed to.

## Forum and session navigation

```mermaid
flowchart TD
    Sidebar["Sidebar"] -->|"Forums"| Forums["Forums<br/>title + plain member names"]
    Forums -->|"Select forum"| Sessions["Sessions"]
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
    Config["Forum config.toml default_persona"] --> Open["Session opens with a one-persona roster"]
    Open --> Descriptor["SessionDescriptor carries ID and display name"]
    Descriptor --> Status["Chat status: From"]
    Submit["Submitted message carries text only"] --> Resolve["Session resolves its own forum persona"]
    Resolve --> Session["Session records author"]
```

Attribution is settled when the session opens, not per submission: the roster it
captured holds one persona, so every message in that session has the same
author. Moving to a forum with a different `default_persona` changes the author
by opening a session there. The browser supplies neither the ID nor the display
name that is persisted with the message, and browsing the persona catalog
changes nothing: the chat status line is the only place the browser reports who
the active conversation speaks as.

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
    Idle["Generation inactive"] --> Send["Send draft; the forum persona authors it"]
    Active["Generation active"] --> Stop["Same button becomes Stop"]
    Stop --> Stopping["Request stop; wait for authoritative inactive state"]
```

The target chooser replaces the unsupported attachment action in the original
mockup. Send and Stop share one composer location; they are never shown at the
same time.
