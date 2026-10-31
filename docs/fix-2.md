# F2: Close the runtime test-hook finding

Status: Verification plan. The grouping is already implemented in `3eb183ff`
(`Log host pump failures and group runtime test hooks`).

## Finding and scope

F2 reported that test choreography lived among the production fields of
`RuntimeSettings`. `src/runtime/runtime_settings.h` now groups the ten worker
callbacks in `RuntimeSettings::TestHooks`. The commit updates their accesses to
`settings.test_hooks`; it does not change callback ownership or execution order.
A missed old member access fails compilation. Every access is in `src/app`
(built into `cha_lib`), `tests/app`, or `tests/bridge`, so the two test binaries
below compile all of them.

Confirm that the grouping remains present, then build and run the existing
suites. No hook inventory, capture-lifetime audit, helper, or new test is needed
for this mechanical change. Preserve existing concurrency and fault-injection
coverage.

The `force_next_*` fault injectors and `Application::*_for_tests` setters are
unchanged and outside this task. Their tests span several binaries; the two
binaries below validate the affected grouping, not every test-only seam.

## Validation and closure

Run from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja --target cha_app_tests cha_bridge_tests
./build/ninja/cha_app_tests
./build/ninja/cha_bridge_tests
```

Close F2 when the grouping is present and these checks pass. A passing run of
these commands on the same commit, with no local source changes, can be reused.
Fix only a demonstrated regression; this task can finish without code changes.

Report the result briefly in the final chat response or commit message. Do not
create a closure document or edit other files in `docs/` during implementation
unless the user explicitly requests it.
