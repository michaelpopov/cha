# Block 5: ChaWeb packaging and deployment handoff

Status: implemented. The Linux package build and the isolated systemd
install/upgrade checks are outstanding; see "Results" at the end.
This block implements plan step 10. It requires passing daemon/nginx tests,
production browser builds, and conversation/iPhone checks from blocks 1–4.
It delivers an installable Linux package and tested upgrade instructions.
The final section is the operator handoff for plan steps 11–12, performed on
the private server after the isolated package checks pass.

This document contains the requirements for this block, including the
deployment and acceptance procedure. Background:
[chaweb.md](chaweb.md) and [chaweb-plan.md](chaweb-plan.md).

## Deployment model

nginx serves a shared static ChaWeb build from `$CHA_DEPLOY_PATH/chaweb`. Each
user has one private HTTPS port whose server block forwards `/api/cha/v1/`
through SCGI to a literal Unix socket such as `/run/cha/alice.sock`. systemd
owns the socket and activates `cha-daemon` using that user's existing vault.
The browser always uses the page's origin, including its port.

There is no ChaWeb API-key screen, login, credential storage, or server setting.
Ports select users; they do not authenticate them. Anyone who can reach a port
can use that user's data, so these listeners remain on the trusted private
network. Preserve the separate OpenAI listener, bearer-key mapping, and streaming
configuration. ChaWeb does not use keys printed by `add_user.sh`.

The server needs nginx and systemd, not Node or a JavaScript server. Node/npm
are build-machine dependencies. Static assets are never embedded in the daemon.
Deploy matching frontend and daemon versions together and restart affected
daemon services after replacing their binary. An old tab can require reload
after an incompatible upgrade; no service worker or API-version handshake is
part of stage 1.

## Files and package changes

| File | Work |
| --- | --- |
| `packaging/linux/package.sh` | Build the browser and include static output and the ChaWeb nginx example. |
| `packaging/linux/install.sh` | Install readable assets and the example while preserving operator configuration. |
| `packaging/linux/README.md` | Document dependencies, installation, per-user routing, checks, and upgrades. |
| `packaging/linux/nginx-chaweb.conf.example` | Use the server block delivered in block 2. |
| `packaging/linux/cha@.service`, `cha@.socket`, `add_user.sh` | Preserve existing provisioning and activation behavior. |

On the Linux build machine use the repository C++ prerequisites plus the exact
Node/npm versions pinned in `webapp/package.json`. Keep the current package's
daemon/config-helper Release build, runtime-library checks, seed-vault creation,
archive naming, and cleanup. Add these build operations:

```sh
npm --prefix webapp ci
npm --prefix webapp run build:chaweb
```

Run them from the repository directory or use its absolute path in the script.
Fail package creation if dependency installation, typechecking, the Vite build,
or required output files fail. Copy the *contents* of `webapp/dist-chaweb/` to
`chaweb/` in the archive, preserving referenced asset paths. Ship
`nginx-chaweb.conf.example` alongside existing templates and scripts. Do not
ship `node_modules`, a development server, or personal configuration.

The resulting archive should retain all existing files and add:

```text
cha-linux-<version>/
  cha-daemon
  chaweb/
    index.html
    assets/...
  nginx-chaweb.conf.example
  install.sh
  add_user.sh
  cha@.service
  cha@.socket
  nginx.conf.example
  nginx.conf.install
  cha-config.example/...
  README.md
```

The existing native frontend build/staging and macOS/Windows packages remain
unchanged. No automatic port allocator, registry, or user-management UI is
needed.

## Installer behavior

Keep the existing `CHA_DEPLOY_PATH` / `CHA_DATA_PATH` workflow, absolute-path
validation, regular installing user, sudo handling, and data ownership policy.
Validate that the extracted package contains the browser entry/assets and
ChaWeb example before installing the new files.

Install the build at `$CHA_DEPLOY_PATH/chaweb`, preserving the directory layout
used by `index.html`. Install the new example under the deployment directory
so the operator can configure listeners after removing the extracted archive.
nginx needs read access to asset files and traversal of all parent directories.
Use ordinary readable file/directory permissions for public static content;
check parent traversal explicitly, particularly under a private home directory.
Prefer a suitable static path or narrowly granted access, not relaxed vault or
configuration permissions. Keep `$CHA_DATA_PATH` outside the static root.

On reinstall, update packaged binaries/assets and the shipped example while
preserving operator-owned active nginx/systemd configuration, TLS settings,
OpenAI key maps, user vaults, and port assignments. Do not silently enable a
new public listener or overwrite an operator's per-user server blocks. Retain
the existing installer's policy for existing units and OpenAI configuration.

`add_user.sh` continues to provision the user's vault, socket, and OpenAI key.
Its generated key is unrelated to ChaWeb. Port configuration remains explicit
in nginx. Print the asset location and the required manual listener setup;
do not add new installation questions or port-registration machinery.

Serve a matching `index.html` and referenced asset set after installation.
Old hashed files need no special retention policy. Preserve the native package's
existing permissions and systemd ownership rather than changing them as a side
effect of adding static files.

## README requirements

Update `packaging/linux/README.md` so the archive alone contains usable
instructions. Include:

- Build dependencies and `make package-linux VERSION=<version>`; no Node
  requirement on the destination server.
- Static file location and why `index.html` refers to hashed JS/CSS names.
  HTML is revalidated (`no-cache`); hashed assets use immutable caching.
- Installation environment variables, regular-user execution and sudo behavior,
  nginx file access, and existing socket permissions/worker group.
- Explicit per-user private ports, literal sockets, hostname/TLS placeholders,
  and validation with `nginx -t` before reload. The enclosing nginx `http`
  configuration must load MIME types and standard SCGI parameters.
- Existing-user/new-user setup, provider settings, protected-vault password
  rules, and the exclusive-vault requirement for native configuration editing.
- Matching daemon/frontend upgrades, daemon service restart, preserved operator
  configuration, and browser reload after an incompatible change.
- The deployment and smoke checks below, plus log locations or commands useful
  when activation, static access, or the upstream socket fails.

## Isolated package verification

First run the completed implementation's checks with local/fake providers:

```sh
make test
make web-check
make itest-local
make itest-daemon
make package-linux VERSION=<version>
```

Inspect `packages/cha-linux-<version>.tar.gz`. Verify `index.html` references
files actually in the archive and that no build dependencies or personal data
were included. Use a disposable Linux VM or suitable isolated systemd
environment for install/upgrade tests, not the operator's active installation.

Test a clean install from the extracted archive without Node on the target.
Provision a test user, configure the private listener using the shipped example,
and verify nginx can traverse the static path and connect to the activated
socket. Check the default `www-data` socket group against nginx's actual worker
group. The existing socket is `/run/cha/%i.sock`, mode `0660`, with `Accept=no`;
the service runs as the installing account and opens
`$CHA_DATA_PATH/%i/config`.

Fetch `/`, referenced JS/CSS, bootstrap, and a stored conversation through the
installed paths. Confirm correct MIME/cache headers, API JSON, bodyless input/
Stop acknowledgements, and no native-host dependency. For chat use a configured
test provider; do not require paid calls for package verification.

For upgrade testing, first create stored history and edit operator-owned
nginx/systemd configuration. Reinstall a newer test package to the same paths,
restart the daemon service, and verify history/IDs, configuration edits,
OpenAI key mapping, and port assignments survive. Verify the new entry document
and all referenced assets are usable. Check an installation where parent
directory access needs attention without granting nginx access to vault data.

Block completion requires a built archive, successful clean-install/upgrade
checks, and complete README instructions. If the required isolated systemd
environment is unavailable, report that check as outstanding; packaging is not
fully verified merely because the archive exists.

## Operator deployment procedure

These steps are the subsequent private deployment handoff, not authorization
to alter the user's running server during implementation of this block.

1. Copy/extract the archive on the server with nginx and systemd installed.
   Run `install.sh` from a regular account, using absolute paths. For example:

   ```sh
   export CHA_DEPLOY_PATH=/srv/cha
   export CHA_DATA_PATH=/srv/cha-data
   ./install.sh
   "$CHA_DEPLOY_PATH/add_user.sh" alice
   ```

2. Reuse an existing user vault or provision one with `add_user.sh`. Verify
   providers, forums, characters, and credentials already work in CHA. Text UI
   has no settings editor. Stop the relevant socket/service when native editing
   needs the exclusive database lease, then restore socket activation afterward.
   Keep password files for protected vaults under existing private-file rules.
3. Confirm `cha@alice.socket` is enabled and accessible to nginx's worker group,
   and its service uses Alice's configuration. Adapt `SocketGroup` if nginx does
   not run under `www-data`, preserving mode `0660` and the existing service user.
4. Copy/adapt `$CHA_DEPLOY_PATH/nginx-chaweb.conf.example` into the operator's
   nginx configuration. Fill in private address, hostname, TLS certificate/key,
   static root, and literal user socket. For example, Alice uses port `8443`
   with `/run/cha/alice.sock`; Bob uses `8444` with `/run/cha/bob.sock`. Both
   roots point to `$CHA_DEPLOY_PATH/chaweb`. Keep the OpenAI listener separate.
5. Keep the template's buffered SCGI settings, 256 KiB request-body limit,
   JSON gzip, and API `Cache-Control: no-store` on errors and successes. The
   standard `scgi_params` forwards `CONTENT_TYPE`; do not remove JSON POST
   validation, grant CORS, or add bearer-key routing to ChaWeb.
6. Ensure the iPhone resolves the hostname, reaches its assigned private port,
   and trusts its HTTPS certificate. Validate and reload nginx:

   ```sh
   sudo nginx -t
   sudo systemctl reload nginx
   ```

7. Request `/` and `/api/cha/v1/bootstrap` through the assigned port and check
   that the correct user's forums appear. At initial setup, check the second
   user's port once for its own forums/sessions. This is the manual routing
   check; no automated multi-daemon ChaWeb fixture is required.
8. If a request fails, check nginx errors, file/parent permissions, socket
   ownership, and `systemctl status cha@alice.socket cha@alice.service` plus
   `journalctl -u cha@alice.service`. Preserve useful diagnostics without
   exposing credentials in browser errors.

For subsequent upgrades install matching frontend/daemon files from one package,
restart each affected daemon service, and recheck bootstrap and a conversation.
Keep operator configuration and ports. Reload old browser tabs after an
incompatible change. A browser reload loses unsent drafts, so finish or copy
them before planned upgrades.

## Installed-app acceptance

Run these checks through the deployed nginx/systemd path with production assets
and HTTPS. Keep test messages short; use a sufficiently long reply only for Stop.

1. Open the assigned Safari address without a key prompt. See a native forum
   combobox, title/date session rows, current-session check, and bottom New
   Session control. Entrance, Welcome, vault controls, sidebar, and Done are
   absent. No forum/session heading occupies the conversation area.
2. New Session opens a local draft and changes no stored list until Send. First
   Send returns a stored session and route; Stop is unavailable until `201`.
   Continue an older session after reload. Confirm no native bridge is needed.
3. Send ordinary text, `@Name`, `/mcast`, and `@-`; preserve CHA's forum default,
   Jev behavior, separate character entries, and Markdown. Later input returns
   `204` and snapshots provide its result. A self-note remains stored even with
   no character reply.
4. Stop a long reply and observe authoritative idle/cancelled state. Also Stop
   during input acceptance in an existing session; it may wait behind Jev or
   naming. Navigate away during generation and return to its stored progress.
5. Check draft preservation across navigation and pending acknowledgements.
   Reject first input and verify no new session remains; reject later input or
   send an oversized prompt and keep its draft with the safe error message.
6. Simulate a failed read and a lost Send response. Recover through snapshots
   or the session list without automatic resubmission. Restart the daemon on
   the same vault and recover using the same session IDs. Background Safari
   and return; current state must refresh.
7. Observe polling stop when generation is inactive; naming alone does not keep
   it running. A temporary title after a short reply may remain until ordinary
   navigation, foreground return, or another accepted turn refreshes the list.
8. On the actual iPhone test portrait/landscape, Safari browser controls,
   keyboard show/hide, and safe areas. The body must not scroll; transcript,
   list, and editor have their own scrolling. Both views and bottom controls
   remain usable in the visual viewport.
9. Toggle the full-width editor between about two lines and half the usable
   area. Keep text, caret, reading position, and open keyboard. Sessions and
   Send/Stop remain in a separate row below the text. Dictate with the keyboard
   microphone while real snapshots arrive, edit the text, then explicitly Send.
   Touch Enter adds a newline; IME/dictation never auto-submits. Also verify
   desktop Enter/Ctrl+Enter and native Enter/Space on the size button.
10. Confirm the existing OpenAI listener and bearer routing still work, while
    `/v1/` remains absent on ChaWeb ports. Use the clients sequentially: a long
    OpenAI turn can occupy the serial daemon and delay ChaWeb requests.

Record results and fix defects found during this deployment check in a focused
follow-up. Stage 1 is complete only when the packaged app passes this real
browser-to-nginx-to-daemon acceptance. Preserve stable session/entry IDs,
additive response fields, and room below the editor for future voice controls;
do not implement microphone, playback, or media APIs as part of this work.

## Results

Changed: `package.sh` runs `npm ci` and `build:chaweb` before the native build,
checks that `index.html` and every `/assets/` file it names exist, and stages
`chaweb/` and `nginx-chaweb.conf.example`. `install.sh` checks for these
files, refuses a `CHA_DATA_PATH` at or under `$CHA_DEPLOY_PATH/chaweb`,
replaces the static root with readable files, installs the example, and warns
when the nginx worker user cannot read `index.html`. `add_user.sh` names its
key as the OpenAI key and prints the ChaWeb listener step. The Linux README is
rewritten; the root README mentions the ChaWeb files.

Automated checks pass on macOS: `make test` (960 tests; the two live OpenAI
tests are skipped without credentials), `make web-check` (925 tests),
`make itest-local` (24 tests) and `make itest-daemon` (22 tests, including
static files, MIME types and cache headers through the shipped ChaWeb block).

`package.sh` ran on macOS with `uname`, `cmake` and `readelf` replaced by
stand-ins, so the npm install, ChaWeb build, output check, staging and tar
steps were real. The archive had the layout above, `index.html` named only
files in the archive, and it contained no `node_modules` or personal files.
Without npm the script stops with exit status 2. A missing asset stops the
output check. No temporary directory remained after either failure.

Outstanding, because no Linux machine or isolated systemd environment was
available:

- `make package-linux` on Linux with the real Release daemon and runtime-library
  check.
- Clean install without Node, the nginx read check, socket activation and the
  `www-data` group check, and requests through an installed listener.
- Upgrade over stored history and edited nginx/systemd configuration.
- The operator deployment and installed-app acceptance above (plan steps 11
  and 12).
