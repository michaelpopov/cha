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
  `cha-daemon`, the SCGI server behind nginx that serves the ChaWeb API, with
  systemd on Linux and `scripts/run_daemon.py` on macOS.
- [ChaWeb guide](chaweb.md): browser text/voice conversations, R2 vault
  actions, recovery, and the current `/api/cha/v1/` API.

The desktop application has no HTTP listener. Older HTTP/SSE server
instructions and the removed `chaweb` server executable are not part of the
current operating workflow. The current ChaWeb is a static browser application
that nginx serves next to `cha-daemon`.

[Users and shared sessions](users.md) is an archived, unimplemented proposal.
[The older UI review](webui.review.txt) is historical review material. Neither
is a description of the current application or an active implementation plan.

[Assistant configuration maintenance](assistant.md) and its
[implementation plan](ass-plan.md) are unimplemented proposals. They do not
provide configuration tools in the current application.
