# F5: Close host failure logging and harden delivery serialization

Status: Verification and small hardening plan based on `3eb183ff`.
The original logging fix is already implemented; validation is pending.

## Findings and decisions

`packaging/host/runtime_bridge.cpp` calls `log_worker_failure` from the
ordinary-task, pump, and delivery catch blocks. It reports standard exception
text or an unknown error and suppresses consecutive repeated failures. The
worker loops retain their existing continuation behavior.

The shipped macOS and Windows hosts use one connection at a time and close it
before opening its replacement (`prepareDocumentReplacement` in
`packaging/macos/native_bridge.swift` and `replace_document_connection` in
`packaging/windows/main.cpp`), so one delivery-suppression flag is sufficient for
that use. No multi-connection suppression work is needed.

A separate hardening opportunity exists at `batch->dump()` in `run_pump`.
Strict JSON serialization throws on invalid UTF-8. Here it is outside the catch,
so that exception would escape the worker thread. A normal-input path producing
such a batch has not been established; do not describe this as a reproduced
application crash.

Use UTF-8 replacement for this final delivery serialization, consistent with
existing diagnostic serialization in `assistant_service.cpp`, `logging.cpp`, and
`voice_websocket.cpp`. This is a bounded one-line change with no new state.
`util/json_serialization.h` is not a replacement-policy helper: it converts this
failure to another exception and is not suitable for this boundary.

## Work

1. Confirm by source inspection that the three catch sites call
   `log_worker_failure` and retain their success resets and continuation paths.
   Keep the helper private. Do not add a header, move it to another namespace,
   inject worker failures, or test independent local booleans.
2. In `run_pump`, replace the final strict serialization with:

   ```cpp
   const std::string json = batch->dump(
       -1, ' ', false, nlohmann::json::error_handler_t::replace);
   ```

   This replaces invalid UTF-8 in delivered strings without altering stored data.
   Do not merely move `dump()` into the existing catch: `take_delivery` has
   already set `connection->in_flight`, so dropping that batch would prevent the
   acknowledgement needed for subsequent delivery. Do not add delivery retries
   or change acknowledgement ownership.
3. Build the native target and run the existing native runtime tests. Their
   coverage includes first flow, connection close/reopen, stale contexts, and
   shutdown. No new test seam or helper-only test suite is needed.

The UTF-8 policy change does not contain every possible host-thread exception.
Keep allocation failures and unrelated callback behavior outside this task.

## Validation and closure

Run from the repository root:

```sh
cmake --preset ninja
cmake --build --preset ninja --target cha_native_runtime_tests
./build/ninja/cha_native_runtime_tests
```

On macOS also run `cmake --build --preset ninja --target cha_macos_runtime` to
check the actual host library. Do not claim an unrun platform build as verified.

Close the original F5 logging finding after source inspection and the existing
native tests pass. Include the replacement-policy change in the same small task.
The native fixture normally disables logging, so test passage is evidence of
runtime regression coverage, not a direct assertion that an error line was
emitted or that invalid native data reached the pump.

Report the result and that limitation briefly in the final chat response or
commit message. Do not create a separate review record or edit other `docs/`
files during implementation unless the user explicitly requests it.
