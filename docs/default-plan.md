# Default entrance and in-chat navigation: implementation plan

Status: ready for implementation, 2026-08-04.

This plan implements the contract in
[`default-design.md`](default-design.md). The design document is the source of
truth for product behavior. This document turns it into an ordered set of
implementation blocks with concrete source changes, tests, migration work, and
completion criteria.

The blocks are deliberately sized so one block can be implemented in a fresh
GPT-5.6-terra context without loading the details of every other block. There
are seven blocks. This is the smallest practical split that keeps the two
frontends, the session-catalog concurrency work, and the application workflow
from competing for the same context window.

## 1. How to use this plan

Implement the blocks in order except that Blocks 5 and 6 may proceed in either
order after Block 4 is complete. Do not begin Block 7 until both frontend
blocks are complete.

At the start of a block:

1. Read this introduction, the block itself, and only the design sections
   listed under **Design references** for that block.
2. Inspect the named current source and test files. Treat the file lists as
   starting points rather than permission to ignore a newly discovered caller.
3. Search all callers before changing a public signature.
4. Keep the tree buildable and run the complete test suite before handing the
   work to the next block. Every block runs the same command; there are no
   per-block test filters. The whole suite is a few seconds, so a subset would
   buy nothing and would silently skip the suites a block is most likely to
   break.
5. Record any necessary deviation in the design first. Do not silently use the
   implementation plan to change the approved behavior.

Every block must preserve existing ordinary stored-session behavior unless the
design explicitly changes it. The application workflow belongs in the shared
`application/` layer; neither frontend may grow a private implementation of
persona, forum, or session navigation.

### Block dependency map

| Block | Outcome | Depends on |
| ---: | --- | --- |
| 1 | Configuration, metadata, public-name, and workspace-snapshot foundation | Current tree |
| 2 | Name-based session catalog and serialized creation | Block 1 name rules |
| 3 | Built-ins, shared persona roster, Entrance sources, and ephemeral Welcome | Blocks 1 and 2 |
| 4 | Chat application, application commands, and switch transactions | Block 3 |
| 5 | Console starts in Welcome and supports application navigation | Block 4 |
| 6 | TUI starts in Welcome and supports navigation and overlays | Block 4 |
| 7 | Obsolete-code removal, workspace/docs migration, and full verification | Blocks 5 and 6 |

### Cross-block rules

These decisions are settled and must not be revisited while implementing the
plan:

- The built-in persona is `Guest`, the built-in character is `Assistant`, the
  built-in forum is `Entrance`, and the run-scoped default session is
  `Welcome`.
- Public commands accept and print public names only. Private directory names,
  participant keys, and database stems remain internal.
- Public names compare using ASCII folding while retaining authored spelling.
- The application constructs one effective persona catalog and one shared,
  immutable `PersonaRoster` object for its entire lifetime. Every terminal
  controller receives that same object, not a copy and not a reload.
- `[provider]` in `app.toml` is the bottom configuration layer for Assistant,
  all workspace characters, and the web frontend's workspace characters.
- The existing top-level `host` and `port` in `app.toml` remain the web
  server's bind address. They are not the provider endpoint and must not be
  conflated with `[provider].host` and `[provider].port`.
- `Welcome` uses the normal SQLite schema, journal, restore path, and
  `SessionLease`, but its database is private to one application run.
- Persistent Entrance sessions live under
  `<workspace>/var/system/entrance/sessions/`.
- `/create` publishes a database only after acquiring its target session
  lease. Publication is the point of no return.
- `/open` and `/create` are available after every successful switch, including
  in workspace sessions and persistent Entrance sessions.
- Seven commands are rejected immediately during generation: `/iam`, `/open`,
  `/create`, `/forums`, `/sessions`, `/members`, and `/personas`. `/help`
  remains available.
- The web UI does not gain the built-in environment or terminal navigation
  commands. Only the shared provider-layer change affects it.

## 2. Block 1 — configuration, metadata, names, and workspace snapshot

### Objective

Create the immutable, validated application inputs required by all later
blocks without changing either terminal startup flow yet. At the end of this
block the workspace can describe personas, character definitions, and forums
by public name and optional description; provider configuration has four-layer
precedence; and one startup snapshot can be used without rescanning identity
metadata.

### Design references

Read Sections 4, 5.2 through 5.3, 9.1, 9.3, 11.1, 12.1, 12.4, 15, and 16.1
through 16.3 of the design.

### Primary current files

- `src/agents/config.h`
- `src/agents/config.cpp`
- `src/agents/agent.h`
- `src/agents/agent.cpp`
- `src/agents/persona.h`
- `src/session/workspace.h`
- `src/session/workspace.cpp`
- `src/apps/web_main.cpp`
- `tests/agents/unit_config_loader.cpp`
- `tests/agents/unit_agent_definition_loader.cpp`
- `tests/session/unit_persona_loader.cpp`
- `tests/session/unit_workspace.cpp`
- `tests/support/test_workspace.h`
- `tests/support/test_workspace.cpp`
- `workspace/app.toml`
- the checked-in persona, character, and forum TOML files
- `CMakeLists.txt`

### Planned new files

- `src/util/public_name.h`
- `src/util/public_name.cpp`
- `src/application/workspace_snapshot.h`
- `src/application/workspace_snapshot.cpp`
- `src/application/workspace_inventory.h`
- `src/application/workspace_inventory.cpp`
- `tests/application/unit_workspace_snapshot.cpp`
- `tests/application/unit_workspace_inventory.cpp`

Equivalent names are acceptable, but the reusable code must not be placed in a
frontend directory.

### 2.1 Establish and record the baseline

Before changing behavior, configure, build, and run the current suite:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

This is the same command every block ends with. On the current tree it runs 548
tests in about five seconds.

Record any pre-existing failure in the implementation handoff. Do not weaken a
new assertion merely because an unrelated existing test is failing.

One such failure is already known:
`WebSessionRuntime.OneActiveStreamRejectsConflictsAndIgnoresStaleCloses`
fails intermittently under parallel execution. It passed on every isolated
rerun and on repeated full `-j8` runs, so it is a pre-existing parallelism
flake in web code that this work does not touch. Do not attribute it to a
block, and do not fix it here. If it becomes disruptive, contain it with
`--repeat until-pass:2` rather than serializing the suite.

### 2.2 Add reusable public-name and description validation

Move the common parts of the current persona display-name validation into a
small utility that can validate persona, forum, character, and session public
names consistently. It must:

- require non-empty valid UTF-8;
- reject control characters and line breaks;
- reject leading or trailing Unicode whitespace;
- preserve internal whitespace and authored casing;
- provide the existing `fold_ascii()` value for comparison;
- optionally reject leading `/` and `@` for participant names;
- return diagnostics that name the entity and source file without printing a
  hidden storage key as a suggested command value.

Add a companion validator for optional descriptions. A present description is
a single non-empty line, may contain internal whitespace, and may not begin or
end with whitespace. Do not trim and silently repair authored data.

Keep participant-specific reserved-name validation outside the generic utility.
Add `guest` to the folded reserved participant-name set while retaining `you`
and all existing protocol-reserved names. Trusted built-in construction will
bypass this workspace-only check in Block 3.

Add focused utility or loader tests for invalid UTF-8, control characters,
line breaks, boundary whitespace, internal spaces, casing preservation, and
ASCII-folded equality.

### 2.3 Extend definition metadata with descriptions

Change the typed metadata as follows:

- `Persona` gains an optional description.
- `CharacterDefinitionMetadata` gains an optional description.
- `Forum` gains an optional description.
- Keep private IDs/directories in the storage-facing values, but expose
  public-name-only snapshot/list values to the future application layer.

Parse optional `description` from:

- `personas/<key>/persona.toml`;
- `characters/<key>/character.toml` at the definition layer only;
- `forums/<key>/config.toml`.

Reject `description` in forum character defaults and member overrides. Reuse
the description validator instead of implementing three slightly different
checks. Missing descriptions are valid and remain absent; no code may derive a
description from `PERSONA.md`, `CHARACTER.md`, or `FORUM.md`.

Update loader tests to cover present, missing, malformed, and forbidden-layer
descriptions. Update fixtures so they can author descriptions without forcing
every test to provide one.

### 2.4 Add the application provider layer

Keep the top-level application fields used by the web server:

```toml
host = "127.0.0.1"
port = 8080
```

Add a required `[provider]` table to `ApplicationConfig`. Represent it as a
typed provider patch plus the source path `app.toml`, so an effective-value
error can identify the layer that supplied the value.

Extend configuration precedence from three layers to four, weakest to
strongest:

1. application `[provider]`;
2. character definition;
3. forum defaults;
4. member override.

Replace any path-only `load_config()` request with a named request that carries
the application provider layer and the three existing optional file layers.
Do not make callers pass four positional values.

The application layer accepts provider/runtime fields only. It must reject
identity fields (`id`, `name`, `display_name`), `tags`, `description`, and the
`[prompt]` table. Keep definition-only and forum-layer restrictions intact.
Track the source of effective `port`, `temperature`, and any other validated
overridable value through all four layers. An invalid application value must
name `app.toml`; an invalid override must name the file containing the
override.

`Assistant` will consume the parsed provider patch directly in Block 3.
Workspace character definition loading must consume it now, including the
paths used by the web frontend. A character definition containing only its
display name plus genuine overrides must load when `[provider]` supplies the
required endpoint.

Add tests that prove:

- a character with no connection fields inherits `[provider]`;
- every layer overrides only the corresponding keys below it;
- application-layer forbidden fields fail and name `app.toml`;
- missing or malformed `[provider]` fails workspace validation;
- top-level bind `host`/`port` and provider `host`/`port` remain distinct;
- loading and checking configuration does not initialize a completion backend
  or open a socket;
- web-created workspace characters inherit the application provider layer.

### 2.5 Build one immutable workspace snapshot

Introduce an application-facing `WorkspaceSnapshot` constructed after
`Workspace` validation. It should own or refer immutably to:

- all custom personas and their private author keys, public names, prompts,
  and optional descriptions;
- all character definition metadata: private key, public name, optional
  description, and tags;
- all forum metadata: private key, public name, optional description, member
  character keys/names, default character, directory, and stored-session
  catalog location;
- name indexes keyed by folded public name.

Construction performs all application-wide uniqueness checks:

- custom persona public names are unique;
- custom forum public names are unique;
- existing character public-name uniqueness remains enforced;
- custom participant names cannot collide with `Guest` or `Assistant`;
- no custom forum can claim `Entrance`;
- private storage keys continue to satisfy their existing path/URL rules.

An empty but existing `personas/` directory is valid. Missing `personas/` may
remain a workspace-shape error unless implementation evidence requires the
design to say otherwise. There is no minimum custom-persona count.

Application-facing lookup APIs accept a public name and return a resolved value
that contains the private key internally. Listings return public names only in
folded-name order. The frontend must never receive an ID/name pair and choose
between them.

Do not initialize character providers while building the snapshot. Structural
validation may load and expand definition/forum prompts as needed for
`--check`, but no `CompletionClient` or backend is created.

### 2.6 Build deterministic inventory data

Add a transport-neutral `WorkspaceInventory` value derived from the snapshot.
It contains:

- custom persona public names and optional descriptions;
- character public names, optional descriptions, and tags;
- forum public names, optional descriptions, public member names, and the
  explicitly identified public default-character name.

It excludes sessions, paths, private keys, configuration values, secrets, and
full prompts. Sort every top-level collection by folded public name; sort forum
members by the same rule independently of their storage-key order.

Add a deterministic serializer for later Assistant prompt assembly. Prefer a
structured representation with correct string escaping, such as an ordered
JSON value inside a clearly labelled reference-data section. The surrounding
text must say that inventory strings are data, not behavioral instructions.

Tests must compare exact serialization for ordering and escaping, verify
name-only rows when descriptions are absent, and search the output for known
fixture keys, paths, provider values, prompt contents, and sessions to prove
they are absent. Construct a snapshot, edit files, and prove the existing
inventory is unchanged while a newly constructed snapshot sees the edits.

### 2.7 Migrate fixtures and keep startup temporarily compatible

Update checked-in and generated `app.toml` fixtures with `[provider]`. Move
shared provider defaults out of forum defaults where the test is meant to
exercise inheritance; retain explicit forum/member overrides only where a test
needs them.

Add useful descriptions to the checked-in sample personas, character
definitions, and forums. Descriptions are optional, so do not add placeholder
text to fixtures that test absence.

The TUI and console may still select their startup entities at the end of this
block. Adapt their composition roots only enough to pass the provider snapshot
to existing session factories. Do not introduce partial built-in startup or
frontend-specific public-name lookup yet.

### Block 1 verification

```sh
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

### Block 1 exit criteria

- The four-layer provider chain is used by all workspace agent factories.
- Top-level web bind configuration is still distinct and working.
- Description metadata is parsed and validated in exactly the allowed files.
- Empty custom-persona collections validate.
- Persona and forum public-name collisions fail at snapshot construction.
- Workspace inventory is complete, deterministic, immutable for the run, and
  contains no sessions or private implementation data.
- Existing terminal startup remains functional pending the later blocks.

## 3. Block 2 — name-based session catalog and serialized creation

### Objective

Make stored sessions discoverable and mutable by public session name while
preserving private database stems, existing web key-based APIs, atomic
publication, and cross-process lease safety. Close the create/open race before
any navigation command depends on it.

### Design references

Read Sections 4.1 through 4.2, 6.3, 8.3, 9.2, 12.1 through 12.3, 14, 15, and
16.1 and 16.4 of the design.

### Primary current files

- `src/session/session_catalog.h`
- `src/session/session_catalog.cpp`
- `src/session/session_lease.h`
- `src/session/session_lease.cpp`
- `src/session/session_database.h`
- `src/session/session_database.cpp`
- `src/session/session_identity.h`
- `src/session/workspace.h`
- `src/session/workspace.cpp`
- `tests/session/unit_session_catalog.cpp`
- `tests/session/unit_session_lease.cpp`
- `tests/session/unit_workspace.cpp`
- `tests/support/lease_test_helper.cpp`
- `tests/support/lease_test_process.h`
- `tests/support/lease_test_protocol.h`
- `CMakeLists.txt`

### Planned new files

- `src/session/catalog_lease.h`
- `src/session/catalog_lease.cpp`
- `tests/session/unit_catalog_lease.cpp`

If platform lock code would otherwise be duplicated, factor the operating
system descriptor/handle ownership into a private reusable exclusive-file-lock
implementation. Keep `SessionLease` and `CatalogLease` as distinct public
types with different acquisition policy and diagnostics.

### 3.1 Separate public session names from private storage IDs

The existing SQLite `label` becomes the public session name. Introduce clear
storage-facing and application-facing result types:

- stored/resolved values may contain a private database ID and path;
- list rows contain public `name`, an optional corruption diagnostic, and an
  `ambiguous` flag;
- application command APIs take forum and session public names;
- existing trusted key-based methods remain available for web URLs and other
  internal callers.

Do not rename existing databases or rewrite metadata. Validate every requested
new public name with the shared validator from Block 1 and require an explicit,
non-empty name for name-based creation. Keep the existing timestamp-based stem
generator as a private filename generator only.

Add a full-catalog public lookup that reads metadata and compares names using
ASCII folding. It must return exactly one healthy match, distinguish not found
from ambiguous, and never select the first match silently.

### 3.2 Implement defensive public listing and resolution

Public listing scans `.sqlite3` files and produces rows ordered by folded
public name, with a deterministic secondary order for equal folded names.

During a scan:

- a healthy database contributes its metadata name;
- a corrupt database contributes an error row without presenting its stem as
  an alternative public identity;
- all healthy rows sharing a folded name are marked ambiguous;
- healthy, unique rows remain usable even when another file is corrupt or
  ambiguous;
- `catalog.cha-lock`, session companion lock files, and unrelated directory
  entries remain invisible.

Public `/open` resolution fails on ambiguity with the exact public-name
diagnostic from the design. Corrupt rows are unopenable. Existing direct-ID
resolution keeps its current metadata identity checks for web compatibility.

### 3.3 Add the bounded forum-catalog lease

Implement `CatalogLease` over `<sessions>/catalog.cha-lock` with the same
cross-platform kernel locking behavior as `SessionLease`:

- POSIX uses a non-blocking exclusive `flock` on a mode-0600 descriptor;
- Windows uses an exclusive `LockFileEx` region on an open handle;
- the lock file is never deleted;
- descriptor/handle destruction releases the lock after exceptions and process
  termination;
- no stale-file inspection or cleanup protocol exists.

Unlike `SessionLease`, catalog acquisition retries with a short backoff until a
bounded deadline of roughly two seconds. Make the clock/backoff policy
injectable enough that unit tests do not actually wait for two seconds. Timeout
is a typed recoverable catalog-busy error suitable for `/create`.

Keep session acquisition immediate. Do not add retries to `SessionLease`.

### 3.4 Publish a session while already owning its target lease

Implement name-based create in this exact order:

1. Validate the requested public name and any built-in reservation supplied by
   the session source.
2. Create the sessions directory if needed.
3. Acquire the catalog lease.
4. Rescan all stored databases under the lease.
5. Compare every readable public name using ASCII folding.
6. If a collision exists, fail with the `use /open` diagnostic and publish
   nothing.
7. Generate a private candidate database stem.
8. Acquire `SessionLease` for the candidate's companion path before the
   database exists.
9. Atomically create and initialize the SQLite database.
10. Release the catalog lease while retaining the target session lease.
11. Return one move-only prepared value containing resolved metadata, database
    path, and the already-held `SessionLease`.

If publication reports a stem collision, release that candidate's session
lease and try another stem while still holding the catalog lease. If any step
before database publication throws, leave no database. Never reacquire the
target lease between publication and controller construction.

Keep corrupt databases out of name-collision checks because their public names
cannot be read. Log their storage diagnostics, but do not let one damaged file
permanently block all creation.

### 3.5 Preserve lock ordering and compatibility APIs

Document in code that the only nested order is catalog lease then a new,
unpublished target session lease. Listing and opening take no catalog lease.
No operation may acquire a catalog lease while waiting on a session it intends
to open.

Adapt `Workspace::create_stored_session()` and web callers carefully:

- terminal/application creation will use the prepared, already-leased result;
- a compatibility API that publishes without immediately opening must either
  consume and release the returned lease explicitly or be renamed so its
  semantics are obvious;
- do not create a second list-then-create path for web or tests;
- existing session-ID URLs and `SessionIdentity` remain valid.

### 3.6 Add unit and process concurrency tests

Extend the helper-process protocol rather than testing POSIX locking by
forking an already-threaded GoogleTest process. Cover:

- two processes creating the same folded public name: exactly one database and
  one recoverable conflict;
- two different names in one forum: both eventually succeed;
- creator holds the target session lease before the database becomes visible;
- a concurrent opener cannot acquire the new session until the creator
  releases or transfers the lease;
- creator death releases catalog and target locks through the OS;
- bounded catalog contention times out without freezing the test;
- a corrupt database does not block a healthy create;
- failed pre-publication initialization leaves no `.sqlite3` file;
- lock files never appear in public or compatibility listings;
- same public session name is allowed in different forum directories;
- same-forum ASCII-folded duplicates are rejected.

### Block 2 verification

```sh
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

### Block 2 exit criteria

- Public session lookup and listing operate on metadata names, not stems.
- Ambiguous rows are visible and unopenable without hiding healthy rows.
- Same-forum creation is serialized across processes.
- A successfully published database is already protected by its creator's
  session lease.
- Pre-publication failures leave no database.
- Existing ID-based web/session behavior still passes its tests.

## 4. Block 3 — built-ins, shared roster, Entrance, and Welcome

### Objective

Construct the complete built-in environment and the reusable session-source
facade, but keep frontend loops out of this block. At the end, tests can create
an application environment whose initial opened session is `Guest` in
`Entrance / Welcome`, reopen that run's Welcome transcript, and open or create
ordinary workspace and Entrance sessions using the same factories.

### Design references

Read Sections 4.3, 5, 6, 7.1, 12.4 through 12.5, 13, 14, and 16.2 and 16.5 of
the design.

### Primary current files

- `src/agents/agent.h`
- `src/agents/agent.cpp`
- `src/agents/agent_registry.cpp`
- `src/agents/persona.h`
- `src/session/session_controller.h`
- `src/session/session_controller.cpp`
- `src/session/opened_session.h`
- `src/session/workspace.h`
- `src/session/workspace.cpp`
- `tests/agents/unit_agent_context.cpp`
- `tests/agents/unit_agent_definition_loader.cpp`
- `tests/session/unit_session_controller.cpp`
- `tests/session/unit_workspace.cpp`
- `tests/support/test_controller.h`
- `CMakeLists.txt`

### Planned new files

- `src/application/builtins.h`
- `src/application/builtins.cpp`
- `src/application/effective_personas.h`
- `src/application/effective_personas.cpp`
- `src/application/forum_catalog.h`
- `src/application/forum_catalog.cpp`
- `src/application/session_source.h`
- `src/application/session_source.cpp`
- `src/application/welcome_storage.h`
- `src/application/welcome_storage.cpp`
- `docs/application-guide.md`
- `cmake/embed_text.cmake`
- `tests/application/unit_builtins.cpp`
- `tests/application/unit_effective_personas.cpp`
- `tests/application/unit_forum_catalog.cpp`
- `tests/application/unit_welcome_storage.cpp`

### 4.1 Embed one canonical application guide

Create `docs/application-guide.md` as the authoritative Assistant-facing guide
to the running application's commands, concepts, storage behavior, and common
errors. Link to it from the main README instead of copying the same prose into
two manually maintained sources.

Add a build step that embeds the guide bytes into a generated C++ source or
header. The generation must:

- depend on `docs/application-guide.md`, so editing it rebuilds the asset;
- preserve UTF-8 and arbitrary Markdown without fragile hand-written raw-string
  delimiters;
- make the bytes available through a read-only application function;
- avoid reading a workspace-relative documentation file at runtime;
- work in all CMake presets and on Windows.

Do not hand-maintain a second C++ string containing the same documentation.
Add a test that checks known guide headings and commands in the embedded asset.

### 4.2 Construct the effective persona catalog once

Add the trusted built-in `Guest` persona and combine it with the custom persona
snapshot from Block 1. The effective catalog owns exactly one
`std::shared_ptr<const PersonaRoster>` (or an equivalent wrapper with the same
identity contract), ordered as Guest followed by custom personas in folded
public-name order.

The catalog provides:

- folded public-name lookup;
- public-name listing of custom personas only;
- access to Guest by name even though it is omitted from the list;
- resolution to the private author key used in transcript writes;
- the one shared roster reference for every controller and prompt builder.

Change all production `SessionController` construction paths to accept and
store the shared immutable roster. Change prompt assembly to read through the
same shared object. Remove production calls that reload or move a
`PersonaRoster` per session. Test-only factories may provide a small helper
that wraps a fixture roster, but tests of multiple controllers must be able to
assert identical object identity.

Keep the effective catalog alive longer than every controller. Ensure member
declaration/destruction order reflects that requirement.

### 4.3 Refactor common agent prompt assembly

`Assistant` must receive all identity, persona-roster, shared-history, and forum
context instructions that ordinary agents receive. Refactor the currently
private prompt/context helpers in `agents/agent.cpp` into reusable functions
instead of recreating their prose in the built-in factory.

The built-in Assistant prompt is assembled deterministically from:

1. built-in application-guide role instructions;
2. the embedded application guide;
3. the serialized immutable workspace inventory from Block 1;
4. built-in Entrance instructions;
5. the effective persona roster;
6. the normal generated forum context.

The inventory boundary must state that its values are reference data, not
instructions. Assistant uses the application provider configuration directly
with no definition/default/member override. Give its config a trusted private
participant key and public name `Assistant`; never surface the key.

Test the complete effective prompt, not just individual fragments. Assert that
it contains documentation, public inventory, Guest and ordinary persona
context, Entrance context, and the normal shared-history guidance. Assert that
it excludes known private keys, paths, provider values, secrets, full workspace
prompts, and session names.

### 4.4 Add Entrance and the forum-catalog facade

Construct a trusted built-in forum with public name `Entrance`, exactly one
member `Assistant`, and Assistant as default. It has no workspace entity
directory.

Add an application `ForumCatalog` over:

- the built-in Entrance descriptor;
- the immutable custom forum snapshot;
- folded public-name lookup;
- custom-only public listing in folded-name order.

It also resolves a built-in or custom forum to its public member character
names, ordered by folded public name. `Entrance` returns exactly `Assistant`;
workspace forums join validated member storage keys to character public names
inside the catalog boundary.

Resolve each forum to a `SessionSource` abstraction that can:

- list persistent sessions by public name;
- resolve and prepare an existing session open;
- create and prepare a persistent session;
- construct the forum's agent definitions and controller using the shared
  persona roster.

Workspace forum sources delegate storage to their existing `sessions/`
directories and load their ordinary agent definitions. Entrance's persistent
source delegates to `<workspace>/var/system/entrance/sessions/` and constructs
the built-in Assistant. Avoid scattering `if (forum == Entrance)` checks
through future command handling.

Keep the ephemeral Welcome source separate from the persistent Entrance
catalog. This provenance, rather than filtering the text `Welcome`, is what
excludes it from `/sessions Entrance`.

### 4.5 Add run-scoped Welcome storage

Implement an RAII `WelcomeStorage` owner that:

- creates a unique private temporary directory per application construction;
- creates one SQLite database with internal metadata corresponding to Entrance
  and public name Welcome;
- acquires a normal `SessionLease` before constructing each Welcome controller;
- retains the directory and database after switching away;
- reopens the same database and restores its transcript within the run;
- recursively removes only the exact directory it created during normal
  destruction, after all controllers are gone;
- logs cleanup failure without treating an old directory as next run's
  Welcome;
- never deletes a workspace or persistent Entrance directory.

Use a secure collision-resistant temporary-directory creation loop and explicit
ownership state. Do not delete a broad temp parent and do not use the process
working directory.

The Welcome session source resolves `Welcome` explicitly, never lists it, and
rejects `/create Entrance Welcome` before taking the persistent catalog lease.
It may coexist with an ordinary session named Welcome in a different forum.

### 4.6 Add prepared open factories

Provide one prepared-open representation that couples:

- public session descriptor;
- resolved private storage path/source;
- acquired session lease;
- restored `SessionRestore`;
- the definitions and default agent needed to build the controller.

For ordinary `/open`, acquire and restore the target fully before replacing any
current controller. For name-based `/create`, consume the already-held lease
returned by Block 2. Controller construction must not resolve the name or
acquire the lease again.

Keep existing key-based `Workspace::open_session()` behavior available to web
callers, adapting it to the shared configuration plumbing without routing it
through terminal public-name commands.

### 4.7 Test built-in and lifecycle behavior

Add tests for:

- trusted construction of Guest and Assistant despite workspace reservation;
- rejection of custom Guest, Assistant, and Entrance collisions under folded
  comparison;
- Entrance membership and default-agent invariants;
- custom-only persona/forum listings while built-ins remain resolvable;
- every controller receiving the same roster object;
- no persona reload when another session is opened;
- distinct Welcome paths for two application environments;
- empty initial Welcome transcript;
- switching the test owner away and reopening Welcome restores that run's
  transcript;
- a second environment cannot see the first one's Welcome;
- Welcome absence from listings;
- persistent Entrance sessions surviving environment reconstruction;
- cleanup removing only the owned temporary directory;
- cleanup failure logging through an injected seam where practical.

### Block 3 verification

```sh
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

### Block 3 exit criteria

- The built-in environment can be constructed without built-in workspace
  directories or files.
- Assistant uses `[provider]` and has the complete, safe, version-matched
  system prompt.
- One shared immutable roster object reaches every terminal controller.
- Entrance persistent and Welcome ephemeral sources obey one common interface.
- Welcome is fresh per environment, restorable within the environment, and
  absent from stored-session listings.
- No terminal frontend has implemented switching yet.

## 5. Block 4 — chat application, commands, and switch transactions

### Objective

Add the owner-thread-only application coordinator above `SessionController`
and the shared terminal command dispatcher. This block establishes all
navigation semantics and failure guarantees in UI-independent unit tests.

### Design references

Read Sections 7 through 9, 12.1, 12.5, 13, 14, 16.1, and 16.6 of the design.

### Primary current files

- `src/session/opened_session.h`
- `src/session/session_controller.h`
- `src/session/session_controller.cpp`
- `src/session/session_change.h`
- `src/ui/text/command.h`
- `src/ui/text/command.cpp`
- `src/ui/text/text_input.h`
- `src/ui/text/text_input.cpp`
- `tests/ui/text/unit_command.cpp`
- `tests/ui/text/unit_text_input.cpp`
- `CMakeLists.txt`

### Planned new files

- `src/application/application_result.h`
- `src/application/chat_application.h`
- `src/application/chat_application.cpp`
- `src/application/application_dispatcher.h`
- `src/application/application_dispatcher.cpp`
- `src/ui/text/application_command.h`
- `src/ui/text/application_command.cpp`
- `tests/application/unit_chat_application.cpp`
- `tests/application/unit_application_dispatcher.cpp`
- `tests/ui/text/unit_application_command.cpp`

### 5.1 Define semantic application results

Add a frontend-neutral result capable of carrying:

- input-consumed state;
- optional one-line notice;
- optional titled list with sanitized-at-presentation public rows;
- persona-changed state;
- session-changed state and the new public descriptor;
- recoverable error state with no mutation;
- any `SessionChange` produced by a delegated single-session command;
- exit request state where the text dispatcher needs it.

Keep this distinct from `SessionChange`. Application code must not write to
curses, stdout, or stderr, and it must not sanitize for a specific terminal.
Frontends obtain the current controller through the application owner after a
session change instead of retaining a permanently bound reference.

### 5.2 Implement `ChatApplication` ownership

`ChatApplication` owns, in safe destruction order:

- immutable application configuration;
- validated workspace snapshot and inventory;
- effective persona catalog and its shared roster;
- forum catalog and session sources;
- Welcome temporary-storage owner;
- current public persona name and private author key;
- current `OpenedSession` and its public forum/session descriptor.

Construction creates and opens fresh Welcome and selects Guest. Expose
owner-thread-only accessors for current controller, public descriptor, selected
persona, selected author key, and current default-agent information.

Its destructor must make a best effort to shut down the current controller,
but explicit frontend shutdown remains responsible for propagating failures.
Keep Welcome storage and the roster alive until after the current controller is
destroyed.

### 5.3 Implement persona and listing operations

Add UI-independent operations:

- `iam(public_name)` resolves against the effective catalog, updates only
  future authorship, and returns `Now speaking as <name>`;
- `forums()` returns custom public forum names only;
- `personas()` returns custom public persona names only;
- `sessions(forum_name)` resolves built-in or custom forums and returns stored
  public rows, excluding run-scoped Welcome;
- `members(forum_name)` resolves built-in or custom forums and returns public
  character names in folded-name order, including Assistant for Entrance;
- empty results return explicit empty-list messages;
- corrupt/ambiguous session rows retain their annotations without exposing
  database filenames.

All operations preserve selected persona across later session switches.
Listings never mutate or enter the transcript and never initialize providers
for the entities merely listed.

### 5.4 Implement prepared switch and commit

Both `open(forum_name, session_name)` and
`create(forum_name, session_name)` use one commit function.

Before commit:

- reject the operation if the current controller is generating;
- resolve all public names;
- detect an already-current open as a successful no-op;
- for open, validate metadata, acquire the target lease, restore the database,
  initialize target providers, and construct the target controller;
- for create, validate the name, publish under the catalog lease while already
  owning the target lease, then construct the target controller from that
  lease;
- leave the current controller untouched on every ordinary preparation
  failure.

Commit performs:

1. mark the accepted command input consumed;
2. call old `SessionController::shutdown()` explicitly and allow failure to
   propagate;
3. destroy the old controller only after successful shutdown;
4. install the prepared `OpenedSession`;
5. retain the selected persona and author key;
6. report the public forum and session names and `session_changed` state.

It is expected that both controllers and both leases coexist during
preparation. Do not tear down the old controller early to save resources.

If old shutdown fails after cancellation/worker teardown has begun, propagate
it as a process-level failure. Do not claim the old session was restored.

### 5.5 Implement `/create` publication failure semantics

Distinguish creation failures by whether SQLite publication occurred:

- before publication: return a recoverable error, store nothing, and retain the
  current session;
- after publication but before target controller installation: retain the new
  empty stored session, release its prepared session lease, retain the current
  session, and report:

  ```text
  Session 'Name' was created in forum 'Forum' but could not be opened: <reason>
  ```

- never delete a published database as rollback;
- a later `/open` can open it after the cause is fixed;
- repeated `/create` reports that it already exists and recommends `/open`.

Add a controller/provider construction fault seam if needed to test both sides
of the publication boundary deterministically.

### 5.6 Add the fixed-arity quoted-name lexer

Keep the existing free-form session command parser for `/mcast`, addressing,
and other current commands. Add a separate fixed-arity application command
parser in `ui/text` for:

- `/iam` — one name;
- `/open` — two names;
- `/create` — two names;
- `/forums` — zero names;
- `/sessions` — one name;
- `/members` — one name;
- `/personas` — zero names;
- `/help` — zero names.

The lexer implements only the grammar in the design: ASCII whitespace,
double-quoted arguments, `\"`, and `\\`. Reject unmatched quotes,
unsupported escapes, empty quoted names, missing arguments, and extra
arguments. Do not use `wordexp`, a shell, environment expansion, globbing, or
single quotes.

Return typed parse errors that the dispatcher maps to stable usage notices.
Test every valid and invalid example, including names with spaces and escaped
quotes/backslashes.

### 5.7 Dispatch terminal text without changing web behavior

Add a terminal application dispatcher that:

1. recognizes application commands;
2. answers `/help` from a built-in complete command table at any time;
3. rejects the other seven immediately while generation is active;
4. invokes `ChatApplication` for application operations;
5. delegates ordinary text and existing session commands to the current
   `handle_text_input(SessionController&, author_key, input)` path;
6. uses the coordinator's current author key for every prompt after `/iam`;
7. produces the expanded terminal unknown-command diagnostic.

Retain the controller-level `handle_text_input` entry point for web and focused
session tests. Do not make the web runtime construct `ChatApplication`, and do
not accidentally enable `/open` in the HTTP command protocol.

The `/help` table is one data source used for both parsing documentation and
rendering. It lists every current command, including `/@Name`, arity, and a
one-line description, without making a completion request.

### 5.8 Test the coordinator as a transaction owner

Use fake session sources/backends and temporary databases to cover:

- initial Guest and Entrance/Welcome state;
- `/iam` changing future authors only and surviving switches;
- open and create from Welcome, workspace sessions, and persistent Entrance
  sessions;
- open from one non-default session directly to another forum/session;
- opening the current session as a no-op;
- failed resolution, busy lease, corrupt database, and target provider failure
  preserving current state;
- pre-publication create failure storing nothing;
- post-publication open failure retaining the session and releasing its lease;
- successful commit releasing the prior session lease;
- explicit shutdown failure propagating;
- no transcript, registry, worker, counter, default-agent, or off-record state
  crossing into the target controller;
- same shared persona-roster identity before and after switching;
- built-ins omitted from lists but directly resolvable;
- `/members` lists public character names without changing the current
  persona, session, controller, or transcript, and `/members Entrance` returns
  exactly Assistant;
- `/create Entrance Welcome` rejected;
- all seven stateful/catalog commands rejected during generation;
- `/help` available during generation and complete;
- no public result or error containing fixture private keys.

### Block 4 verification

```sh
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

### Block 4 exit criteria

- All application behavior is testable without a terminal.
- The coordinator, not `SessionController`, owns persona and session changes.
- `/open` works from every current session.
- `/create` has explicit pre- and post-publication failure behavior.
- Commands use quoted public names and never expose private keys.
- The existing web command path remains session-scoped.

## 6. Block 5 — console integration

### Objective

Replace console startup selection with immediate Entrance/Welcome startup and
make its append-only loop follow the mutable `ChatApplication` controller while
preserving transcript, notice-stream, FIFO, backpressure, and shutdown
semantics.

### Design references

Read Sections 7.5, 8 through 9, 11, 14, 15, 16.8, and the console portions of
16.9 and 18 of the design.

### Primary current files

- `src/apps/console_main.cpp`
- `src/ui/console/console_startup.h`
- `src/ui/console/console_startup.cpp`
- `src/ui/console/console_session.h`
- `src/ui/console/console_session.cpp`
- `src/ui/console/transcript_emitter.h`
- `src/ui/console/transcript_emitter.cpp`
- `src/ui/console/console_writer.h`
- `src/ui/console/console_writer.cpp`
- `tests/ui/console/unit_console_startup.cpp`
- `tests/ui/console/unit_console_session.cpp`
- `tests/ui/console/unit_transcript_emitter.cpp`
- `tests/integration/console_process_test.cpp`
- `tests/support/scripted_console.h`

### 6.1 Simplify command-line parsing

Remove `--persona`, `--forum`, `--session`, `--new`, `--list-forums`, and
`--list-sessions`. The accepted forms become:

```text
chacon [--color=auto|always|never]
chacon --check
```

`--check` takes no forum and cannot be combined with interactive-only options
unless the existing color policy explicitly permits it. Removed flags and
extra positional arguments return exit code 2 with clear usage errors.

Parse arguments early enough to select `--check` before constructing built-ins
or providers. Loading `app.toml`, including structural validation of required
`[provider]`, is allowed and required.

### 6.2 Implement whole-workspace `--check`

Replace per-forum check with a whole-workspace structural validation method.
It validates:

- app configuration and `[provider]` shape;
- all personas and character definitions;
- all forums, member references, defaults, configuration overlays, and prompt
  files.

It opens no session, creates no Welcome temp directory, initializes no backend,
requires no credential value, and performs no network operation. Return 0 on a
valid workspace, 1 on validation failure, and 2 on usage failure. Print a
stable whole-workspace success message rather than a selected forum report.

### 6.3 Start interactive console in Welcome

For ordinary startup:

1. load environment and application configuration;
2. initialize diagnostic logging;
3. validate and snapshot the workspace;
4. construct `ChatApplication` with the console notifier;
5. create the initial emitter for current Welcome;
6. if stdin is a TTY, print `Entrance / Welcome ready` to the notice stream;
7. enter the console loop with no selector arguments.

The initial prompt names Assistant. Never print Welcome's internal database
stem or private participant key.

### 6.4 Make the console loop controller-relative

Change `ConsoleSession` to hold `ChatApplication&`, not a permanent
`SessionController&` and fixed author ID. Query `application.controller()` and
the current default agent at every operation that can follow a switch.

Maintain an emitter associated with the current controller. On successful
`/open` or `/create`:

1. emit and flush the old controller's final suffix before committing or
   acknowledging the switch;
2. dispatch the application command;
3. write the sanitized public switch notice to the notice stream;
4. discard the old emitter watermark;
5. construct/reset an emitter for the new controller;
6. emit restored target history once under normal restored-entry rules;
7. rearm the prompt using the new default agent.

If needed, split dispatch preparation from commit or add a narrow pre-switch
flush callback so a failed transcript flush prevents the switch. Do not rely on
being “usually caught up” as a substitute for the specified ordering.

Application lists and notices go to the existing notice stream and never to
the transcript stream or SQLite database.

### 6.5 Preserve active-generation and FIFO behavior

The current queue waits to process ordinary lines until the controller is
idle. Add application-command classification at enqueue time so:

- `/help` is answered immediately even during generation;
- the other seven application commands are rejected immediately during
  generation and never retained for later execution;
- `/stop` remains immediate;
- ordinary prompts retain existing FIFO and backpressure behavior;
- after an idle successful switch, already-read following lines execute
  against the new current controller in FIFO order.

Signals and notifier events must always target the current controller. EOF
continues to drain an active response before exit. Explicit final shutdown goes
through `ChatApplication` and propagates a clean-path persistence failure.

### 6.6 Update console tests

Replace selection/listing startup tests with tests for the new option grammar
and whole-workspace check. Extend unit and process tests to cover:

- no-argument interactive and piped startup;
- removed flags returning exit code 2;
- `--check` with no provider credentials and no server request;
- exact public ready banner;
- Guest authorship before `/iam` and selected authorship afterwards;
- `/open` and `/create` with quoted names;
- `/members` with a quoted forum name, including public-name ordering and
  Entrance's Assistant row;
- switching flush order and one-time restored history;
- notice/list output staying off stdout's transcript contract;
- prompt default-agent update after forum switch;
- immediate navigation rejection during generation;
- FIFO input after a successful switch targeting the new session;
- restart clearing Welcome while retaining an ordinary created session;
- busy, ambiguous, corrupt, and post-publication failure diagnostics containing
  public names only.

### Block 5 verification

```sh
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

### Block 5 exit criteria

- `chacon` needs no entity-selection arguments and starts in Welcome.
- `chacon --check` validates the entire workspace without a provider backend.
- Every application command works from the current console session.
- Session changes preserve append-only output ordering and reset emitter state.
- Active-generation navigation is rejected rather than delayed.
- Removed startup/listing flags are no longer accepted.

## 7. Block 6 — TUI integration

### Objective

Remove the TUI startup-selection workflow, route its event loop through
`ChatApplication`, reset all transcript-bound rendering state on a switch, and
present application lists in a transient scrollable overlay.

### Design references

Read Sections 7.5, 8 through 10, 14, 15, 16.7, and the TUI portions of 16.9 and
18 of the design.

### Primary current files

- `src/apps/tui_main.cpp`
- `src/ui/tui/persona.h`
- `src/ui/tui/persona.cpp`
- `src/ui/tui/persona_session.h`
- `src/ui/tui/persona_session.cpp`
- `src/ui/tui/session_view.h`
- `src/ui/tui/tui.h`
- `src/ui/tui/tui.cpp`
- `src/ui/tui/render_plan.h`
- `src/ui/tui/render_plan.cpp`
- `src/ui/tui/screen_layout.h`
- `src/ui/tui/screen_layout.cpp`
- `tests/ui/tui/unit_persona_session.cpp`
- `tests/ui/tui/unit_render_plan.cpp`
- `tests/ui/tui/unit_screen_layout.cpp`
- `CMakeLists.txt`

The startup-selector files remain present until Block 7 so this block can focus
on replacing all callers before deletion.

### 7.1 Compose Welcome directly

Replace selector use in `tui_main.cpp` with:

1. environment and application configuration loading;
2. logging initialization;
3. workspace validation/snapshot;
4. terminal and event-loop construction;
5. `ChatApplication` construction, which opens Welcome;
6. direct entry into the chat run loop.

There is no cancellation path for persona/forum/session selection. Startup
errors now cover workspace/configuration problems, temporary database failure,
and Assistant provider initialization.

### 7.2 Make the TUI session state machine application-relative

Rename `run_persona` and `PersonaSession` if useful, because the object now
represents an application chat rather than one immutable persona/controller.
Its owner must hold `ChatApplication&` and resolve the current controller for
rendering, event receipt, stop requests, input submission, default-agent target
display, and shutdown.

Replace direct `handle_text_input()` calls with the application dispatcher.
The selected author comes from the application. On a session change, do not
retain references, spans, pointers, generation projections, or default-agent
information from the old controller.

The event source and notifier stay process-long. Provider events are drained
only from the current controller; commit is allowed only while the old one is
idle, so no in-flight old event may cross the boundary.

### 7.3 Add explicit session-view reset

Extend the testable `SessionView` boundary with an explicit operation for a
successful session replacement. In the curses implementation it resets:

- `TranscriptRenderPlanner` watermark;
- transcript pad contents and rendered tail coordinates;
- transcript column/cache state as needed;
- `TranscriptViewport` offset and follow state;
- rendered generation projection;
- old input target derivation;
- any active application overlay.

The next render must be a full build from the new controller's restored
transcript. A failed switch calls no reset and leaves the existing transcript,
viewport, and editor usable. A successful `/open` or `/create` clears the
editor through `input_consumed`; `/iam` and list commands follow their semantic
result without modifying transcript state.

### 7.4 Add transient list overlay presentation

Introduce a presentation-only overlay value with a title, ordered rows,
annotations, scroll offset, and empty-state text. The application result
provides raw public rows; the TUI applies normal terminal sanitization while
rendering.

Overlay behavior must include:

- one row per name or command;
- visible ambiguous/corrupt session annotations;
- clipping and scrolling for lists taller than the terminal;
- Page Up/Page Down and arrow scrolling without moving the transcript
  viewport;
- Escape dismissal back to the unchanged chat;
- correct resize relayout while open;
- dismissal on a successful session switch;
- no insertion into the transcript pad or session journal.

One-line notices continue to use the status area. `/help` uses the same overlay
and remains available during generation.

### 7.5 Preserve input and cancellation behavior

While generation is active:

- Escape and interrupt retain the current explicit stop/exit behavior;
- `/help` renders immediately;
- the seven other application commands produce the generation-in-progress
  notice and do not mutate catalogs or selections.

Typing ordinary characters clears stale notices under the current policy.
Decide overlay key routing before editor routing so scrolling a visible overlay
does not edit the prompt accidentally.

Explicit shutdown must call through the application owner. The outer exception
path still restores the terminal and preserves the original failure if cleanup
also fails.

### 7.6 Update TUI tests

Extend the fake `SessionView` and TUI state-machine tests for:

- direct startup state without selector input;
- current controller/default-agent changes after navigation;
- `/iam` author selection across switches;
- successful switch clearing editor and resetting view exactly once;
- failed switch preserving controller, transcript, viewport, and editor;
- restored target transcript rendered from a clean planner;
- list/help overlay rows, empty states, scrolling, resize, and dismissal;
- lists never appearing in transcript entries;
- application-command behavior during generation;
- terminal failure and shutdown behavior after at least one switch.

Keep pure render-planner and screen-layout tests separate from coordinator tests
so failures identify the correct layer.

### Block 6 verification

```sh
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

### Block 6 exit criteria

- The TUI enters Welcome without rendering a selector.
- Every application command works from every current TUI session.
- Successful switching completely resets transcript-bound view state.
- Failed switching leaves the old chat and editor usable.
- Lists and help use a scrollable transient overlay and never enter SQLite.
- Stop, resize, terminal restoration, and notifier handling still work.

## 8. Block 7 — cleanup, migration, documentation, and release verification

### Objective

Remove obsolete selection code only after both frontends no longer reference
it, finish the checked-in workspace and documentation migration, and verify the
entire design across unit, process, integration, and sanitizer builds.

### Design references

Read Sections 2 through 3, 10 through 11, 14 through 18, and use all of Section
16 as the final verification checklist.

### Primary files

- `src/ui/tui/startup_selector.h`
- `src/ui/tui/startup_selector.cpp`
- obsolete selector-specific TUI test files
- `src/ui/console/console_startup.h`
- `src/ui/console/console_startup.cpp`
- `CMakeLists.txt`
- `Makefile`
- `README.md`
- `src/README.md`
- `src/apps/README.md`
- `src/session/README.md`
- `src/agents/README.md`
- `src/ui/README.md`
- `src/ui/text/README.md`
- `src/ui/console/README.md`
- `src/ui/tui/README.md`
- relevant web README text about inherited provider configuration
- checked-in `workspace/`
- all test fixtures and integration harnesses

### 8.1 Remove obsolete code and stale build entries

Delete `StartupSelector` production files and selector-only tests after an
`rg` search proves no caller remains. Remove their CMake entries. Remove console
listing helpers and old selection-opening helpers that have no nonterminal
caller. Retain only the simplified argument parser and whole-workspace check
support needed by `console_main.cpp`.

Search for and remove stale concepts:

```text
--persona
--forum
--session
--new
--list-forums
--list-sessions
StartupSelector
select_persona
select_forum
select_session
```

Some strings may remain intentionally in historical design documents or tests
that assert removed flags fail. Review each match rather than blindly deleting
all text.

Ensure new `application/`, utility, generated documentation, source, and test
files are present in the correct CMake libraries and test targets. `cha_core`
or a dedicated application library may own application code, but dependency
direction must remain:

```text
frontends -> application -> session/agents/util
web -> existing session/text path
```

No application source may link back to curses or console libraries.

### 8.2 Finish checked-in workspace migration

Update `workspace/app.toml` with a valid global `[provider]` while retaining the
top-level web bind settings and logging table. Remove redundant connection
values from character/forum files where inheritance is intended. Preserve
intentional overrides.

Add concise `description` values to the sample Reader persona, all sample
character definitions, and sample forums so Assistant's inventory is useful.
Do not create workspace directories for Guest, Assistant, Entrance, or Welcome.

Remove checked-in databases or lock artifacts accidentally produced by manual
testing. Preserve intentional durable sample sessions only if the repository
already treats them as fixtures.

Update `Makefile` so `run-console` launches `chacon` without removed selection
flags. Keep commands workspace-root-relative as before.

### 8.3 Update user and architecture documentation

Document:

- immediate Guest / Entrance / Welcome startup in both terminal variants;
- the complete slash-command table and quoted-name examples;
- forum-member discovery through `/members <forum>`;
- public-name-only behavior and ASCII-folded lookup;
- transient Welcome versus persistent created sessions;
- persistent Entrance session location at a conceptual level without exposing
  private keys as command syntax;
- new `app.toml` `[provider]` inheritance and the separate web bind fields;
- optional description metadata;
- argument-free whole-workspace `chacon --check`;
- removed console flags and migration examples;
- the application/session/frontend ownership boundary;
- web scope: inherited provider settings only, no terminal built-in workflow.

Keep `docs/application-guide.md` canonical for the documentation embedded in
Assistant. README material should link to or summarize it without creating a
second separately maintained command reference.

### 8.4 Run automated verification across every build configuration

Earlier blocks already run the default configuration's full suite. This step
adds the configurations they do not cover: the standalone integration binary,
the console-only build, and the sanitizer presets.

Run the normal TUI-capable build and all discovered tests:

```sh
cmake --preset ninja
cmake --build --preset ninja
ctest --test-dir build/ninja -j8 --output-on-failure
```

Run the standalone integration executable from the checked-in workspace:

```sh
cd workspace
../build/ninja/itest
```

Return to the repository root, then verify the console-only build so no TUI
dependency leaked into shared code:

```sh
cmake --preset console
cmake --build --preset console
ctest --test-dir build/console --output-on-failure
```

Run the existing sanitizer presets:

```sh
cmake --preset console-asan-ubsan
cmake --build --preset console-asan-ubsan
ctest --test-dir build/console-asan-ubsan --output-on-failure

cmake --preset console-tsan
cmake --build --preset console-tsan
ctest --test-dir build/console-tsan --output-on-failure
```

If a sanitizer limitation is pre-existing, document it with the exact failing
test and evidence. Do not globally suppress a new race in catalog creation,
controller switching, or temp cleanup.

### 8.5 Run end-to-end behavioral checks

Use deterministic test-provider configuration or the process mock provider to
verify both frontends:

1. Start with no selection input and confirm Guest / Entrance / Welcome /
   Assistant.
2. Ask Assistant an application question and receive a documented answer.
3. Run `/forums`, `/personas`, `/sessions Entrance`, `/members Entrance`, and
   `/help`; confirm Entrance, Guest, and Welcome are omitted from their
   discovery listings, Assistant is Entrance's sole member, and no output
   enters the transcript.
4. Use quoted multi-word names with `/iam`, `/create`, and `/open`.
5. Switch from Welcome to a workspace session, then directly to another forum
   and session, then back to `Entrance Welcome`.
6. Confirm the same-run Welcome transcript returns.
7. Restart and confirm Welcome is empty while the created persistent session
   remains.
8. Run TUI and console concurrently and confirm their Welcome transcripts and
   leases are independent.
9. Race two creators of the same folded name and confirm exactly one success.
10. Force post-publication provider construction failure, then fix it and open
    the retained session.

### 8.6 Audit public output and filesystem safety

Search tests and production presentation code for accidental output of forum
IDs, persona IDs, session stems, database paths, or lock paths. It is acceptable
for diagnostic logs intended for developers to contain safe paths where the
existing logging policy permits them; terminal command output must not.

Audit all cleanup code:

- only the exact owned Welcome temp directory is recursively removed;
- no startup path deletes a workspace SQLite database;
- catalog lock files are never unlinked;
- published sessions are never deleted as failed-open rollback;
- all private path components pass the existing confinement validators.

Finish with:

```sh
git diff --check
git status --short
```

Review untracked files and build artifacts without staging or deleting user
work that is outside this implementation.

### Block 7 exit criteria

- No obsolete startup selector or console selection/listing path remains.
- All source, tests, fixtures, build files, and documentation use the new
  workflow.
- Normal, console-only, integration, and sanitizer verification has passed or
  any pre-existing environmental limitation is precisely documented.
- Every Section 16 design test is represented by an automated test or an
  explicitly recorded end-to-end check.
- No user-visible terminal output exposes a private entity key or storage path.
- Welcome cleanup and published-session failure handling pass the safety audit.

## 9. Final implementation completion checklist

The feature is complete only when every statement below is true:

- TUI and console start directly as Guest in Entrance / Welcome with Assistant.
- Guest, Assistant, and Entrance require no workspace entity files.
- Welcome is a normal SQLite-backed session that is fresh per process and
  restorable only within that process.
- Assistant has embedded version-matched documentation, immutable workspace
  inventory, the effective persona roster, Entrance instructions, and normal
  forum/shared-history context.
- `[provider]` supplies the bottom configuration layer to Assistant, workspace
  agents, and web workspace agents.
- One immutable effective persona roster object is shared by every terminal
  controller opened during the run.
- `/iam`, `/open`, `/create`, `/forums`, `/sessions`, `/members`, `/personas`,
  and `/help` implement the approved quoting, listing, generation, and
  diagnostic rules.
- `/open` remains available after switching to any other session.
- `/create` publishes atomically while holding both the catalog lease and its
  prepared target session lease in the approved order.
- A post-publication controller failure retains the new session and the old
  current chat.
- TUI session switching resets all transcript-bound render state and lists use
  an overlay.
- Console switching preserves append-only transcript ordering and uses the
  notice stream for application output.
- `chacon --check` validates the whole workspace without constructing a
  provider or session.
- Built-ins are excluded by provenance from discovery lists but remain valid
  explicit command targets.
- Existing key-based web routes and stored sessions remain compatible.
- Documentation and sample workspace configuration teach only public names and
  the new startup flow.
