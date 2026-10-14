# CHA documentation

- [User manual](UserManual.html): macOS installation, sessions, recipient detection,
  search, voice, settings, and vaults.
- [Workspace maintainer guide](MaintainerGuide.md): configuration files,
  validation, native import/export, and database maintenance.
- [Codebase tutorial](tutorial.md) ([HTML](tutorial.html)): shared session runtime,
  ownership, storage, generation, native bridge, frontend state, and tests.
- [ChaWeb guide](chaweb.md): browser text/voice conversations, R2 vault
  actions, recovery, deployment, and the current `/api/cha/v1/` API.
- [Jev research proposal](jev.md): research decisions and recipient evaluation.

The desktop application has no HTTP listener. Older HTTP/SSE server
instructions and the removed `chaweb` server executable are not part of the
current operating workflow. The current ChaWeb is a static browser application
that nginx serves next to `cha-daemon`.

[The older UI review](webui.review.txt) is historical review material, not a
description of the current application or an active implementation plan.
