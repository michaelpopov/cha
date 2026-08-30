# Users and shared forum sessions

Status: proposed implementation plan.

## 1. Goal

Add a small, static set of application users to CHA. A user logs in, sees the
workspace content available to that user, and may participate in any session
belonging to a forum of which the user is a member.

Users do not own sessions. There are no invitations or per-session access
lists. A session belongs to its forum, and every member of that forum has the
same rights over every stored or active session in it.

Active sessions become collaborative. Several logged-in forum members may
connect to the same `LiveSession`, observe the same transcript and generation,
and submit messages attributed to independently selected personas.

The design is deliberately small. CHA is expected to have roughly three or
four users, a handful of forums, and a small number of simultaneous browser
connections. The implementation should remain readable without a general
identity framework, a policy engine, or distributed infrastructure.

## 2. Decisions and assumptions

This plan makes the following decisions so implementation can proceed without
building unused flexibility:

- Users are defined statically under `workspace/users/`.
- User IDs are stable, URL-safe directory names.
- Passwords are supplied through environment variables named by user config;
  plaintext passwords are not committed to workspace files.
- Login sessions are opaque, random, in-memory server records carried by an
  HTTP-only cookie. Restarting `chaweb` logs everyone out.
- Forum config contains an explicit list of user IDs.
- Every forum member has equal rights. There are no owner, moderator, viewer,
  or administrator roles in the session model.
- A nonmember cannot discover or access a forum or any session in it.
- The built-in Entrance forum and Welcome session are available to every
  configured user. Welcome is one shared session, like every other session.
- Every forum member may select any workspace persona. Personas are not
  assigned exclusively to users.
- Persona selection is browser-local and is sent with each submitted message.
  It is not durable server state.
- Transcript attribution remains persona-based. The authenticated user is not
  added to persisted transcript entries in the first implementation.
- The current default character remains session-wide. Making the target
  character browser-local is a possible later refinement, not part of this
  plan.
- Existing application-wide configuration operations remain available to any
  authenticated user. Forum membership governs forum and session access, not
  operator roles.

If any of these product decisions changes, it should be changed here before
implementation. In particular, private Welcome sessions, exclusive persona
claims, durable per-user persona choices, or unequal rights would materially
change the design.

## 3. Non-goals

The first user implementation does not include:

- registration, deletion, or password changes through the browser;
- email addresses, password recovery, MFA, OAuth, or external identity
  providers;
- a users database or durable login-session database;
- session owners, invitations, sharing tokens, or session-level memberships;
- forum roles or command-specific permission matrices;
- user profiles beyond an ID and display name;
- presence lists, typing indicators, read receipts, or chat mentions of users;
- exclusive reservation of a persona;
- horizontally scaled servers or cross-process event delivery;
- an audit log connecting every transcript entry to an account;
- protection suitable for exposing a plaintext-HTTP listener directly to the
  public internet.

These omissions are intentional. They are not extension points that need
interfaces or placeholder tables now.

## 4. Target model

The access relationship is simple:

```text
Authenticated user
        |
        | member of
        v
      Forum --------------------> Stored sessions
        |                              |
        | contains                     | opened once per FullSessionId
        v                              v
   Characters                    LiveSession actor
                                       |
                                       | exclusively owns
                                       v
                               SessionController
```

The authorization question for a session is always:

```text
Is authenticated_user.id listed in session.forum_id?
```

The user is the principal accessing a resource; the user is not part of the
resource identity. `FullSessionId` therefore stays unchanged:

```cpp
struct FullSessionId {
    ForumId forum_id;
    SessionId session_id;
};
```

All members resolving the same `FullSessionId` reach the same entry in
`LiveSessionManager` and therefore the same actor, controller, transcript,
generation, journal, and session lease.

## 5. Workspace layout

### 5.1 User definitions

Add one directory per user:

```text
workspace/
  users/
    alice/
      user.toml
    bob/
      user.toml
```

Each `user.toml` contains only public presentation data and the name of the
environment variable carrying the password:

```toml
display_name = "Alice"
password_env = "CHA_USER_ALICE_PASSWORD"
```

The workspace `.env`, or the environment used to start the process, supplies
the value:

```dotenv
CHA_USER_ALICE_PASSWORD=choose-a-long-password
```

This deliberately follows the existing provider-secret pattern. It avoids a
password database, password-hash format, password-generation command, and
credential migration code. It is appropriate for a few trusted users on a
personal installation.

The limitation must be documented clearly: `.env` contains plaintext secrets
and must remain private. It is already ignored by Git. Changing `.env` requires
a server restart because CHA loads it only during startup.

### 5.2 Forum membership

Add a `users` array to each configured forum's `config.toml`:

```toml
display_name = "The Stoics Forum"
default_character = "seneca"
default_persona = "michael"
users = ["alice", "bob"]
```

The name `users` avoids confusion with the existing `members/` directory,
whose members are characters and may carry character-specific prompt
overrides.

Configured forums must list at least one user. The built-in Entrance forum is
constructed with every loaded user as a member; it has no editable config file.

### 5.3 Workspace values

Add a shared identifier alongside the existing chat IDs:

```cpp
using UserId = std::string;
```

Add a workspace-owned value:

```cpp
struct WorkspaceUser {
    UserId id;
    std::string display_name;
    std::string password_env;
};
```

Extend `WorkspaceForum` with a plain list because forum users have no
forum-specific settings:

```cpp
std::vector<UserId> user_ids;
```

Extend `Workspace` with:

```cpp
std::span<const WorkspaceUser> users() const noexcept;
const WorkspaceUser* find_user(std::string_view id) const noexcept;
bool forum_has_user(
    std::string_view forum_id,
    std::string_view user_id) const noexcept;
```

An additional `WorkspaceForumUser` class would carry no information and should
not be introduced.

### 5.4 Workspace validation

`Workspace::load()` should load users before forums and enforce:

- `workspace/users/` exists and contains at least one direct user directory;
- every user directory name is a valid URL-safe identifier;
- `user.toml` is a regular file and contains only `display_name` and
  `password_env`;
- both fields are nonempty strings;
- display names satisfy the existing public-name rules;
- user IDs and case-folded display names are unique;
- every configured forum has a nonempty `users` array;
- every listed forum user exists;
- duplicate user IDs within a forum are rejected;
- the configured password environment variable is present and nonempty before
  the server begins listening.

As with the rest of the workspace, a bad user or forum reference rejects the
candidate as a whole. `loadws()` must never publish a partially valid user
roster.

User IDs and password environment-variable names must not be included in the
workspace inventory sent to model providers. Forum user membership is an
application authorization concern, not model context.

## 6. Authentication

### 6.1 Small authentication service

Add a concrete web-owned class, for example `Authentication`, under `src/web/`.
It needs only:

- password verification against the current `WorkspaceUser`;
- creation of a login token;
- lookup of a token from a request cookie;
- deletion of a token on logout;
- a mutex-protected in-memory map from token to `UserId`.

It does not need an interface, plugin mechanism, repository, SQLite schema, or
background cleanup thread.

Generate tokens from 32 random bytes using OpenSSL `RAND_bytes`, which is
already an application dependency. Encode the token as lowercase hexadecimal
or base64url. Never derive it from a user ID, password, clock, or process ID.

Password comparison should be constant-time. Neither passwords nor login
tokens may be logged. A failed login returns one generic message regardless of
whether the user ID or password was wrong.

### 6.2 Cookie

On successful login, return a browser-session cookie similar to:

```text
Set-Cookie: cha_session=<opaque-token>; Path=/; HttpOnly; SameSite=Strict
```

Do not set `Max-Age`; closing the browser removes its copy. The server record
also disappears on logout or process restart. Multiple browsers may hold
independent tokens for the same user.

The current server is plain HTTP. `Secure` cannot be required until CHA itself
is served over HTTPS. Documentation should say that a non-loopback deployment
belongs on a trusted network or behind a TLS reverse proxy. Building TLS,
certificate management, and public-internet hardening into this change would
be out of scope.

### 6.3 Authentication routes

Add:

```text
POST /api/v1/auth/login
POST /api/v1/auth/logout
GET  /api/v1/me
```

Login accepts:

```json
{
  "user_id": "alice",
  "password": "..."
}
```

`GET /api/v1/me` returns only:

```json
{
  "id": "alice",
  "display_name": "Alice"
}
```

Logout deletes the server token when present and always expires the cookie.
It may be idempotent.

The shell, static assets, `/health`, and the login endpoint remain accessible
without a cookie so the browser can render its login screen. Application API
routes require authentication.

Add distinct protocol errors for unauthenticated and unauthorized requests.
Do not reuse `forbidden_origin`, whose meaning is specifically the existing
same-origin mutation check.

### 6.4 Request helpers

Keep route integration explicit and small. A shared helper should:

1. parse the exact `cha_session` cookie;
2. resolve it through `Authentication`;
3. verify that the current published workspace still contains the user;
4. return a small authenticated-user value or write a `401` response.

A second helper should check `Workspace::forum_has_user()` and write `404` for
a forum the user cannot access. Returning `404`, rather than distinguishing an
unknown forum from a forbidden one, keeps nonmember forum and session names
out of the API.

Do not accept a user ID from a route, query parameter, input JSON body, or SSE
event and treat it as authentication. Only the login cookie establishes the
request user.

The existing matching-Origin validation remains required for login, logout,
and every other JSON mutation. `SameSite=Strict` is useful defense in depth,
not a replacement for that check.

## 7. Authorization rules

### 7.1 Global authenticated operations

Every authenticated user may:

- read the character and persona rosters and details;
- use the existing character-settings operation;
- request a workspace reload;
- inspect their own `/api/v1/me` response.

This follows the equal-rights requirement. If application administration later
needs to differ from forum participation, that should be a separate, explicit
feature rather than an `is_admin` field added preemptively now.

### 7.2 Forum operations

Only a forum member may:

- discover that forum in bootstrap;
- read its forum detail;
- list or create its sessions;
- see its sessions in Recent;
- validate, open, or reattach to one of its sessions;
- connect to its live event stream;
- fetch a snapshot or submit a command;
- rename, download, or delete a session.

Every one of these checks happens before consulting `SessionRepository` or
`LiveSessionManager`. In particular, the current disk-free `try_reattach()` and
`lookup()` paths must not bypass authorization merely because another member
already opened the actor.

`SessionRepository` should remain unaware of users. It is an internal storage
component, and the web boundary authorizes access before calling it. Adding a
user parameter to every catalog and journal operation would mix access policy
into otherwise reusable storage without providing another protection boundary.

### 7.3 Bootstrap filtering

`GET /api/v1/bootstrap` becomes user-specific:

- include all shared characters and personas;
- include only forums containing the authenticated user;
- include only Recent sessions from those forums;
- choose the initial forum/session from the accessible set;
- include the authenticated user's public ID and display name, either directly
  or through the already-fetched `/api/v1/me` result.

If Entrance remains the initial forum, it is always available because its
member list is every workspace user.

## 8. Session identity and persistence

No session-storage schema change is required merely to add users:

- sessions remain in `forums/<forum>/sessions/`;
- `SessionDatabaseMetadata` continues to contain session ID, forum ID, and
  label;
- `FullSessionId` remains forum ID plus session ID;
- `SessionRepository`, `SessionCatalog`, leases, and backups retain their
  current layout;
- creating a session does not record an owner;
- removing a user from a forum does not move or rewrite any session database.

The transcript continues to persist the selected persona as
`TranscriptEntry::participant_id` and `display_name`. That is what the browser
and model context need.

For the first version, the authenticated account that submitted an entry is
not persisted. Several users may choose the same persona and therefore be
indistinguishable in an exported transcript. If account-level accountability
later becomes a real requirement, add a separate optional `user_id` column and
schema migration; never overload the persona participant ID with an account
ID.

## 9. Per-message persona attribution

The current controller-wide persona cannot survive collaboration: two browsers
need different `From` selections at the same time.

Change the input request from:

```json
{ "text": "Hello" }
```

to:

```json
{
  "persona_id": "michael",
  "text": "Hello"
}
```

The route validates that `persona_id` names a current workspace persona, then
passes it to the owner command. `LiveSession` ultimately calls the existing
author-aware controller operation:

```cpp
controller.submit_prompt(persona_id, text, handle);
```

The authenticated `UserId` should accompany the owner command for diagnostic
event logging, but `SessionController` does not need it. The session layer
continues to reason about chat participants—personas and characters—not web
accounts.

The browser initializes its persona selector from the forum's configured
`default_persona`. It keeps the selected ID in browser state and sends it with
every message. Selection may differ between browsers or tabs, even for the same
authenticated user.

Remove the session-global persona path:

- `SessionController::default_persona_id_`;
- `ControllerView::default_persona_id`;
- `SessionController::set_default_persona()`;
- the initial persona constructor arguments;
- `OpenedSession::persist_default_persona`;
- the `/!Name` server command and forum-default write it triggers.

`WorkspaceForum::default_persona_id` remains. It is static configuration and
the initial browser choice, not mutable session state. Session projection
should read it directly from `WorkspaceForum`.

The first UI should use an ordinary persona picker. Preserving `/!Name` as a
browser-local shortcut can be considered later; duplicating persona handle
resolution in TypeScript is not required for the initial implementation.

## 10. One live actor, several readers

### 10.1 Preserve the actor boundary

Do not make `SessionController`, `Transcript`, or `SessionJournal` thread-safe.
All participant commands continue through the existing bounded
`LiveSession::commands_` queue and execute on its owner thread.

This gives shared sessions a clear ordering:

- the first accepted prompt starts generation;
- another prompt received while generation is active gets the existing busy
  outcome;
- stop, clear, rename, target, and transcript operations remain serialized;
- every observer sees controller state in the same order.

Supporting simultaneous model generations or a pending prompt queue is not
needed for a few collaborative users.

### 10.2 One mailbox per SSE connection

Keep `SseMailbox` as a single-reader queue, but allocate one mailbox for each
accepted event stream:

```text
LiveSession
  connection 1 -> SseMailbox 1
  connection 2 -> SseMailbox 2
  connection 3 -> SseMailbox 3
```

Replace the single `mailbox_` member with an owner-thread-only map containing:

- a server-generated connection ID;
- the authenticated user ID for safe lifecycle logging;
- the connection's `shared_ptr<SseMailbox>`.

Each connection begins with its own full snapshot and independent append
sequence. A new connection no longer supersedes an existing one. The browser
and server can remove the `superseded` event and takeover recovery path.

On a controller update, `LiveSession` offers the update to every mailbox. A
slow mailbox may require a replacement snapshot without forcing healthy
mailboxes to abandon an applicable append. Build at most one fallback snapshot
for that update and copy it to whichever mailboxes need it.

A slow or disconnected browser must never block publication to another
browser. Final-drain waiting must use one shared absolute deadline across all
mailboxes rather than spending the complete drain duration once per
connection.

### 10.3 Connection and unload state

Replace `BrowserConnectionState`'s single active connection with a small set of
active connection IDs. Its rules become:

- accepting a connection adds it to the set;
- closing an unknown or already-closed ID is harmless;
- closing one of several connections leaves the session connected;
- closing the last connection records `disconnected_since`;
- accepting the first new connection clears `disconnected_since`;
- idle and generation-orphan deadlines begin only when the set is empty.

The actor still unloads after the existing grace period once everybody leaves.
An active generation may continue until the existing orphan limit.

`/exit` must no longer stop the shared actor. Leaving a session is a browser
navigation and stream-close operation. A future explicit "close for everyone"
action is unnecessary while any member can already delete the session or let
it unload naturally.

### 10.4 HTTP worker capacity

The current HTTP pool assumes at most one long-lived SSE request per live
session. Multiple readers invalidate that calculation.

Keep one global bound instead of multiplying several configuration knobs:

- reserve `http_request_headroom` workers for commands and ordinary requests;
- allow at most `http_thread_pool_size - http_request_headroom` concurrent SSE
  streams process-wide;
- reject an additional stream with a clear `503` error before it occupies the
  last request worker;
- release the admission slot in the stream close callback.

A tiny process-owned counter guarded by a mutex is sufficient. It does not need
a general semaphore abstraction. The existing defaults provide twelve SSE
slots, which is ample for three or four users.

## 11. Shared and private command results

Today, `LiveSession` can turn a command's `ControllerUpdate.notice` into the
session-wide notice included in every snapshot. That is confusing in a shared
session: one user's malformed command or unknown persona should not replace
every participant's notice.

Use this rule:

- validation, usage, busy, and success messages produced by a synchronous user
  command are returned only in that command's HTTP response;
- transcript mutations and generation state are broadcast;
- asynchronous provider failures, cancellation, and session lifecycle notices
  remain shared because they describe shared state.

This can be implemented by keeping command-local notice handling in
`CommandResult` and applying session presentation notices only for asynchronous
controller events. It does not require addressed notice entries or a private
SSE protocol.

Shared commands retain equal rights:

- any member may send, stop, clear, use off-record controls, multicast, change
  the current target, change session style, rename, download, or delete;
- their effects are visible to everybody;
- the UI should describe destructive actions as affecting the shared session.

The current default character and style overrides remain session-wide to keep
this plan focused. If users frequently fight over the `To` selection, the same
per-message technique used for personas can later carry a character ID.

## 12. API and protocol changes

Update `resources/cha.yaml` first, then regenerate
`webapp/src/api/schema.d.ts` through the existing browser API workflow.

Add protocol values for:

- login and logout requests;
- the current user summary;
- `authentication_required` and `forbidden` errors;
- `persona_id` on `InputRequest`;
- any SSE-capacity error selected for an admitted-but-full server.

Document cookie authentication in the OpenAPI description and security
scheme. The browser client uses same-origin `fetch`, so cookies accompany
ordinary requests automatically. Native `EventSource` also sends same-origin
cookies.

Remove protocol statements that currently promise:

- no authentication;
- one browser per live session;
- takeover and `superseded` events;
- session-wide current persona attribution;
- `/!Name` persisting the forum default.

Session snapshots remain common, non-personalized values. They should carry
the forum's configured default persona only as forum metadata. The browser's
currently selected persona belongs in local state, not in a snapshot broadcast
to every participant.

## 13. Browser changes

Add a small authentication state before bootstrap:

```text
checking login -> login form -> authenticated bootstrap -> application
```

The browser should:

- call `/api/v1/me` at startup;
- show a user ID/password form after a `401`;
- fetch bootstrap only after authentication succeeds;
- offer logout and return to the login form after logout;
- treat a later API `401` as a logged-out state rather than a generic server
  failure;
- handle an event-stream authentication failure through the ordinary snapshot
  recovery probe, whose `401` returns the app to login;
- filter nothing itself that the server has not already authorized;
- show a persona selector in the chat composer;
- initialize it from the forum default and include its ID with every input;
- remove the parked/superseded-device state because several streams may now
  coexist;
- continue using one common session snapshot and append protocol.

Presence indicators and user names beside transcript entries are deliberately
absent. The visible author remains the chosen persona.

## 14. Reload and revocation behavior

User and membership configuration changes are published through the existing
workspace reload path.

The current reload operation stops active sessions before publishing the
candidate. Preserve that coarse behavior for the first implementation. It
provides a simple revocation boundary:

1. the old event streams close;
2. the candidate workspace is validated and published;
3. reconnecting requests authenticate against the current user roster;
4. forum access is checked against the new membership list.

If a user was removed, existing login tokens naming that ID stop authenticating
because authentication verifies the user against the current `Workspace` on
every request. If only a forum membership was removed, the token remains valid
but the user can no longer discover or rejoin that forum.

Changing the password value in `.env` requires a process restart. A restart
also deletes every in-memory login token, so the new password is required at
the next login.

## 15. Logging and secret handling

Add user IDs to structural authentication and session events where useful:

```text
web auth user_id=alice event=login_succeeded
web session user_id=alice forum_id=stoics session_id=... event=sse_connected
```

Do not log:

- passwords;
- cookie headers or login tokens;
- submitted prompt bodies;
- transcript or generated content.

Login failures may log the attempted valid-format user ID and source address,
but a generic `login_failed` event without those fields is simpler and avoids
accidental disclosure. No rate limiter or account lockout is planned for the
trusted, small deployment. This limitation belongs in deployment
documentation.

## 16. Implementation stages

Each stage should compile and keep its focused tests passing before the next
stage begins.

### Stage 1: workspace users

1. Add `UserId` and `WorkspaceUser`.
2. Load and validate `workspace/users/*/user.toml`.
3. Add user indexes and lookup methods to `Workspace`.
4. Add forum `users` parsing and membership validation.
5. Populate Entrance membership from the loaded users.
6. Extend `TestWorkspace` and packaged/sample workspaces with users and forum
   membership.
7. Add workspace unit tests for valid and invalid user configuration.

At the end of this stage, behavior is still single-browser and unauthenticated;
the new values are only loaded and tested.

### Stage 2: login and route authentication

1. Add the small in-memory `Authentication` service.
2. Add cookie parsing, secure token generation, and constant-time password
   comparison.
3. Add login, logout, and current-user routes.
4. Require authentication for application API routes while keeping shell,
   assets, health, and login public.
5. Add the login screen and authenticated bootstrap sequence.
6. Update OpenAPI and generated client declarations.

At the end of this stage, all configured users can log in, but they still see
the current unfiltered application.

### Stage 3: forum authorization

1. Filter bootstrap forums and Recent sessions by membership.
2. Protect forum detail and every stored-session route.
3. Protect live snapshot, input, action, and event routes before manager
   lookup or reattach.
4. Make deep-link recovery report inaccessible forums as not found.
5. Add two-user route tests proving both access and isolation.

At the end of this stage, users share forum sessions according to static
membership, but a newly connected browser still takes over the one stream.

### Stage 4: participant-local personas

1. Add `persona_id` to `InputRequest` and browser state.
2. Pass the selected persona through the trusted owner command.
3. Remove controller-wide persona state and `/!Name` persistence.
4. Project the forum's configured default persona directly from `Workspace`.
5. Make synchronous command notices private to the caller.
6. Add controller, route, projection, and component tests for two different
   persona authors in one transcript.

### Stage 5: multiple live readers

1. Replace takeover bookkeeping with a connection set.
2. Allocate one `SseMailbox` per stream.
3. Broadcast snapshots and appends independently.
4. Start unload timing only after the last connection closes.
5. Bound total SSE admission while preserving HTTP request headroom.
6. Remove the `superseded` protocol and browser recovery branch.
7. Add multi-reader unit, process, stress, and browser tests.

At the end of this stage, the requested collaborative behavior is complete.

### Stage 6: documentation and cleanup

1. Update root, workspace, web, API, tutorial, design, packaging, and browser
   documentation.
2. Remove obsolete persona persistence, one-reader comments, and tests.
3. Add setup instructions for creating users and password environment values.
4. Run native, browser component, browser end-to-end, and package checks.

## 17. Expected file areas

| Area | Expected work |
| --- | --- |
| `CMakeLists.txt` | Add authentication sources and link `cha_web` to OpenSSL Crypto for random tokens and constant-time comparison. |
| `src/chat/ids.h` | Add `UserId`. |
| `src/workspace/workspace.*` | Load users, store/index them, parse forum membership, expose lookups. |
| `src/workspace/session_open.*` | Remove initial/persisted session persona wiring. |
| `src/session/session_controller.*` | Remove controller-wide persona state while retaining explicit author resolution. |
| `src/session/controller_view.h` | Remove the current persona field. |
| `src/web/authentication.*` | In-memory login tokens, password verification, request authentication. |
| `src/web/lobby_routes.*` | Auth routes, user-specific bootstrap, forum authorization. |
| `src/web/session_routes.*` | Forum authorization, authenticated command context, multi-reader events. |
| `src/web/live_session.*` | Per-message author, connection set, mailbox fan-out, shared/private notices. |
| `src/web/sse_mailbox.*` | Retain single-reader behavior; only small copy/fan-out helpers if needed. |
| `src/web/browser_connection_state.*` | Replace takeover semantics with multiple connection bookkeeping, and likely rename. |
| `src/web/protocol.*` | User/auth DTOs, errors, persona-bearing input, removal of superseded semantics. |
| `src/web/http_server.*` | Global SSE admission bound and authentication-related fallback handling. |
| `src/web_main.cpp` | Own and inject `Authentication` and SSE admission state. |
| `resources/cha.yaml` | Cookie security scheme, auth routes, changed input and event contracts. |
| `webapp/src/api/*` | Login/current-user calls, regenerated types, changed event handling. |
| `webapp/src/state/*` | Authentication state and local persona selection. |
| `webapp/src/components/*` | Login form, logout action, persona picker, shared-session wording. |
| `tests/support/test_workspace.*` | Default test users, forum membership helpers, password environment setup. |
| Native/web/browser tests | Authentication, authorization, persona attribution, multi-reader behavior. |
| Sample and packaged workspaces | User definitions, forum memberships, setup documentation. |

This table is a navigation aid, not a requirement to touch every file. Prefer
the smallest edit that establishes each stage's behavior.

## 18. Test plan

### 18.1 Workspace tests

Cover:

- loading several users;
- lookup by ID;
- forum membership lookup;
- Entrance containing every user;
- missing `users/`, empty user set, missing `user.toml`, unknown fields, invalid
  IDs, duplicate names, empty password variable, missing password environment
  value, empty forum membership, duplicate members, and unknown members;
- failed reload preserving the previous published workspace.

### 18.2 Authentication tests

Cover:

- correct and incorrect passwords;
- unknown user producing the same public failure as a wrong password;
- random distinct tokens across logins;
- token lookup and logout;
- missing, malformed, and unknown cookies;
- removed workspace user invalidating an otherwise live token;
- no password or token appearing in JSON or logs;
- Origin rejection on login and logout mutations.

### 18.3 Authorization tests

With Alice in Forum A and Bob in Forum B, cover every relevant route category:

- bootstrap and Recent filtering;
- forum details;
- session list, create, open, reattach, rename, download, and delete;
- live snapshot, SSE, input, stop, and default-character actions;
- authorization occurring before live-manager lookup and storage inspection;
- malformed identifiers still following the established route-not-found
  behavior;
- both users accessing the same session when both belong to the forum.

### 18.4 Persona tests

Cover:

- two forum members submitting with different persona IDs;
- correct persona ID and display name in durable transcript entries;
- model context retaining both persona display names;
- unknown persona rejection without transcript mutation;
- changing one browser's persona not changing another browser or forum config;
- forum default used only to initialize browser state;
- same persona selected by two users remaining legal.

### 18.5 Live collaboration tests

Cover:

- two and three event streams receiving the same snapshot;
- both streams receiving transcript and generation updates;
- independent append sequence numbers;
- a slow reader collapsing or replacing only its own pending payload;
- a new connection not closing an existing stream;
- one disconnect not starting unload while another remains;
- last disconnect starting the existing idle/orphan deadline;
- stale and duplicate close notifications being harmless;
- final shutdown closing every mailbox under one bounded deadline;
- SSE admission preserving command-request headroom;
- concurrent prompts remaining serialized and yielding one accepted generation.

### 18.6 Browser end-to-end scenario

Use two isolated browser contexts so cookies represent two actual users:

1. Alice and Bob log in independently.
2. Both open the same forum session.
3. Alice selects one persona and Bob selects another.
4. Alice submits; both browsers observe the prompt and response.
5. Bob submits; both browsers observe the second persona attribution.
6. Alice navigates away; Bob's stream remains live.
7. Alice reconnects and receives the current snapshot without displacing Bob.
8. A nonmember login cannot discover or open the forum.

Keep the existing deterministic provider and short timing bounds. No external
network or production credential should be needed.

## 19. Migration and rollout

This is an intentional configuration migration rather than a permanent
anonymous compatibility mode.

Before running a user-enabled build:

1. create at least one `workspace/users/<id>/user.toml`;
2. add its password variable to the process environment or workspace `.env`;
3. add a `users` array to every configured forum;
4. update packaged/sample workspaces and test fixtures;
5. restart `chaweb`.

Do not silently manufacture an anonymous or default-password user when the
configuration is absent. A clear startup error is safer and simpler than
maintaining authenticated and anonymous modes indefinitely.

Stored session databases need no migration. Their forum IDs continue to
identify the authorization boundary.

## 20. Completion criteria

The user feature is complete when:

- a server cannot start with invalid user or forum membership configuration;
- each configured user can log in and log out with an in-memory cookie session;
- unauthenticated API access is rejected without blocking the login shell;
- users discover and access only member forums;
- all members of a forum have equal access to all its sessions;
- two users can keep simultaneous event streams to one live session;
- either user can submit using an independently selected persona;
- both users receive the same ordered transcript and generation state;
- leaving one browser does not stop the shared actor;
- no session owner, invitation, role, user database, or per-session ACL has
  been introduced;
- native tests, browser tests, end-to-end tests, sanitizer builds, and package
  checks pass;
- setup and deployment documentation states the password and plaintext-HTTP
  limitations plainly.

## 21. Deferred choices

Only revisit these after real use demonstrates a need:

- storing password hashes instead of environment-backed passwords;
- expiring or persisting login sessions;
- private rather than shared Welcome sessions;
- durable per-user persona preferences;
- restricting persona choices per forum or per user;
- showing authenticated user identity in transcript or audit data;
- presence, typing, and reconnect cursors;
- participant-local target characters and styles;
- owner/moderator/viewer roles;
- TLS inside `chaweb` rather than at a reverse proxy.

Deferring these is part of keeping the implementation manageable, not an
oversight.
