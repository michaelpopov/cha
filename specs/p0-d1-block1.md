# P0 D1 implementation block 1 — Containment and forum rows

**November 19, 2026** · Block 1 of 3

## Start here

Implement this block in its own Codex chat against the current branch. Read [the revised D1 design](p0-d1-design.md), especially §§2, 3, and 6–7, and [AGENTS.md](../AGENTS.md). The revised design is the implementation contract. It retains note paths and turn positions and uses a mixed-version queue transition. Those changes supersede the earlier implementation outline in the design chat.

The completion criteria are a Desktop worker that preserves all D1 queues when capacity is exhausted and a Desktop config transaction that creates its `forums` rows before a peer can use them. The current Desktop wire format still runs at this checkpoint. Block 2 implements the durable retry protocol.

The design calls for a live REST preflight. Use the procedure in [block 2](p0-d1-block2.md#preflight) and carry the results forward so the checks run once. If disposable D1 access is unavailable, proceed with the local containment and forum changes. Capacity failure must preserve the queues whether or not the service accepts acknowledgement at capacity.

## Implement

1. **Keep the queues on a capacity failure.** Trace the `full_database` branch in `D1SyncWorker::send_pass`. For Desktop, keep the failed outbox row, stop the send pass, and allow the receive pass to run so it can try to free space through ordinary acknowledgement. Retain the existing cadence and final-send behavior. Keep the missing-table path distinct from capacity failure. Limit the change to the Desktop path at the existing host boundary. Remove eviction helpers only when their last permitted caller is gone. [Worker loop and send](../src/app/d1_sync.cpp#L98) · [Production worker wiring](../src/app/application.cpp#L408)
2. **Create relational forum rows inside the config transaction.** Reuse the candidate's forum IDs and the current idempotent `INSERT ... ON CONFLICT DO NOTHING`. Put the row insertion on the Desktop commit path that saves the config and enqueues `config.edit`. Keep it independent of the `surviving_forums` pruning condition, which still governs remote removal. Reuse the insertion on Desktop workspace open and reopen before the first receive pass, to repair older vaults. If the existing startup sequence already guarantees that repair, use it and verify it through the Desktop open path. Keep the current snapshot publication order and fault handling. Use the existing host context to limit any changed behavior to Desktop. [Settings entry point](../src/app/application.cpp#L1193) · [Workspace load](../src/workspace/workspace_config_store.cpp#L1524) · [Workspace commit](../src/workspace/workspace_config_store.cpp#L1583)
3. **Keep diagnostics within the current UI contract.** Keep the existing log and missing-table message. Coordinate new visible D1 Event wording with block 2 and the approval rule in the design. This block needs no new screen controls.

## Daemon permission boundary

Design §6 records the planned D1 shutdown for `cha-daemon`, with explicit implementation permission still required under [AGENTS.md](../AGENTS.md). Proceed with the Desktop containment and forum work independently. Keep the current daemon behavior and test expectations unless that permission is present in the implementation chat. If permission is granted, §6 is the next containment step before the v2 protocol is enabled. Use the existing host mode to suppress worker creation, sync machine propagation to record producers, and the Welcome inbox-clear request. Verify the existing daemon D1 test is updated to cover the new behavior, including no remote requests or local sync records and the warning when credentials are saved. Leave unrelated daemon behavior alone. Treat any real D1 queue retirement as a deployment action owned by the operator.

## Verification

- Reuse the existing worker capacity fixture. Cover a full-database response with the outgoing row still present, no Desktop `DROP TABLE`, and an incoming record still reaching acknowledgement. If the old eviction fixture still protects a permitted legacy host mode, keep that contract and add the Desktop case to the same fixture. Otherwise replace the obsolete expectation. [Current eviction regression fixture](../tests/storage/unit_sync_chat.cpp#L1120)
- Exercise Desktop Settings forum creation followed by a peer chat before the creator opens any local chat in the forum. The existing config-then-chat fixture covers the receiving machine and does not exercise the creator. Add one focused application case if needed to cover the creator. Use the existing config fault injection to check that a failed commit leaves the forum row, config, and outbox unchanged, and use an existing open fixture for repair of a missing row. [D1 application tests](../tests/app/unit_d1_sync.cpp#L435) · [Workspace fault tests](../tests/workspace/unit_workspace_config_store.cpp#L2044)
- Rebuild the focused native/core targets, for example `cmake --build --preset ninja --target cha_tests cha_app_tests` on the current macOS setup. Run the affected cases by exact GoogleTest filter, using the names in the finished diff. The shared CTest labels are broader than this block. Run the permitted Desktop and storage cases. Keep new tests limited to behavior absent from the current fixtures, with no coverage percentage target.

## Handoff

Finish with the containment changes buildable and the focused checks passing. Report the branch or worktree, the starting and ending revision or working-tree state, the files changed, and the exact checks run. State whether the daemon step was authorized and completed. The next chat should use this result and [block 2](p0-d1-block2.md). Keep unverified live D1 behavior visible for block 2's preflight.
