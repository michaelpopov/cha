# Vaults implementation plan

Implement vaults in four sequential blocks. Give a fresh Codex session the
repository and the corresponding block file. Each file includes the relevant
behavior, prerequisite code, implementation work, tests, and completion
criteria; previous conversations and block files are not required.

| Order | Implementation brief | Result |
| --- | --- | --- |
| 1 | [block1.md](block1.md) | Configuration directory, vault registry, selected-vault startup and maintenance, application-wide credentials, and updated code consumers and fixtures. |
| 2 | [block2.md](block2.md) | In-process runtime switching, retargetable resources, selection persistence, and runtime integration tests. |
| 3 | [block3.md](block3.md) | Vault identity in the protocol, switch endpoint, browser checks, and sidebar selector. |
| 4 | [block4.md](block4.md) | Native application integration, packaged configuration, launchers, migration documentation, and final acceptance checks. |

The configuration-directory layout and other product decisions in
[vaults-design.md](vaults-design.md) remain unchanged. This reorganization does
not adopt the proposed single-file configuration alternative, remove selection
persistence, or change the mirror pre-check policy. The block files correct
implementation ordering and call out lock/API constraints that the previous
step-by-step pseudocode overlooked.

## How to execute a block

1. Read the repository's `AGENTS.md`/`CLAUDE.md` and the block file. Inspect the
   working tree and preserve unrelated changes.
2. Verify the block's prerequisites in code. Earlier blocks must already be
   implemented; do not assume that earlier session output is available.
3. Implement only that block, including dependent fixtures and checks. Keep
   solutions small and obvious. Smaller commits within a block are fine;
   there is no prescribed number of commits or automatic commit requirement.
4. Run the specified verification. Record pre-existing failures and unavailable
   toolchains separately from regressions; do not describe an unrun check as
   passing.
5. Finish with a short handoff: implemented behavior, relevant interface
   changes, checks and results, and remaining platform checks. The repository
   is the source of truth for the next session.

Each block must leave its affected build and test suites working. Blocks 1–3
use the updated test harness or an explicitly supplied configuration directory;
packaged launchers, native first-run setup, and migration instructions are
completed in block 4. Those intermediate commits are not release candidates.

## Coverage of the former plan

| Former steps | New owner |
| --- | --- |
| 0: Baseline | Block 1, with focused baseline checks at the start of later blocks. |
| 1: Shared TOML rewrite helper | Block 2, together with its new persistence caller. |
| 2: Configuration directory and registry | Block 1, including every C++ consumer and fixture. |
| 3: OAuth and environment | Block 1, with environment loading before runtime construction. |
| 4: Retarget operations | Block 2, together with the switch operation that uses them. |
| 5: Identity and current vault | Current-vault ownership in block 1; protocol, serializers, browser validation, and fixtures together in block 3. |
| 6: Switch operation and route | Runtime operation and direct integration tests in block 2; HTTP route, schema, and HTTP tests in block 3. |
| 7: Browser selector | Block 3. |
| 8: macOS application | C++ bridge compatibility and dynamic capability getter in block 1; Swift, packaging, and native verification in block 4. |
| 9: Packaging and launchers | Block 4; test-server configuration generation moves with the parser in block 1. |
| 10: Documentation | Block 4. |

The final block contains the full acceptance checklist. Each earlier block also
lists the checks that must pass before handing off.
