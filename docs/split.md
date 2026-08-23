# Splitting provider transport out of `agents/`

## Status

This document is an implementation plan, not a design change. Every step below
is a relocation or a rename. No type, signature, control flow, or behavior
changes, and no test assertion changes. If a step requires editing logic, the
plan is wrong and should be corrected before continuing.

Revision note: an earlier draft of this plan understated the include churn,
proposed a dependency rule the tree already violates, omitted the relocated
tests' own includes, and gave a verification command that cannot pass. Those
are corrected below.

Post-implementation note: a follow-up renamed `provider_response.*` to
`chat_completions_api.*`, moved the Chat Completions encoder beside its
decoders, and moved the protocol-neutral streaming decoder seam into
`model_backend.h`. A second follow-up moved the now-orphaned
`generation_event.h` from `agents/` to `providers/`. The steps below retain the
original names and locations because they describe the source tree and
mechanical rename sequence at the time this split was performed.

## Motivation

`agents/` currently holds two unrelated responsibilities: loading and
projecting character/provider configuration, and performing HTTP against a
provider. Its own README is titled "Provider execution", which describes only
the second half. The directory name describes neither, and there is no
"agent" in the codebase.

The layering inside `agents/` is already acyclic at file level. This change
buys a directory name that matches its contents and two enforceable dependency
rules. It does not fix a bug.

## Why the obvious cut fails

The intuitive split is "provider things" versus "character things". That
produces a cycle at the directory level:

- `CharacterDefinition` embeds a `ProviderSelection`, so `character.h` needs
  the provider configuration types.
- `ProviderClient` embeds a `SharedCharacterDefinition`, so the transport
  needs `character.h`. This is required by the design: a request owns its
  immutable character snapshot and `prepare()` reads its `system_prompt`.

If `ModelBackendConfig` and `ProviderSelection` move into `providers/`, then
`agents/character.h` includes `providers/provider_config.h` while
`providers/provider_client.h` includes `agents/character.h`. The individual
files remain a DAG, but the two directories include each other and the
boundary means nothing.

The cut must therefore be drawn at **transport**, not at "provider versus
character". Provider *configuration* stays with the definitions; only code
that performs or decodes HTTP moves.

## Target layout

| Stays in `agents/` | Moves to `providers/` |
| --- | --- |
| `character.{h,cpp}` | `model_backend.h` |
| `character_config.{h,cpp}` | `provider_client.{h,cpp}` |
| `model_context.{h,cpp}` | `provider_response.{h,cpp}` |
| `generation_event.h` | `responses_api.{h,cpp}` |
| | `sse_framer.{h,cpp}` |
| | `providers.{h,cpp}` |

Eleven production files move, seven stay, and five test files move.

Edges introduced by the split, all pointing `providers/` -> `agents/`:

```text
providers/model_backend.h      -> agents/model_context.h, agents/generation_event.h
providers/provider_response.h  -> agents/character_config.h
providers/responses_api.h      -> agents/character_config.h, agents/model_context.h
providers/provider_client.h    -> agents/character.h
providers/providers.h          -> (providers/ only)
```

Nothing in `agents/` includes anything in `providers/`. The seven staying
files have zero dependencies on the eleven moving ones today, so this holds by
construction.

That covers only the `agents/`/`providers/` pair. The tree has one further
edge that breaks the wider layering — see P0.

## Prerequisites

### P0 — Move `FullSessionId` into `chat/` (required)

`src/agents/model_context.h:5` formerly included
`session/session_identity.h`. It was the only `agents/` -> `session/`,
`workspace/`, or `web/` edge in the tree and created this directory cycle:

```text
agents  -> session   (model_context.h needs FullSessionId)
session -> agents    (session_controller.h needs character.h)
```

After the split this becomes `session -> providers -> agents -> session`. The
files stay acyclic, but any README rule saying `agents/` must not depend on
`session/` would be false the day it is written. Without P0 the split still
compiles and the `agents/`/`providers/` boundary still holds, but the wider
architectural claim does not.

This is not a new rule. `chat/README.md` already states that `chat/` owns
"stable forum, character, and session IDs" and is "a leaf. Everything else may
depend on it; it depends on nothing in the project." `FullSessionId` is
eleven lines of exactly that vocabulary and already depends only on
`chat/ids.h`.

The implementation was three steps:

1. Create `chat/session_identity.h` holding `FullSessionId` alone.
2. Repoint consumers to `chat/session_identity.h`.
3. Remove the obsolete session-layer identity header.

### P1 — Move `character_runtime_info` and `provider_endpoint` (recommended)

Both are declared in `agents/provider_client.h:21-22` and defined in
`agents/provider_client.cpp:507-530`. Neither touches curl, `ModelBackend`, or
transport state; they are pure projections over `CharacterDefinition` and
`ModelBackendConfig` sitting in the curl transport file.

This is a genuine cleanup but it is **not** a prerequisite for the split, and
it does not prevent any cycle. `session/session_controller.h` holds a
`Providers&` and a `vector<shared_ptr<ProviderRequest>>`, so `session/`
depends on the transport directory by design regardless of where these two
functions live. Do P1 because the functions are misfiled, not because the
boundary needs it.

The two functions want different homes:

| Function | Destination | Reason |
| --- | --- | --- |
| `character_runtime_info()` | `agents/character.{h,cpp}` | Projects a `CharacterDefinition`; belongs beside `CharacterRuntimeInfo`. |
| `provider_endpoint()` | `agents/character_config.{h,cpp}` (or `provider_config.*` if P2 is done) | Builds a URL from `ModelBackendConfig`; it is provider-configuration projection, not character behavior. |

Note that `character_runtime_info()` calls `provider_endpoint()`, so
`character.cpp` needs `character_config.h`. It already includes it directly at
line 5, so no new include is required.

Call sites:

- `src/session/session_controller.cpp:117` — the only production caller of
  `character_runtime_info()`. Already reaches `agents/character.h`
  transitively; no include edit needed.
- `src/agents/provider_client.cpp:636` — a second production caller of
  `provider_endpoint()`, used to build the request URL. After the move this
  file (in `providers/`) calls into `agents/character_config.h`. That is a
  legal `providers/` -> `agents/` edge; add the direct include rather than
  relying on the transitive one through `character.h`.
- `tests/application/unit_workspace_definition.cpp:3` — change
  `#include "agents/provider_client.h"` to `#include "agents/character.h"`.
- `tests/agents/unit_provider_client.cpp` — four `character_runtime_info()`
  assertions (lines 192, 239, 804, 843). Relocating them to
  `unit_character_definition_loader.cpp` is optional and can be deferred.

### P2 — Split `character_config.*` (defer; not recommended for this change)

The header does divide cleanly and one-way — the character half depends on the
provider half, never the reverse:

| `provider_config.{h,cpp}` | `character_config.{h,cpp}` |
| --- | --- |
| `Mode`, `ReasoningFormat`, `ProviderApi` | `CharacterConfigPaths` |
| `WebSearchMode`, `CacheRetention` | `LoadedCharacterConfig` |
| `default_provider_api`, `default_web_search_mode` | `load_character_config` |
| `ModelBackendConfig`, `ProviderSelection` | `load_character_metadata` |
| `ProviderConfig`, `make_backend_config` | `styles_directory`, `load_named_style` |
| `validate_provider_selection` | `prompt_scope_table` |
| `is_direct_openai_host`, `load_named_provider` | |
| `providers_directory` | |

But it is not needed to establish the transport boundary, and under the
project's simplicity rule it should not ride along with a directory move.
**Skip it for the initial split.** Wherever this document says
`provider_config.h`, read `character_config.h`.

If it is done later, one correction to an earlier draft of this plan: only
`read_optional` is shared between the halves. `read_choice` is used solely by
`read_style_config` (lines 104-109) and stays with the character half. Share
just the one template — do not introduce a general `toml_read.h` parsing
header for it.

Both halves stay in `agents/` either way. `provider_config.*` does **not**
move to `providers/` — see "Why the obvious cut fails".

## Steps

### 1. Create the directory and move files

```bash
mkdir -p src/providers
git mv src/agents/model_backend.h src/agents/provider_client.h src/agents/provider_client.cpp src/agents/provider_response.h src/agents/provider_response.cpp src/agents/responses_api.h src/agents/responses_api.cpp src/agents/sse_framer.h src/agents/sse_framer.cpp src/agents/providers.h src/agents/providers.cpp src/providers/
```

Use `git mv` so history follows. Reviewers should see renames, not
delete-plus-add.

### 2. Update includes inside the moved production files — 13 lines

Only includes of *moving* headers change. Includes of staying headers
(`agents/character.h`, `agents/character_config.h`, `agents/model_context.h`,
`agents/generation_event.h`) are already correct and must be left alone.

| File | Change |
| --- | --- |
| `providers/provider_client.h` | `agents/model_backend.h` -> `providers/` |
| `providers/provider_response.h` | `agents/model_backend.h`, `agents/sse_framer.h` -> `providers/` |
| `providers/responses_api.h` | `agents/model_backend.h`, `agents/provider_response.h` -> `providers/` |
| `providers/providers.h` | `agents/provider_client.h` -> `providers/` |
| `providers/provider_client.cpp` | `agents/provider_client.h`, `agents/provider_response.h`, `agents/responses_api.h` -> `providers/` |
| `providers/provider_response.cpp` | `agents/provider_response.h` -> `providers/` |
| `providers/responses_api.cpp` | `agents/responses_api.h` -> `providers/` |
| `providers/sse_framer.cpp` | `agents/sse_framer.h` -> `providers/` |
| `providers/providers.cpp` | `agents/providers.h` -> `providers/` |

Two moved files need no edit and must be left alone: `model_backend.h`
includes only `agents/model_context.h` and `agents/generation_event.h`, both
of which stay, and `sse_framer.h` includes nothing from `agents/`.

### 3. Update external includes — 12 lines in 12 files

This is the complete external consumer set. The moved headers have no others.

`agents/providers.h` -> `providers/providers.h`:

- `src/session/session_controller.h`
- `src/web_main.cpp`
- `src/workspace/session_open.cpp`
- `tests/application/unit_session_open.cpp`
- `tests/integration/integration_test.cpp`
- `tests/support/test_web_graph.h`

`agents/model_backend.h` -> `providers/model_backend.h`:

- `tests/session/unit_concurrent_controllers.cpp`
- `tests/session/unit_session_controller.cpp`
- `tests/support/test_backends.h`
- `tests/support/test_generations.h`
- `tests/support/test_live_session.h`
- `tests/web/unit_text_input.cpp`

`agents/provider_client.h` has one external consumer,
`tests/application/unit_workspace_definition.cpp`. If P1 is done first it
becomes `agents/character.h` there and needs no further edit; if P1 is
skipped, repoint it to `providers/provider_client.h` and the count for this
step is 13 lines in 13 files.

`provider_response.h`, `responses_api.h`, and `sse_framer.h` have no consumers
outside the moving set and the relocated tests in step 4.

### 4. Relocate tests and fix their includes — 5 lines

```bash
mkdir -p tests/providers
git mv tests/agents/unit_providers.cpp tests/agents/unit_provider_client.cpp tests/agents/unit_provider_response.cpp tests/agents/unit_responses_api.cpp tests/agents/unit_sse_framer.cpp tests/providers/
```

Each of these five files opens with an include of the header it tests, and
each must be repointed. Missing this is a compile failure:

| File | Line 1 |
| --- | --- |
| `tests/providers/unit_providers.cpp` | `agents/providers.h` -> `providers/providers.h` |
| `tests/providers/unit_provider_client.cpp` | `agents/provider_client.h` -> `providers/provider_client.h` |
| `tests/providers/unit_provider_response.cpp` | `agents/provider_response.h` -> `providers/provider_response.h` |
| `tests/providers/unit_responses_api.cpp` | `agents/responses_api.h` -> `providers/responses_api.h` |
| `tests/providers/unit_sse_framer.cpp` | `agents/sse_framer.h` -> `providers/sse_framer.h` |

`tests/agents/` keeps `unit_model_context.cpp`,
`unit_character_definition_loader.cpp`, and `unit_config_loader.cpp`.

### Include accounting

| Bucket | Lines |
| --- | --- |
| Inside moved production files (step 2) | 13 |
| External consumers (step 3, after P1) | 12 |
| Inside relocated tests (step 4) | 5 |
| **Total for the split** | **30** |

P0 adds one edit plus a new header. P1's edits are listed in its own section
and are not counted here.

### 5. Update `CMakeLists.txt`

In the `cha_core` source list (currently lines 177-184), repoint five entries
and leave three:

```cmake
    src/agents/character.cpp          # unchanged
    src/agents/model_context.cpp      # unchanged
    src/agents/character_config.cpp   # unchanged
    src/providers/providers.cpp
    src/providers/provider_client.cpp
    src/providers/provider_response.cpp
    src/providers/responses_api.cpp
    src/providers/sse_framer.cpp
```

In `CHA_TEST_SOURCES` (currently lines 291-297), repoint four and leave three:

```cmake
        tests/agents/unit_model_context.cpp                 # unchanged
        tests/agents/unit_character_definition_loader.cpp   # unchanged
        tests/agents/unit_config_loader.cpp                 # unchanged
        tests/providers/unit_providers.cpp
        tests/providers/unit_provider_response.cpp
        tests/providers/unit_responses_api.cpp
        tests/providers/unit_sse_framer.cpp
```

In the `if(NOT WIN32)` block (currently line 315), repoint the POSIX-only
transport test:

```cmake
            tests/providers/unit_provider_client.cpp
```

### 6. Write `src/providers/README.md`

Follow the existing per-directory format, ending with an explicit rule as
`workspace/README.md` does:

> This directory may depend on `agents/`, `chat/`, and `util/`.
> It must not depend on `session/`, `workspace/`, `web/`, or executable wiring.

Move the transport, registry, cancellation, shutdown, and diagnostics sections
out of the current `agents/README.md`. Its "Ownership" section and most of its
"Source map" describe the moved files and belong here.

### 7. Rewrite `src/agents/README.md`

What remains is character definitions, character and provider configuration
loading, prompt assembly, and model-history projection. Retitle it — it is no
longer "Provider execution". Keep the "Character and provider input" section.
Add:

> This directory may depend on `chat/` and `util/`.
> It must not depend on `providers/`, `session/`, `workspace/`, or `web/`.

The second line is only true once P0 is done. Do not write it otherwise.

### 8. Update `src/README.md`

Three places:

- Line 13, dependency shape: `cha_core -> workspace / providers / agents / chat / session / util`
- Line 42, directory table: narrow the `agents/` row to configuration,
  definitions, and model context; add a `providers/` row for transport,
  request execution, cancellation, and event delivery.
- Line 109, contracts list: add a link to `providers/README.md`.

### 9. Fix `docs/tutorial.md`

Already stale independently of this split. It documents `GenerationBatch`,
`GenerationExecutor`, the session `ThreadPool`, and the shared start gate —
all deleted on the `simple_providers` branch — and five of its links point at
files that no longer exist:

```text
src/agents/generation_batch.h
src/agents/generation_executor.h
src/util/thread_pool.h
src/util/thread_pool.cpp
tests/agents/unit_generation_batch.cpp
```

It also has 19 `agents/` path references this split invalidates further.
Prefer correcting the stale architecture content first, on its own, so the
split commit stays a pure relocation.

### 10. Naming

After the split, `agents/` holds character definitions, configuration loading,
and model-history projection. "agents" still describes none of that.
`characters/` is the closest fit, though `model_context` sits slightly oddly
under it.

Renaming is a second mechanical pass over roughly 30 more include lines. Do it
as a separate commit, or accept the name. Do not fold it into the split.

## Verification

### Boundary rules

Both directions, and the full rule rather than just the `agents/`/`providers/`
pair. `rg` is not installed in this environment; these use `grep`.

Must print nothing (requires P0):

```bash
grep -rnE '#include "(providers|session|workspace|web)/' src/agents/
```

Must print nothing:

```bash
grep -rnE '#include "(session|workspace|web)/' src/providers/
```

### Build and test

```bash
make build && make test
```

Then the suites the provider work is covered by, per `docs/design.md`:

```bash
cmake --preset tsan && cmake --build --preset tsan && ctest --test-dir build/tsan --output-on-failure
```

```bash
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan && ctest --test-dir build/asan-ubsan --output-on-failure
```

Baseline before starting: **667 tests, all passing** (verified on
`simple_providers` at the time of writing).

Because the change is pure relocation, the bar is exact equality — the same
667 tests pass, and no test is added, removed, renamed, or edited beyond its
`#include` lines.

### Shape of the diff

An earlier draft proposed requiring an empty
`git diff --stat <base> | grep -v '=>'`. That cannot pass: the twelve external
consumers in step 3 receive ordinary include-line edits and are not renames.

Use instead:

```bash
git diff --name-status --find-renames <base>
```

Then check by hand that:

- the 11 moved production files and 5 moved tests appear as `R` renames;
- every `M` entry is one of the 12 external consumers from step 3, plus
  `CMakeLists.txt` and the README/doc files from steps 6-9;
- no other file appears at all.

Any `M` outside that list means a step went wrong.

## Commit sequencing

1. **P0** — `FullSessionId` into `chat/`. Small, independently correct,
   required before the `agents/` rule in step 7 can be written truthfully.
2. **P1** — `character_runtime_info()` to `character.*`,
   `provider_endpoint()` to `character_config.*`. Independent cleanup.
3. **Tutorial correction** — remove the deleted-architecture content from
   `docs/tutorial.md`.
4. **The split** — steps 1 through 8. Pure `git mv` plus include-path and
   build-file edits. No logic changes.
5. **The rename**, if wanted — step 10.

P2 is deliberately absent; see its section.

Do not merge these into one commit. Commits 1-3 above change content; commit 4
changes only locations. Combining them produces a diff where a reviewer cannot
tell which is which, and that is exactly the review this change needs to pass.

Land all of this after the `simple_providers` branch merges. Mixing a
relocation of this size into a branch that already carries a behavioral
redesign would make both harder to review and harder to revert independently.
