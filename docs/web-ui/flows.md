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
    Name -->|"Empty name"| Name
    Name -->|"Start session"| Create["Create stored session"]
    Create -->|"Failure"| Name
    Create -->|"Success"| OpenNew["Open new session and add it to Recent"]
    OpenNew --> Chat
```

Stored-session rows contain a name and compact time metadata only. They do not contain descriptions.

## Persona selection

```mermaid
flowchart LR
    Sidebar["Sidebar"] -->|"Personas"| Personas["Personas"]
    Personas -->|"Select one radio"| Current["Update current persona"]
    Current --> Next["Next submitted message carries that persona ID"]
    Current --> Status["Update Chat status: From"]
    Current -.-> Unchanged["Forum and active session remain unchanged"]
```

## Chat routing status

```mermaid
flowchart LR
    Forum["Current or active forum"] --> Status["Forum · From: Persona · To: Default character"]
    Persona["Current persona"] --> Status
    Default["Forum/session default character"] --> Status
    Snapshot["Live session snapshot or event"] --> Default
```

The status line is read-only and appears below the prompt composer. It is the only persistent display of the current persona, forum, and target character in Chat.

