# F6: Refresh settings without cancelling active answers

Status: Small implementation plan based on source evidence at `3eb183ff`.
The regression test below must verify the behavior; it has not been run.

## Evidence and intended behavior

`SessionController::definition_for` reads the current workspace for each new
generation and creates an immutable character definition. `start_generation`
passes that definition to a request-owned provider client, built by
`shared_openai_provider_factory` in `src/app/application.cpp`. Existing requests
retain their definitions; later requests can read changed settings.

`ApplicationSettings.ProviderEditsRefreshSessionOnlyChoicesAndRestartRestoresOverrides`
already verifies that provider name/reasoning edits leave a session running when
that provider is selected only through a session override. Workspace-selected
providers instead trigger `invalidate_affected_sessions` for the same kind of
edit. This selection-dependent restart is unnecessary based on the current
ownership model and should be removed with a focused regression test.

Provider and character-setting edits should refresh presentation, let the active
answer finish with its original request configuration, and apply updated settings
to the next request, respecting existing session overrides. Use existing refresh
paths. Do not add a settings-diff framework, deferred edits, or permanent tests
that assert the restart behavior being removed.

## Changes

1. In `settings::update_provider` in `src/app/settings_operations.cpp`, remove
   `invalidate_affected_sessions` and keep `refresh_all_sessions`. Keep the store
   update and validation; discard its now-unused edit result explicitly. The
   all-session refresh already covers providers selected through session overrides.
2. In `workspace::update_character_settings` in
   `src/app/workspace_operations.cpp`, remove the `restart` variable and call
   `apply_edit(..., false)` whenever a field changed. Fold the old restart
   comparisons into `changed`, preserving provider presence/identity, reasoning,
   web search, style, voice, and web-search-tool comparisons. Preserve validation
   and the no-change path.
3. Add the focused application regression below. Keep the existing appearance,
   session-override, immutable-request, and persistence tests.

Do not change the default `apply_edit` restart policy or other callers. Entity
removal, membership changes, instruction/file edits, vault changes, Stop, and
shutdown retain their current behavior. No UI or schema changes are needed.

## One paused-answer regression

Use one application test with two small variants and shared setup, covering both
modified call sites. Reuse `MockHttpServer`, `provider_body`, and the existing
subscription helpers in `tests/app/unit_settings_operations.cpp` or
`tests/app/unit_application.cpp`.

- Provider variant: change the model on the provider selected by a workspace
  character.
- Character variant: switch that character to another already-configured provider
  with a different model, pointing at the same local mock server.

For each variant:

1. Open and subscribe to a session. Start the mock server with two successful
   responses, pause before response 1, submit a prompt, and wait for request 1
   with `wait_for_requests`. Capture the active request ID and session handle.
2. Apply the edit while the response is paused. Check that the same session and
   subscription remain active, with the same generating request ID and no
   reloading/terminal state. Acknowledge output as the existing tests do.
3. Resume the response. Verify one complete answer, a running session, and no
   replacement session or interrupted answer.
4. Submit another prompt. After the server has finished and been joined, inspect
   its captured request bodies: the first uses the old model and the second uses
   the new model. Do not read its request vector while the server can mutate it.

A provider-only test would miss the second changed call site; the second variant
closes that gap without the previous eight-case investigation. Use bounded waits
and release paused responses on failure. No paid provider calls, new test hooks,
temporary production experiments, or Assistant-path comparison are required.

If the test reveals an actual dependency on restarting, address that dependency
or retain restart for the affected operation and explain why. Do not weaken the
assertions or add a broad investigation process in place of the small fix.

## Validation and closure

Run from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja --target cha_app_tests cha_bridge_tests
./build/ninja/cha_app_tests
./build/ninja/cha_bridge_tests
```

Close F6 when both settings paths preserve the active answer and the next request
uses the new model, with existing application and bridge tests passing. Existing
session-override and appearance tests must remain intact. No test asserting the
removed restart behavior should be added.

Report the change and actual test results briefly in the final chat response or
commit message. Do not create a decision table, investigation report, or other
`docs/` file during implementation unless the user explicitly requests it.
