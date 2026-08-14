# CLAUDE.md

## Keep it simple

**This is very important.** CHA is a personal, toy-like application. It is not an
enterprise-grade program.

Every proposed code change and design solution must strive for simplicity,
maintainability and readability. Do not overcomplicate it.

- Prefer the smallest change that solves the actual problem.
- Prefer a plain, obvious solution over a general or clever one.
- Do not add abstraction, configuration, or machinery for hypothetical needs.
- When two designs both work, pick the one that is easier to read later.

## Local repository investigation

A local repository-search assistant is available as:

```bash
local-investigate "question"
```

Use it to reduce repository exploration and context consumption before doing broad manual searches or reading many files.

`local-investigate` is best for focused retrieval tasks such as:

- finding definitions, references, assignments, or call sites;
- locating code associated with an error message, option, symbol, or behavior;
- identifying likely implementation files;
- finding relevant tests;
- filtering a large set of repository matches down to useful source locations.

Examples:

```bash
local-investigate "Find definitions and call sites of common_fit_params."

local-investigate "Find where n_gpu_layers is assigned. Exclude tests and build artifacts."

local-investigate "Find the implementation that emits the error string 'failed to load model'."

local-investigate "Find the most relevant source locations for server_task and briefly state what each one does."
```

Prefer `local-investigate` when the alternative would require multiple repository-wide searches or reading substantial source material into the main model's context.

Keep requests focused. The tool is a retrieval and context-reduction assistant, not a replacement for primary reasoning. Do not ask it to independently solve complex architectural or debugging problems when the main model can reason from retrieved evidence.

Treat its output as evidence, not authority. It returns compact `file:line` references intended for follow-up inspection. Verify important or surprising conclusions directly from the referenced source before making code changes.

For very simple lookups where one obvious `rg` command is sufficient, use `rg` directly instead of invoking `local-investigate`.

