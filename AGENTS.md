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

### Apply KISS before implementation

- Separate the user's required behavior from proposed implementation details.
  Do not turn a convenient mechanism into an additional requirement.
- First look for an existing code path or source of data that can solve the
  problem. Extend or use it before adding a separate subsystem.
- Before adding state, identify the concrete requirement that cannot be met
  without it. Do not add a second store, buffer, cache, or index for data that
  is already available unless a demonstrated need requires it.
- Do not invent expiry periods, timers, background work, automatic resets,
  or other lifecycle policies. Add them only when the requested behavior or
  correctness requires them.
- Judge simplicity by the state, dependencies, execution paths, and cleanup
  rules a maintainer must understand, not only by the number of lines changed.
- Recheck repository plans and design documents against these rules before
  implementing them. They do not justify unnecessary machinery. Follow the
  user's explicit requirements; use the simplest design that satisfies them.
- Before finishing, remove machinery that the solution no longer needs,
  including tests and contracts that exist only to support removed behavior.
  Passing tests do not make an unnecessary design appropriate.

Do not update documentation in the docs/ directory unless this is explicitly requested by the user.

## UI approval

Before making any UI change, present a visual representation of the proposed
change to the user. Wait for the user's explicit approval before implementing
it. A request for functionality does not itself approve a UI design. Apply
this rule to layout, controls, labels, styling, and other visible changes.

Do not add title labels or legends to the top of panels in screen designs.

Do not add nonessential explanatory labels or helper text that merely narrates
obvious screen or form behavior. Keep UI copy limited to control labels,
actionable state, validation, and errors.

Do not block an operation that can complete successfully because configuration
contains unused or obsolete settings. Ignore those settings and write warning
log messages instead.
