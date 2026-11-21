# CHA documentation

- [User manual](UserManual.md) ([HTML](UserManual.html)): daily use of
  conversations, notes, voice, the Library, and the ChaWeb browser client.
- [Administrator manual](AdminManual.md) ([HTML](AdminManual.html)):
  installation, settings, providers, voices, vaults, R2 and D1 transfer, the
  Assistant, diagnostics, and ChaWeb deployment.
- [Workspace maintainer guide](MaintainerGuide.md): configuration files,
  validation, native import/export, and database maintenance.
- [Codebase tutorial](tutorial.md) ([HTML](tutorial.html)): shared session runtime,
  ownership, storage, generation, native bridge, frontend state, and tests.
- [ChaWeb guide](chaweb.md): browser text/voice conversations, recovery,
  deployment, and the current `/api/cha/v1/` API. ChaWeb changes or refreshes
  its vault with the Welcome commands `/open <name>`, `/download`, and
  `/download confirm`; the administrator manual describes them.
- [Jev research proposal](jev.md): research decisions and recipient evaluation.

The desktop application has no HTTP listener. Older HTTP/SSE server
instructions and the removed `chaweb` server executable are not part of the
current operating workflow. The current ChaWeb is a static browser application
that nginx serves next to `cha-daemon`.

[The older UI review](webui.review.txt) is historical review material, not a
description of the current application or an active implementation plan.
