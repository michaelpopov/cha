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
    Sidebar -->|"Settings gear"| Settings["Settings"]
    Characters -->|"Select character"| Detail["Character detail"]
    Detail -->|"Characters back row"| Characters
    Forums -->|"Select forum"| Sessions["Sessions"]
    Open --> Chat["Chat"]
```

## Character browsing

```mermaid
flowchart LR
    Sidebar["Sidebar"] -->|"Characters"| List["Workspace character list<br/>name + short description"]
    List -->|"Select row"| Detail["Character detail<br/>render CHARACTER.md"]
    Detail -->|"Back"| List
    Detail -.-> Unchanged["Persona, forum, session, and default character unchanged"]
```

Character detail is informational. It contains no forum list and does not change chat routing.

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

## Persona selection

```mermaid
flowchart LR
    Sidebar["Sidebar"] -->|"Personas"| Personas["Personas"]
    Personas -->|"Select one radio"| Current["Update current persona"]
    Current --> Next["Next submitted message carries that persona ID"]
    Next --> Resolve["Session resolves ID in application-wide roster"]
    Resolve --> Session["Session records author; roster is not forum membership"]
    Current --> Status["Update Chat status: From"]
    Current -.-> Unchanged["Forum and active session remain unchanged"]
```

Resolution happens for every submission against the effective Guest-plus-
workspace roster captured when the session opened. Guest can write in an
ordinary workspace forum because that roster is application-wide, not forum
membership. The browser never supplies the display name that is persisted with
the message.

## Chat routing status

```mermaid
flowchart LR
    Forum["Active forum"] --> Status["Forum · From: Persona · To: Default character"]
    Persona["Current persona"] --> Status
    Default["Live session default character"] --> Status
    Snapshot["Live session snapshot or event"] --> Default
```

The status line is read-only and appears below the prompt composer. It is the only persistent display of the current persona, forum, and target character in Chat.

## Chat controls

```mermaid
flowchart TD
    Target["Target chooser"] -->|"Select character"| Request["Request default-character change"]
    Request --> Snapshot["Snapshot or event confirms default character"]
    Snapshot --> Status["Update read-only To status"]
    Idle["Generation inactive"] --> Send["Send draft as selected persona"]
    Active["Generation active"] --> Stop["Same button becomes Stop"]
    Stop --> Stopping["Request stop; wait for authoritative inactive state"]
```

The target chooser replaces the unsupported attachment action in the original
mockup. Send and Stop share one composer location; they are never shown at the
same time.
