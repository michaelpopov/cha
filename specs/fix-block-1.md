# Fix block 1: Quick safety

**Fixes:** F1, F3, F4 · **Written:** November 19, 2026

## Before you start

- This file is complete. Do not read `docs/fix-plan.md`, the architecture
  reviews in `docs/`, or `specs/p0-d1-design.md`.
- **Start condition:** none. Start from the latest `main`.
- Other work (P0-D1, a D1 sync rework) is in progress. Do not change D1 sync
  code (`src/app/d1_sync.cpp`, `src/storage/sync_*.cpp`).
- File paths and line numbers come from commit `11ca9039`. Lines can move.
  Find the code by the function name.

## Approvals

- **ChaWeb permission:** all three fixes change `cha_lib` code that cha-daemon
  also runs. Before you change code, ask the user for explicit permission. If
  the user does not give it, stop. Run ChaWeb tests only with this permission.
- **UI approval:** none. No visible text changes.

## Rules

- Make the smallest fix. Do not add machinery.
- Add only the tests that this file names. Run the tests of the changed area.
- Do not change `docs/` or `specs/`. The document updates are a separate task.
- Do not commit unless the user tells you to.

## F1. Finite time limit for note requests

**Decision (user, November 19):** one finite overall limit of 7200 s for all
note requests, with or without Flex.

**Problem:** `prepare_note_provider` sets `timeout_s = 0` for every note
request: Compressed, L1, L2, L3 sections, L3 overview, and Save summary. It
does this also when Flex is off. The idle timer starts only at the first body
byte. A server that keeps the connection open and sends nothing holds the
single summary batch slot. A new Summarize then reports that summarization is
already running. Only a window reload, a vault switch, or quit frees the slot.
TCP keepalive does not help, because the connection stays alive.

**Fix:**

1. In `prepare_note_provider` (`src/notes/notes_operations.cpp:103`), replace
   `timeout_s = 0` with 7200 s. `CURLOPT_TIMEOUT` also limits the streamed
   body, so the value must cover a long L3 overview.
2. Do not change the idle timer in `ProviderClient`. Keep cancellation as it
   is.

This changes the Flex design rule that keeps an unlimited first-byte wait.
Do not edit `specs/flex-design.md`.

**Test:** No new test. If the existing timeout test
(`tests/bridge/unit_bridge_router.cpp:4783`) expects the unlimited value,
change its expectation.

## F3. D1 token in `Redactor`

**Problem:** `Redactor` collects the provider keys, the R2 secret, and the
OAuth secrets. It does not collect the D1 API token. The Assistant log and
Event tools send their text to the model. No code logs the token today, so
there is no known leak.

**Fix:** Add `d1_storage()->api_token` to the secret list
(`src/app/assistant_service.cpp:180-211`).

**Test:** Add a dummy D1 token to the existing redaction test
(`tests/app/unit_assistant_service.cpp:1120`).

## F4. Base path in the credential destination check

**Problem:** The saved-key guard compares the provider's scheme, host, and
port. It ignores the base path. On a shared gateway that routes by path, a
`base_path` change can send a saved key to a different service on the same
host.

**Fix:** Add the base path to the destination string in
`credential_destination_pairs` (`src/workspace/workspace_config_store.cpp:1283,
1339`; the guard is at `2055`).

**Test:** One test: a `base_path` change with a saved key is refused.

## Not in this block

- Do not redact `host_config_read`. That breaks `host_config_write`.
- Do not change the Welcome web tools (`web_search`, `web_read`). They are
  required.
- Do not add limits for picture fetch or `web_read` to local network
  addresses. The user decided: no change.

## Done when

- F1, F3, and F4 are in the code.
- The F3 and F4 tests pass, and the tests of the changed areas pass.
- Your final report says, for each fix, what changed and which tests ran.
