# Web-plan coding-agent harness

## Purpose

The harness executes the implementation blocks in [web-plan.md](web-plan.md)
with three coding agents. Codex implements each block, Grok performs the first
independent review, Pi performs the second independent review, and Codex repairs
the findings after each review.

The harness is deliberately outside the repository at `~/var/cha`. Agents edit
the repository worktree, while prompts, reviews, diffs, build logs, and run
metadata remain outside it. The harness never commits, resets, stashes, or
checks out repository files.

## Directory layout

```text
~/var/cha/
├── README.md
├── run-block.sh
├── block01-session-lease/
│   ├── block.sh
│   ├── latest -> runs/<run-id>
│   └── runs/
│       └── <run-id>/
├── block02-concurrent-controllers/
│   └── block.sh
├── ...
└── block14-platform-audit/
    └── block.sh
```

There is one block directory for every implementation block in
[web-plan.md](web-plan.md). Each `block.sh` is a small executable wrapper that
sets four block-specific values:

- The block number.
- The block name.
- The corresponding section of `web-plan.md`.
- Any additional CTest labels required by that block.

Every wrapper then sources `~/var/cha/run-block.sh`. Agent configuration,
preflight checks, prompts, snapshots, review stages, test execution, and failure
handling are centralized in that shared runner.

## Recommended workflow

Blocks depend on earlier blocks and should normally be run in numerical order.
Use one clean commit per completed block.

First run the preflight without starting an agent:

```bash
cd ~/var/cha/block01-session-lease
CHECK_ONLY=1 ./block.sh
```

When the preflight succeeds, start the block:

```bash
./block.sh
```

After the script exits successfully:

1. Inspect the worktree and the artifacts under `latest/`.
2. Review `changes-final.diff` and `build-final.log`.
3. Run any additional verification appropriate to the change.
4. Commit the block in the repository.
5. Start the next block from the resulting clean worktree.

Do not run two block scripts concurrently against the same worktree. The agents
inside one invocation are serialized, but the harness does not acquire a
cross-process worktree lock.

## Execution sequence

One invocation follows this sequence:

| Phase | Actor | Action | Principal output |
|---|---|---|---|
| 1 | Codex | Implements the bounded plan block and its focused tests | `01-implement.log` |
| Check | Harness | Snapshots the change and independently builds/tests it | `changes-after-implement.diff`, `build-after-implement.log` |
| 2 | Grok | Reviews the complete diff, specification, and build log | `review-grok.md` |
| 3 | Codex | Repairs every genuine Grok finding | `03-fix-grok.log` |
| Check | Harness | Snapshots and independently builds/tests the repaired tree | `changes-after-grok-fixes.diff`, `build-after-grok-fixes.log` |
| 4 | Pi | Checks the Grok findings and performs a fresh review | `review-pi.md` |
| 5 | Codex | Repairs every genuine Pi finding | `05-fix-pi.log` |
| Gate | Harness | Snapshots, then runs the final required build and tests | `changes-after-pi-fixes.diff`, `build-final.log`, `changes-final.diff` |

The two intermediate build/test checks are diagnostic. A failure is recorded
and passed to the next reviewer so the following repair stage can address it.
The final build/test execution is a hard gate: any failure makes the harness
exit nonzero. The implementation and final snapshots must also be nonempty, so
an unchanged baseline cannot be accepted as a completed block.

Each Codex repair prompt explicitly says that a review finding is not
automatically correct. Codex must reject findings that contradict
[web-design.md](web-design.md) or exceed the current block's scope, and explain
the rejection in its stage log.

## Specification supplied to the agents

The implementation prompt requires Codex to read, in order:

1. Sections 1 and 2 of `web-plan.md` for the global execution rules.
2. The current block's section and its complete “Read first” list.
3. `web-design.md`, which is authoritative when the design and plan differ.
4. [src/README.md](../src/README.md) for dependency boundaries.
5. The current contents of every file it intends to change.

Both reviewers receive the relevant plan section, the authoritative design,
the full diff including new files, and the latest independently captured build
log. Pi additionally receives the Grok review and must determine whether each
earlier finding was fixed, incorrectly fixed, or justifiably rejected.

The prompts keep the agents within the current block. Work from a later block
must not be implemented merely to make a future interface convenient.

## Preflight

Preflight runs before a run directory is created or an agent is started. It
checks:

- `git`, `cmake`, `ctest`, `codex`, `grok`, and GNU `timeout` are available.
- Pi can be resolved and started with a compatible Node runtime.
- `REPO` names the root of a valid Git worktree with a valid `HEAD`.
- `docs/web-plan.md` and `docs/web-design.md` exist.
- The CMake `console` configure preset exists.
- The worktree is clean unless `ALLOW_DIRTY=1` was explicitly supplied.
- Codex is authenticated.
- Grok is authenticated and the configured Grok model is available.
- Pi can start and the configured Pi model is in its model catalog.
- The block's `latest` path is absent or is a symlink, so it cannot overwrite a
  real file or directory.

Set `CHECK_ONLY=1` to exit immediately after these checks:

```bash
CHECK_ONLY=1 ./block.sh
```

Preflight validates authentication and model discovery without validating that
an account has enough remaining quota for the complete run. Provider, network,
or quota failures later in a run are ordinary stage failures and are captured
by the failure handler.

## Agent configuration and permissions

### Codex

Codex defaults to `gpt-5.6-terra` with `medium` reasoning effort. It runs with:

```text
approval policy: never
sandbox: danger-full-access
configuration validation: strict
session persistence: ephemeral
terminal color: disabled
```

Codex can read the review artifacts and modify the repository worktree. The
default `danger-full-access` mode avoids a dependency on bubblewrap and does not
restrict Codex commands to the worktree. Those commands have the same host
permissions as the user running the harness. Approval is `never`, so Codex does
not stop to ask for command permission. Use a dedicated worktree and run the
harness only with credentials and filesystem access appropriate for autonomous
execution.

`CODEX_SANDBOX=workspace-write` restores filesystem sandboxing on a host where
the Codex bubblewrap sandbox is known to work. It is not the default because a
sandbox setup failure can otherwise prevent every Codex tool command from
starting.

### Grok

Grok defaults to `grok-4.5` with `high` reasoning effort. It runs in plan
permission mode with only the `Read`, `Grep`, and `Glob` tools. Subagents,
cross-session memory, and web access are disabled. Grok can inspect the
repository and harness artifacts but cannot execute shell commands or modify
files.

### Pi

Pi defaults to `openrouter/moonshotai/kimi-k3` with `high` thinking. It runs
without session persistence, extensions, skills, prompt templates, or context
file discovery. Its tool allowlist is `read`, `grep`, `find`, and `ls`, so it
cannot execute shell commands or modify files.

The Pi launcher uses `#!/usr/bin/env node`. The harness first uses `PI_BIN` or
the `pi` found on `PATH`; if neither resolves, it searches
`~/.local/share/pi-node/node-*/bin/pi`. It prepends the launcher's own `bin`
directory to `PATH` when invoking Pi so the launcher uses its adjacent Node
runtime instead of an incompatible system Node.

Pi's `--approve` option trusts project-local files for the invocation. It does
not expand the read-only tool allowlist.

## Baseline and snapshots

Every diff is relative to the state that existed when the harness started, not
merely to the repository's current `HEAD`.

For the normal clean-worktree case, the baseline is `HEAD`. If the operator
explicitly sets `ALLOW_DIRTY=1`, the runner creates a synthetic baseline commit
object containing the complete starting worktree:

- Tracked changes are included.
- Untracked, non-ignored files are included.
- Untracked Git-ignored files are excluded; already tracked files remain
  included.
- The real Git index, branch, stash, and worktree are not modified.

The synthetic object is not attached to a branch or tag and may eventually be
removed by Git object pruning. Its object ID is recorded in `baseline.txt` and
`metadata.txt` for as long as the object remains available.

Snapshots use a private temporary Git index. This makes newly created files
appear as additions in the diff without staging anything in the repository's
real index. Every snapshot also records the corresponding `git status --short`
output.

`ALLOW_DIRTY=1` is an attribution mechanism, not a backup or rollback feature.
Use it only when preserving pre-existing work in place is intentional.

## Builds and tests

Every test pass starts with:

```bash
cmake --preset console
cmake --build --preset console
ctest --test-dir build/console --output-on-failure \
    -LE "web_process|web_stress"
```

Focused labels are added by the block wrappers:

| Blocks | Additional CTest labels |
|---|---|
| 1–11 | None |
| 12 | `web_process` |
| 13 | `web_process`, `web_stress` |
| 14 | `web_process`, `web_stress` |

The same commands are included in the Codex prompts and are executed again by
the harness after implementation, after the Grok repair, and at the final gate.

Block 14 also requires the available platform and sanitizer verification
described by `web-plan.md`. The shared runner cannot create unavailable macOS or
Windows runners and does not guess project-specific sanitizer presets. Codex
must execute the locally available checks and document every platform or
sanitizer that was not exercised, as required by the plan.

## Run artifacts

Artifacts are written to:

```text
<block-directory>/runs/<UTC timestamp>-<process-id>/
```

The block directory's `latest` symlink points to the most recently started run.
Prior runs are retained and are not overwritten.

A complete successful run normally contains:

| Artifact | Contents |
|---|---|
| `metadata.txt` | Block, repository, baseline, versions, models, reasoning settings, and focused labels |
| `baseline.txt` | Baseline Git object ID |
| `status-initial.txt` | Worktree status at the start |
| `01-implement.log` | Captured Codex implementation-stage output |
| `01-implement.final.txt` | Final Codex response used for stage-completion validation |
| `changes-after-implement.diff` | Diff after initial implementation |
| `build-after-implement.log` | Independent build/test output after implementation |
| `review-grok.md` | Grok's Markdown review |
| `02-review-grok.log` | Grok diagnostic output |
| `03-fix-grok.log` | Captured first Codex repair-stage output |
| `03-fix-grok.final.txt` | Final Codex response used for stage-completion validation |
| `changes-after-grok-fixes.diff` | Diff after the first repair |
| `build-after-grok-fixes.log` | Independent build/test output supplied to Pi |
| `review-pi.md` | Pi's Markdown review |
| `04-review-pi.log` | Pi diagnostic output |
| `05-fix-pi.log` | Captured second Codex repair-stage output |
| `05-fix-pi.final.txt` | Final Codex response used for stage-completion validation |
| `changes-after-pi-fixes.diff` | Diff preserved before the final gate |
| `build-final.log` | Final gating build/test output |
| `changes-final.diff` | Successful final diff against the original baseline |
| `status.txt` | Status from the most recent snapshot |
| `result.txt` | Final numeric exit status and `success` or `failure` |

Each named diff also has a sibling `.status` file containing the status at that
snapshot.

Review output must be nonempty. An empty `review-grok.md` or `review-pi.md` is
treated as a stage failure rather than as a successful review with no findings.
A reviewer that finds no defects should say so plainly.

Each Codex prompt requires a completion marker in the separately captured final
response. The marker means Codex could inspect the workspace, perform the
requested implementation or repair work, and attempt the requested validation.
It is deliberately absent when Codex reports an environment, permission,
authentication, quota, or tool blocker. A missing response or marker fails the
stage even if the Codex process itself exits with status zero. An empty initial
or final diff also fails the run.

## Failure and interruption behavior

The runner uses Bash strict mode (`set -euo pipefail`). A failed agent command,
timeout, missing Codex completion marker, empty implementation or final diff,
empty review, or final build/test failure causes a nonzero exit.

Agent time limits default to:

- Codex: 120 minutes per invocation.
- Grok: 60 minutes.
- Pi: 60 minutes.

`SIGINT` exits with status 130 and `SIGTERM` exits with status 143. On any
failure after the baseline has been established, the exit handler attempts to
write:

- `changes-on-failure.diff` with the worktree state at failure time.
- `changes-on-failure.status` and the current `status.txt`.
- `failure-snapshot.log` with any snapshot diagnostics.
- `result.txt` with the nonzero exit status and `result=failure`.

Temporary Git indexes are removed on exit. Repository changes are intentionally
left in place for inspection and manual repair. The harness does not roll back,
resume, or automatically retry a failed stage.

## Configuration overrides

Configuration is supplied through environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `REPO` | `/home/mpopov/projects/cha` | Repository or dedicated Git worktree root |
| `RUN_ROOT` | `<block>/runs` | Parent directory for per-run artifacts |
| `CODEX_MODEL` | `gpt-5.6-terra` | Codex implementation/repair model |
| `CODEX_EFFORT` | `medium` | Codex reasoning effort |
| `CODEX_SANDBOX` | `danger-full-access` | Codex command sandbox (`danger-full-access` or `workspace-write`) |
| `GROK_MODEL` | `grok-4.5` | First-review model |
| `GROK_EFFORT` | `high` | Grok reasoning effort |
| `PI_BIN` | resolved automatically | Pi executable name or path |
| `PI_PROVIDER` | `openrouter` | Pi provider |
| `PI_MODEL` | `moonshotai/kimi-k3` | Second-review model |
| `PI_THINKING` | `high` | Pi thinking level |
| `CODEX_TIMEOUT` | `120m` | Timeout for each Codex stage |
| `GROK_TIMEOUT` | `60m` | Timeout for the Grok stage |
| `PI_TIMEOUT` | `60m` | Timeout for the Pi stage |
| `ALLOW_DIRTY` | `0` | Permit and baseline a dirty worktree when set to `1` |
| `CHECK_ONLY` | `0` | Run preflight and exit when set to `1` |

Examples:

```bash
# Use a dedicated worktree and store artifacts elsewhere.
REPO=/work/cha-web RUN_ROOT=/work/cha-runs ./block.sh

# Give complex Codex stages more reasoning and time.
CODEX_EFFORT=high CODEX_TIMEOUT=180m ./block.sh

# Use Codex filesystem sandboxing on a host with working bubblewrap support.
CODEX_SANDBOX=workspace-write ./block.sh

# Select another configured Pi model.
PI_PROVIDER=openrouter PI_MODEL=anthropic/claude-sonnet-4.6 ./block.sh
```

Model overrides must identify models available to the corresponding account.
Preflight rejects unavailable Grok and Pi model names. Codex validates its model
when the first Codex stage starts.

## Adding or changing a block wrapper

A wrapper contains no execution logic. Its shape is:

```bash
#!/usr/bin/env bash
set -euo pipefail

BLOCK="Block 12"
TITLE="Complete server composition and bounded process shutdown"
PLAN_SECTION="15"
FOCUSED_TEST_LABELS=(web_process)

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/run-block.sh"
```

Keep the block name and section synchronized with `web-plan.md`. Add a focused
CTest label only when the block creates or changes tests carrying that label.
Changes to agent policy, prompts, artifacts, or execution flow belong in the
shared runner rather than in an individual wrapper.

## Operational boundaries

- The harness modifies the selected worktree. Use a dedicated Git worktree when
  isolation from day-to-day development is important.
- It does not enforce block dependency completion. The operator is responsible
  for running blocks in order and committing successful results.
- It does not lock the worktree. Never point concurrent harness runs at the same
  worktree.
- It does not commit, push, open pull requests, or communicate externally beyond
  the configured model-provider calls.
- It does not automatically run unavailable platform CI or invent sanitizer
  configurations.
- Logs contain source excerpts, diffs, prompts, and model responses. Store and
  share the artifact directory accordingly.
- A successful exit means the configured local test gate passed; it does not
  replace human review or unconfigured platform verification.

## Troubleshooting

If preflight reports a dirty worktree, inspect `git status`, then commit or stash
the changes. Use `ALLOW_DIRTY=1` only when the existing changes are intentional
and must remain in the baseline.

If Pi is installed but cannot start, run:

```bash
PI_BIN=/absolute/path/to/pi CHECK_ONLY=1 ./block.sh
```

The runner will place that launcher's directory first on `PATH` so an adjacent
Node binary is selected.

If an agent times out, inspect `latest/result.txt`, its stage log, and
`latest/changes-on-failure.diff`. Increase only the relevant timeout before
starting a new run. A new invocation creates a new run directory; it does not
resume the interrupted conversation.

If a Codex stage reports a bubblewrap error such as `Failed RTM_NEWADDR`, use
the default `danger-full-access` mode. Check that `CODEX_SANDBOX` is not exported
as `workspace-write`, then start a new run. A missing completion marker now
turns this condition into a harness failure instead of allowing reviews and
baseline tests to continue.

If an intermediate build fails but the run continues, inspect the corresponding
build log together with the following review and repair log. This continuation
is intentional. Only the final build/test execution is the hard gate.
