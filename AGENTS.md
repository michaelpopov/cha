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

Do not update documentation in the docs/ directory unless this is explicitly requested by the user.

Do not add title labels or legends to the top of panels in screen designs.

Do not add nonessential explanatory labels or helper text that merely narrates
obvious screen or form behavior. Keep UI copy limited to control labels,
actionable state, validation, and errors.

Do not block an operation that can complete successfully because configuration
contains unused or obsolete settings. Ignore those settings and write warning
log messages instead.
