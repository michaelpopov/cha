# CHA documentation

- [User manual](UserManual.html): macOS installation, sessions, recipient detection,
  search, voice, settings, and vaults.
- [Conversation guide](for_user.html): personas, characters, forums, and sessions.
- [Workspace maintainer guide](MaintainerGuide.md): configuration files,
  validation, native import/export, and database maintenance.
- [Codebase tutorial](tutorial.md) ([HTML](tutorial.html)): shared session runtime,
  ownership, storage, generation, native bridge, frontend state, and tests.
- [Editing workspace entities](editing.md): implementing an editing workflow
  across the store, application, bridge, and frontend.
- [Headless daemon design](headless.md) and [tutorial](head-tutorial.html):
  `cha-daemon`, an OpenAI-compatible SCGI server behind nginx, with systemd on
  Linux and `scripts/run_daemon.py` on macOS.

CHA is a native desktop application. Older HTTP/SSE server instructions and the
removed `chaweb` executable are not part of the current operating workflow.

[Users and shared sessions](users.md) is an archived, unimplemented proposal.
[The older UI review](webui.review.txt) is historical review material. Neither
is a description of the current application or an active implementation plan.
