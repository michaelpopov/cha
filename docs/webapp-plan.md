# CHA web application implementation plan

> Historical migration plan. The browser-only architecture after removal of
> the earlier application variants is recorded in [removal-plan.md](removal-plan.md).

Status: plan derived from the accepted [webapp.md](webapp.md), 2026-08-06.

This plan turns the accepted proposal into ordered, verifiable work. It says what
to build, where it goes, which development tools are needed, and how each step is
proved. The proposal and UI design remain the sources for rationale and visual
detail.

## Inputs

| Document | Role in this plan |
| --- | --- |
| [webapp.md](webapp.md) | The accepted proposal. Architecture, technical decisions, packaging, and the definition of done. |
| [`../resources/cha.yaml`](../resources/cha.yaml) | The authoritative wire contract. Every request, response, status, and `error.code` the frontend may encounter. TypeScript types are generated from it. |
| [web-ui/](web-ui/README.md) | The visual and behavioral design contract. Screen catalogue, sidebar rules, and the state model in [behavior.md](web-ui/behavior.md). |
| [web-ui/mockup.html](web-ui/mockup.html) | Reference markup and CSS. Not production source: it hard-codes content and loads `lucide` and `@floating-ui` from unpkg. |

## Where this starts

The application API and session runtime are implemented and tested. `chaweb`
serves the eleven API operations in `cha.yaml`, streams session events, and answers `/` and
`/s/{forum}/{session}/` with a placeholder document:

```html
<h1>cha</h1><p>The chat browser is not installed yet.</p>
```

No production browser client exists. There is no frontend source, frontend
build, file-based static asset serving, or packaging. `chaweb` also resolves `.env`, `app.toml`, its log
path, and the workspace from its working directory, which the packaged
application cannot rely on. Nor is there anywhere to run a deployment-shaped
build: `src/resources/` and `bin/` do not exist yet, and CMake leaves the
executable in the build tree.

## How this is organized

Seven blocks, ordered by dependency. Each one leaves the repository buildable
with the full suite green, so work can stop between blocks. Blocks 1 and 2 are
C++ only; 3 through 6 are frontend; 7 is packaging and hardening.

| Block | Subject | Proposal stage |
| --- | --- | --- |
| 1 | Roots and configuration | Stage 1 |
| 2 | Static asset serving | Stage 1 |
| 3 | Frontend skeleton and build | Stage 1 |
| 4 | API client and discovery screens | Stage 2 |
| 5 | Stored sessions and URLs | Stage 3 |
| 6 | Live chat and stream recovery | Stage 4 |
| 7 | Hardening, packaging, launcher | Stage 5 |

## Fixed decisions

Inherited from [webapp.md](webapp.md). They are settled; do not reopen them
while implementing.

| Decision | Choice |
| --- | --- |
| Frontend stack | TypeScript, React, Vite. No server-rendering framework. |
| Dependencies | Local and bundled. No CDN at runtime. |
| Routing | History API and a small parser. No routing library. |
| State | React state and reducers. No global state library. |
| OpenAPI | Generate types only. The client is written by hand. |
| Asset base | Vite `base: '/'`. Never relative. |
| Shell | One document served at `/` and `/s/{forum}/{session}/`. |
| Directories | Two, always separate. The application directory holds the executable, `web/`, `app.toml`, and the launcher and is replaced by an upgrade. The workspace holds `workspace.toml`, `.env`, characters, personas, forums with their stored conversations, and `logs/`. Its path is set once at setup and may be anywhere. |
| Binding | Configurable. The shipped `app.toml` sets `0.0.0.0` so another machine on the same network can open the application. No authentication and no transport security in v1. |
| Configuration | Two files. `app.toml` in the application directory holds `host`, `port`, and the workspace path; `--config` names it and `--host`, `--port`, `--workspace` override it. `workspace.toml` in the workspace holds `[provider]` and `[logging]`. |
| Settings screen | Removed in v1, not deferred. There is no gear and no Settings view. |
| Platforms | Linux first, then macOS, then Windows. |
| Live viewers | One browser per session. A second browser is told the session is open elsewhere; it does not retry silently. |
| Markdown | Restricted subset, sanitized, no raw HTML or image fetch. |
| Attachments | Omitted. No API exists. |
| Initial browser target | Current Chrome and Edge; routine automation uses Playwright Chromium. |
| Composer target control | The omitted attachment position becomes a target-character chooser. |
| Send and Stop | The Send button becomes Stop while generation is active. |
| Build ownership | npm builds and tests browser files; CMake builds C++; the packaging script assembles a release. CMake copies, but never generates, browser files. |
| Local deployment | `bin/`, beside `src/` and `workspace/`, is a real deployment directory used for development test runs. Generated, not committed. |

## Development tools and commands

The customer does not need web-development tools. They are used only to build
and test the browser files. The current development shell already provides
Node.js 22.23.1 and npm 10.9.8. Block 3 records Node.js 22.23.1 in `.node-version`
and in `package.json` so another development machine does not silently use an
incompatible release. If either `node --version` or `npm --version` fails on a
future machine, install that recorded Node.js version before continuing.

Block 3 creates the frontend project and makes these the standard commands,
always run from `src/resources/webapp/`:

| Command | Purpose |
| --- | --- |
| `npm ci` | Install the exact dependency versions recorded in the lockfile. |
| `npm run dev` | Start the editable frontend through the Vite development server. |
| `npm run build` | Type-check and produce production files in `src/resources/webapp/dist/`. |
| `npm run check` | Regenerate/check API types and run type, unit, and component tests. |
| `npm run e2e` | Build, then run the browser tests against a deterministic local test server: once through the development server, once against the production build served by `chaweb` itself. |
| `npm run stage` | Build, then replace `bin/web/` with the result. Takes `--root <dir>` to stage somewhere else. |

Frontend dependencies are installed under `src/resources/webapp/node_modules/`,
not globally.
Runtime packages are `react`, `react-dom`, `marked`, and `dompurify`.
Development packages are `typescript`, `vite`, `@vitejs/plugin-react`,
`openapi-typescript`, `vitest`, `jsdom`, `@testing-library/react`,
`@testing-library/user-event`, and `@playwright/test`. Block 3 commits
`package-lock.json`, installs Playwright's Chromium browser for development
tests, and records all exact versions. None of these packages or the Playwright
browser is copied into the customer package.

---

## Block 1 — Roots, configuration, and listener address

**Goal.** `chaweb` resolves the program's own files and the customer's data from
two independent directories rather than from the working directory, takes its
listener settings from a configuration file or the command line, and can be
reached from another machine on the local network.

### Background

`load_application_config()` and `Workspace` already accept a root
([workspace.h](../src/session/workspace.h#L52),
[workspace.h](../src/session/workspace.h#L64)), and `load_dotenv()` already
accepts a path ([environment.h](../src/util/environment.h#L20)). Only the
entry point hardcodes `"."`.

**Two roots, not one.** The application directory holds what ships with a
release and is replaced wholesale by the next one: the executable, `web/`,
`app.toml`, and the launcher. The workspace holds everything belonging to the
customer: `workspace.toml`, `.env`, `characters/`, `personas/`, `forums/` —
which contain the stored conversations as `forums/<forum>/sessions/*.sqlite3` —
and `logs/`.
Keeping them apart is what makes an upgrade safe; a single root would put the
customer's characters, conversations, and provider key inside the directory an
upgrade overwrites. The workspace path is chosen once during setup and can be
anywhere, including a drive shared with other machines.

The listener address needs a matching change. `allowed_host_authorities()`
([http_server.cpp](../src/ui/web/http_server.cpp#L45)) derives the permitted
`Host` header values from the configured listener host, and every request is
checked against that set before routing. Setting `host = "0.0.0.0"` alone would
therefore permit only the literal `Host: 0.0.0.0:8080`, and every browser that
used a real address — including `localhost` on the same machine — would be
answered `403 forbidden_host`.

### Work

1. **Add `executable_directory()`** in `src/util/executable_path.{h,cpp}`.
   Read `/proc/self/exe` on Linux, `GetModuleFileNameW` on Windows, and
   `_NSGetExecutablePath` on macOS. Do not derive it from `argv[0]`, which the
   launcher cannot be trusted to set. Compile all three guarded implementations
   now; test the native branch on each platform build.
2. **Split the configuration in two, following the directories.** Today one
   `app.toml` in the workspace carries `host`, `port`, `[provider]`, and
   `[logging]` ([workspace.cpp](../src/session/workspace.cpp#L316)). Split it:
   - `workspace.toml`, in the workspace, keeps `[provider]` and `[logging]`.
     Rename `ApplicationConfig` to `WorkspaceConfig` and
     `load_application_config()` to `load_workspace_config()`, dropping `host`
     and `port` from both. This is the file all three frontends read.
   - `app.toml`, in the application directory, is new and holds `host`, `port`,
     and `workspace`. Only `chaweb` reads it.

   The division is not cosmetic: `host` and `port` describe the machine the
   server runs on, and must be readable *before* a workspace is located,
   because one of them says where the workspace is.
3. **Read the application configuration.** `--config <path>` names the file and
   defaults to `<root>/app.toml`. `--host`, `--port`, and `--workspace`
   override individual values from the command line. There are no compiled-in
   defaults: each setting comes from the file or the command line, and a
   missing one is an error naming the setting and both ways to supply it. An
   explicitly named `--config` that does not exist is an error; the default
   path may be absent when the command line carries everything, which is how
   tests and development runs work.
4. **Resolve the application root in `web_main.cpp`.** Change `main()` to
   accept `argc` and `argv`. `--root <path>` is the application directory and
   defaults to `executable_directory()`. Reject an unknown option with a usage
   message rather than ignoring it, since a mistyped workspace path that
   silently fell back to something else would start the application against the
   wrong conversations.
5. **Send each path to the side that owns it**
   ([web_main.cpp](../src/apps/web_main.cpp#L24)). The workspace owns the
   customer's files: `load_dotenv(workspace / ".env")`,
   `load_workspace_config(workspace)`, and `Workspace(workspace, config)`.
   Use the returned `log_file` unchanged; the loader already resolves a
   relative logging path beneath the root it was given, so `logs/cha.log` now
   lands in the workspace, which is where a customer's log belongs. The
   application root owns only `web/`, wired in Block 2. Nothing forbids the two
   paths being equal, which keeps test fixtures simple.
6. **Fail clearly when the workspace is wrong.** A missing or
   `workspace.toml`-less workspace is the mistake this setup invites, and it
   will usually be a customer's typo rather than a developer's. Report the
   resolved path and the option that sets it, not just a file-not-found error.
7. **Announce the address on standard output** once the listener is ready, as
   one predictable line. It is written for the person starting CHA, who opens
   that address in a browser themselves: the port is configurable, so neither a
   customer nor a developer should have to work out what to type from a
   configuration file. Nothing consumes the line programmatically. Keep
   `/health` as it is; the tests use it.
8. **Update `console_main.cpp` and `tui_main.cpp` for the rename only.** They
   call `load_application_config()` with the default `"."`
   ([console_main.cpp](../src/apps/console_main.cpp#L49),
   [tui_main.cpp](../src/apps/tui_main.cpp#L28)) and use only the logging and
   provider values, so they follow the rename and gain nothing else. The
   terminal frontends stay working-directory relative: they are launched from
   inside a workspace and must keep behaving that way.
9. **Update the process test helper.** `WebServerProcess` currently `chdir`s
   into the workspace before exec ([web_server_process.cpp](../tests/support/web_server_process.cpp#L136));
   it must pass `--root`, `--workspace`, `--host`, and `--port` instead,
   because the child's working directory no longer determines anything and
   there is no configuration file in a temporary fixture. Root and workspace
   may name the same temporary directory, which already holds the fixture web
   root from Block 2.
10. **Accept a wildcard listener address.** When the configured host is `0.0.0.0`
   or `::`, `allowed_host_authorities()` must accept any `Host` whose port
   equals the listener port and whose host part is an IP address literal or
   `localhost`, and keep rejecting everything else. Refusing arbitrary DNS names
   costs nothing here and denies a hostile web page the easiest route to this
   API: pointing its own domain at the machine's address so the browser sends
   that domain as `Host`. A specific configured host keeps today's behavior
   exactly. The `Origin`-must-equal-`Host` rule for JSON mutations is unchanged
   and keeps working from any address, because a real browser sends the address
   the user actually typed in both headers.
11. **Leave development runs on `127.0.0.1`.** There is no reason to expose a
    development machine, and the wildcard path is covered by the tests below
    rather than by daily use. Block 7 ships `host = "0.0.0.0"` in the packaged
    `app.toml`.
12. **Make the build fill in `bin/`, a real deployment.** Add a post-build step
    to `chaweb_app` ([CMakeLists.txt](../CMakeLists.txt#L552)) that copies the
    executable to `bin/chaweb`. Follow the existing `-P` script pattern used for
    [`embed_text.cmake`](../cmake) rather than a chain of `cmake -E` calls,
    since Block 2 adds a conditional copy to the same step.

    [`bin/start-cha.sh`](../bin/start-cha.sh) already exists and is committed.
    It passes `--root`, `--workspace`, `--host`, and `--port` explicitly from
    three assignments at the top of the file, so a development run needs no
    configuration file at all; `bin/` is otherwise generated, which
    [`.gitignore`](../.gitignore) expresses as `/bin/*` with the script negated.
    The step also seeds `bin/app.toml` the first time, and never rewrites it
    afterwards: it is a file a developer edits to try a different port or
    workspace, and a build that reverted it every time would be worse than not
    writing it at all. Command-line settings override it by design, so a
    developer who prefers configuring the application can edit that file and
    delete the flags from the script.

    Block 2 extends the same step with `web/`, and from then on
    `cmake --build` followed by `./bin/start-cha.sh` is the ordinary way to run
    the application. This is the only development path that exercises the
    two-root split, the command-line settings, and the asset root together.

### Tests

- The root argument wins over the executable directory; absent the argument, the
  executable directory is used.
- A command-line `--host`, `--port`, or `--workspace` wins over the same
  setting in the configuration file; a setting present in neither is an error
  naming it.
- The two directories are independent: an application root containing no
  workspace still starts when `--workspace` names one elsewhere.
- `--config` names a file that does not exist: an error. The default
  configuration path is absent but the command line is complete: it starts.
- A relative `log_file` resolves under the workspace, not the application root;
  an absolute one is untouched.
- A workspace path that does not exist, or that holds no `workspace.toml`,
  fails with a message naming that path.
- The ready line appears on standard output only after the listener is bound,
  and carries the port actually in use.
- `chaweb` started from an unrelated working directory serves `/health` and
  `/api/v1/bootstrap` from the workspace named by `--workspace`.
- The terminal frontends still start from inside a workspace after the rename.
- With `0.0.0.0` configured: an IP-literal `Host` on the listener port is
  accepted, as are `localhost` and `127.0.0.1`; a DNS name, and a correct
  address on the wrong port, are rejected with `forbidden_host`.
- With a specific host configured, the accepted and rejected sets are unchanged.
- Existing terminal and web suites stay green.

### Done when

`chaweb` behaves identically from any working directory with the two
directories in unrelated places, whether its settings come from `app.toml` or
from `--root`, `--workspace`, `--host`, and `--port`; configured with `0.0.0.0`
it answers a browser on a second machine; and a build leaves `bin/` holding the
executable and a configuration file that points at the repository workspace.

---

## Block 2 — Static asset serving

**Goal.** `chaweb` serves the built browser application from `<root>/web/`.

### Work

1. **Give `AssetHandler` a web root.** Replace the placeholder
   ([asset_handler.h](../src/ui/web/asset_handler.h)) with
   `AssetHandler(std::filesystem::path web_root)`. Fail at construction with a
   clear message if `web_root/index.html` is missing; a package without it is
   broken and should say so at startup rather than answer 404s. Name the missing
   path in that message and, from Block 3 onward, the staging command that
   creates it, because in a fresh development checkout this failure is expected
   rather than a defect.
2. **Serve the shell** at `/` and, through `SessionRoutes`, at
   `/s/{forum}/{session}/`. Both return `web/index.html`. Keep them a single
   code path so they cannot drift.
3. **Serve `GET /assets/<file>`** from `web/assets/`. One path segment only.
   Validate the filename with the existing `is_valid_route_component()`, which
   already accepts Vite's `app-<hash>.js` shape, then resolve with
   `weakly_canonical` and require the result to stay under the canonical web
   root. That covers `..`, absolute paths, and symlink escapes together.
4. **Assign content types** from a small extension table covering `.html`,
   `.css`, `.js`, `.svg`, `.png`, `.woff2`, `.json`, and `.map`. An unknown
   extension is a 404, not a guess. `.woff2` is listed for a future web font;
   the design uses system fonts today and the build emits no font file.
5. **Set cache headers.** `index.html` gets `Cache-Control: no-cache`. Anything
   under `/assets/` gets `public, max-age=31536000, immutable`, which is safe
   because content hashes change the URL.
6. **Set a Content Security Policy** on the shell response. Start strict:
   `default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'
   data:; font-src 'self'; connect-src 'self'; base-uri 'none'; form-action
   'none'; frame-ancestors 'none'`. If the build turns out to need inline
   styles, change the build before loosening the policy, and record whichever
   choice is made. This header is the whole policy: the served `index.html`
   carries no CSP `<meta>` tag of its own (Block 3, step 5). `'self'` covers
   every address the server answers on, so the wildcard binding from Block 1
   needs nothing here.
7. **Wire the application root through `web_main.cpp`**:
   `AssetHandler(root / "web")`. This is the one place that uses `--root`;
   everything else in the entry point takes the workspace.
8. **Provide assets to every test server.** Add a reusable temporary web-root
   fixture which writes a minimal `index.html` and `assets/app.js`. Pass that
   root to the lobby-route and session-route test servers, and add it to every
   process-test workspace before starting `chaweb`.
9. **Extend the `bin/` staging step with `web/`.** The post-build step from
   Block 1 also mirrors `src/resources/webapp/dist/` into `bin/web/` whenever
   that directory exists, so a build refreshes both halves of the deployment.
   It must not fail when the frontend has never been built; `chaweb` will say
   what is missing when it starts, which is the clearer message anyway.
   Until Block 3 produces a real build, write the minimal `index.html` and
   `assets/app.js` into `bin/web/` by hand. Nothing here is committed, and the
   workspace never gains a `web/` directory. The C++ suite does not depend on
   any of it, since step 8 gives every test its own temporary web root.

### Tests

- `/` and a session deep link return the same document.
- An asset returns its content type and the immutable cache header.
- `index.html` returns `no-cache` and the CSP header.
- `..`, an absolute path, a nested subdirectory, and an unknown extension all
  return 404 with the standard error envelope.
- A process test serves a real file from a real `web/` directory.
- Every pre-existing route and process test receives a temporary web root and
  the full C++ suite remains green.

### Done when

A hand-written `bin/web/index.html` and `bin/web/assets/app.js` are served
correctly at `/`, at a deep link, and at `/assets/app.js` by a `./bin/chaweb`
started with no arguments, with every safety case rejected.

---

## Block 3 — Frontend skeleton and build

**Goal.** The real shell, built by Vite, served by `chaweb`, with a working
development loop.

### Work

1. **Pin and verify the toolchain.** Commit `.node-version` for Node.js 22.23.1
   and declare the same supported version in `package.json`. Verify the
   development-tool commands listed above before adding application code.
2. **Create `src/resources/webapp/`** with the layout in
   [webapp.md](webapp.md#source-and-output-directories): `package.json`, a
   committed lockfile, `vite.config.ts`, `index.html`, and `src/` split into
   `components/`, `api/`, `state/`, `styles/`, and `main.tsx`. `src/resources/`
   is new; the existing top-level `resources/` keeps the OpenAPI description
   and application guide and is unrelated.
3. **Install and lock the named dependencies.** Use local npm dependencies
   only. Add scripts for `dev`, `build`, `typecheck`, `test`, `e2e`,
   `api-types`, `check`, and `stage`; `check` is the normal frontend validation
   entry point.
4. **Configure Vite** with `base: '/'` and the proxy entry given verbatim in
   [webapp.md](webapp.md#development-workflow). Both parts matter: the shell
   route must stay unmatched, and both `Host` and `Origin` must be rewritten to
   the target, or every mutation is rejected.
5. **Port the mockup's structure and CSS.** Take the layout, class hierarchy,
   and styling from [mockup.html](web-ui/mockup.html) into `src/styles/` and
   component markup. Drop its hard-coded content. Two things must not come
   across. The mockup's `<meta http-equiv="Content-Security-Policy">` exists
   only to make a standalone file work and permits unpkg, inline script, and
   `eval`; the production `index.html` carries no CSP meta tag at all, so the
   Block 2 response header stays the single policy. Its `frame-src 'self'` is
   likewise a mockup artifact. Keep `--font-sans` and `--font-mono` as the
   system stacks the mockup already defines: no web font is downloaded, so
   there is none to bundle and none to serve.
6. **Replace the external icon and positioning libraries.** The mockup loads
   `lucide` and `@floating-ui` from unpkg. Inline the handful of SVG icons the
   design actually uses rather than adding a dependency for them.
7. **Build the static component tree** from
   [web-ui/README.md](web-ui/README.md) and the
   [screens](web-ui/screens): sidebar with title, Personas, Characters, Forums,
   Recent, and no gear; exclusive main area; chat composer with the context
   line; the two-line toggle. Desktop and iPhone widths, sidebar pushing rather
   than overlaying. Replace the mockup's unsupported attachment button with the
   target-character chooser. The Send button occupies the same location when
   idle and becomes a Stop button while generation is active; the context line
   remains read-only.
8. **Add the staging command.** `npm run stage` runs the production build and
   then safely replaces `bin/web/` with the result, so frontend-only work needs
   no CMake run; `--root <dir>` stages elsewhere. The Block 7 packaging script
   performs the corresponding copy for a release. CMake copies `dist/` when it
   is there but never installs npm dependencies and never builds browser files.
   Nothing new needs ignoring: `bin/` is already ignored and the workspace never
   contains `web/`. Document the command, since a fresh checkout has no
   `web/index.html` at all and `chaweb` will say so at startup.
9. **Establish Playwright now.** Add its configuration and Chromium browser,
   plus a deterministic test workspace using the existing test-mode provider.
   The browser suite starts and stops a real test `chaweb` and never requires a
   provider key, Internet access, or paid model request. Downloading the
   browser is not enough to run it: it links against system libraries that
   `sudo npx playwright install-deps chromium` installs separately, which
   [the webapp README](../src/resources/webapp/README.md) records. Treat a
   machine where that step has not been run as a machine where this suite has
   not been run, and never report the block complete on its strength.
10. **Ignore generated directories**: `src/resources/webapp/node_modules/`,
    `src/resources/webapp/dist/`, Playwright output, and local staged package
    directories. `/bin/` was ignored in Block 1.

### Tests

- Type-check passes.
- A component test renders the shell at both viewport widths and confirms the
  two-line button changes only sidebar visibility, per
  [behavior.md](web-ui/behavior.md).
- A Playwright smoke test starts the deterministic server, loads `/`, reloads a
  session-shaped deep link through Vite, and completes one JSON mutation through
  the development proxy.
- `npm run check`, `npm run e2e`, and the C++ suite all pass independently.

### Done when

All four Stage 1 criteria in [webapp.md](webapp.md#stage-1--production-shell)
hold: the shell loads from `chaweb` at `/` and at a deep link, the deep link
reloads correctly against the development server, a scripted API mutation
succeeds through the development proxy, and `chaweb` starts from a foreign
working directory. In practice this means `cmake --build`, `npm run stage`, and
`./bin/chaweb` produce the real interface in a browser.

---

## Block 4 — API client and discovery screens

**Goal.** Every read-only screen renders real workspace data.

### Work

1. **Generate types.** `npm run api-types` produces `src/api/schema.d.ts` from
   [`../resources/cha.yaml`](../resources/cha.yaml) with `openapi-typescript`.
   Commit the output so a checkout type-checks without running the generator,
   and regenerate whenever the contract changes. `npm run check` regenerates
   the file and fails if the committed output changes, so API drift cannot be
   overlooked.
2. **Write `src/api/client.ts`**, the only module that knows URLs and wire
   format. One function per operation: bootstrap, character detail, list
   sessions, create session, open session, snapshot, submit input, stop, set
   default character. It sends correct headers, parses success bodies, and
   converts the `ErrorResponse` envelope into a single
   `ChaError { status, code, message }`. Components never touch `fetch`.
3. **Write `src/api/events.ts`** for streaming: open the `EventSource`,
   distinguish `snapshot` from `append`, and hand typed events to the state
   layer. Its error callback exposes only a stream failure, never an HTTP status
   or API error body. Recovery logic lands in Block 6; this block only
   establishes the module and guarantees that closing it is idempotent.
4. **Implement the state model** from
   [behavior.md](web-ui/behavior.md#state-model) as a reducer: sidebar, main
   area, current persona and forum, active conversation, inspected character,
   and current default character.
5. **Load bootstrap at startup** and select the three initial IDs it returns.
   Do not hardcode `builtin-guest`, `builtin-entrance`, or `builtin-welcome`.
6. **Add a small defensive check** that the bootstrap response carries the three
   initial IDs and four collections, and fail into a clear incompatible-response
   state rather than a blank screen. Generated types are compile-time only.
7. **Implement Personas, Characters, Character detail, and Forums** against the
   screen catalogue and the sidebar rules in
   [web-ui/README.md](web-ui/README.md). Forum rows list member character names;
   there are no forum descriptions and member names are not links.
8. **Add restricted Markdown rendering** for `character_markdown`: headings,
   paragraphs, emphasis, lists, inline code, and code blocks. Sanitize the
   output; do not execute raw HTML, fetch images, or create interactive links.
   Character definitions may contain unexpanded template directives such as
   `$$(EPICTETUS.md)`; rendering them literally is accepted.

### Tests

- Each client function against a stubbed `fetch`, including the error envelope
  producing a `ChaError` with the documented `code`.
- The Markdown renderer strips `<script>`, `<img>`, and link interactivity.
- Component tests for each screen from fixture data.
- The reducer preserves sidebar state across every navigation action the
  behavior table requires.

### Done when

All read-only navigation shows real workspace data from a running `chaweb`.

---

## Block 5 — Stored sessions and URLs

**Goal.** A user can browse, create, open, and return to sessions.

### Work

1. **Sessions screen** listing a forum's sessions with names and compact time
   metadata from `updated_at`. No descriptions, no excerpts.
2. **New session**: trim the entered name, submit only a non-empty label, then
   create, open, set active, refresh Recent, and show Chat. The server accepts
   an empty label and substitutes the generated ID, so this validation is the
   only guard.
3. **Recent** from bootstrap's `recent_sessions`, newest first, with the active
   entry highlighted. There is no separate Recent endpoint: refreshing it means
   re-fetching bootstrap.
4. **URL model**: `/` starts from bootstrap defaults; `/s/{forum}/{session}/`
   identifies the active conversation; switching sessions updates history
   without a page load. Use the History API and a small parser.
5. **Deep-link boot**: bootstrap, then open or reattach the identified session,
   then snapshot, then stream. If the session cannot be opened, show a useful
   error and offer a way back to Welcome.
6. **Prevent duplicate creates and opens** while one is pending.

### Tests

- Create-then-open flow against a stubbed client, including the trimmed-name
  rule.
- Route parser unit tests, including the required trailing slash.
- An end-to-end deep-link reload restoring the conversation.

### Done when

Browsing, creating, opening, and revisiting all work, and a session URL survives
a reload.

---

## Block 6 — Live chat and stream recovery

**Goal.** The chat workflow operates end to end, including after an
interruption.

### Work

1. **Load the snapshot and connect the stream** on becoming active. Render the
   transcript, generation state, and the context line
   `<Forum>   From: <Persona>   To: <default character>` from
   [web-ui/README.md](web-ui/README.md).

   **Including the two paths that reach a conversation without opening it.**
   Block 5 opens, snapshots, and streams whenever a session is *chosen* — a
   stored-session row, a Recent entry, a deep link, a restored history entry.
   Two paths instead adopt the initial conversation from bootstrap and set it
   active without any request: the plain `/` boot, and Return to Welcome. That
   is invisible while the transcript is a placeholder and will not be once it
   is real, so becoming active must drive the open, not the click that caused
   it. A conversation that is active with no stream attached is the state to
   look for.
2. **Apply events**: a `snapshot` event replaces state wholesale; an `append`
   event appends to the entry or reasoning target it names.
3. **Submit input** as `{"persona": "<id>", "text": "<text>"}` with the selected
   persona. Preserve the draft when a submission fails.
4. **Implement default-character selection and Stop.**
   The target-character chooser occupies the composer position vacated by the
   attachment button and lists the active session's characters. Selection calls
   the default-character endpoint. While `generation.active` is true, the Send
   arrow in the composer becomes a Stop square calling the stop endpoint; it
   becomes Send again only when authoritative session state is inactive.
   A disconnected event stream does not disable the HTTP Stop action, and the
   composer remains editable so a reconnect never destroys or blocks a draft.
   Send and target changes still wait for a connected stream: a page that
   cannot observe updates, especially a second viewer, must not start work it
   cannot follow.
5. **Keep exactly one stream.** Close the old one before connecting a new one on
   a session switch.
6. **Implement application-owned recovery** exactly as
   [webapp.md](webapp.md#recovering-an-interrupted-stream) specifies. On an
   `EventSource` error, close that object and probe the ordinary session
   snapshot endpoint because `EventSource` does not expose a refused response's
   status or JSON body. If the snapshot succeeds, the session is live: back off
   and create a new stream. If it returns `409 session_not_live`, re-open the
   session, load a fresh snapshot, and create a new stream. Network or server
   failures remain in the retry path. Use delays of 250 ms, 500 ms, 1 second,
   2 seconds, and 4 seconds, then stop and show an explicit Retry action.
7. **Name the second-viewer case instead of retrying at it.** A session serves
   one event stream at a time and refuses a second with
   `409 browser_stream_in_use` ([web_session_runtime.cpp](../src/ui/web/web_session_runtime.cpp#L342)).
   That is now ordinary: a second tab, or a second person on the network, opens
   a session someone already has open. `EventSource` cannot see that status, so
   infer it. Every snapshot probe succeeding while every stream attempt fails
   immediately, through the whole ladder, means the one stream is held
   elsewhere. Say so — `This session is open in another window` — and offer
   Retry and a way to open a different session. Keep the transcript that the
   snapshot already returned on screen: this view is current but not updating,
   which is far better than a blank panel or an unexplained Retry button.
   If the inference proves confusing in use, the cheap fix is a server-side
   flag in session state saying whether a stream is attached; that is a
   `cha.yaml` change and is deliberately not in v1.
8. **Treat `503 session_limit_reached` as transient but bounded.** Show
   `Waiting for another session to close`, retry on the same bounded backoff,
   and then show Retry and return-to-Welcome actions. Never leave an endless
   spinner or retry loop.

### Tests

- Reducer tests for snapshot replacement and for appends against both target
  kinds.
- A plain `/` boot and Return to Welcome each attach a stream to the initial
  conversation, rather than leaving it active but unattached.
- A fake event source driving snapshot and append sequences.
- Recovery tests in which a stream error is followed by a successful snapshot,
  by `session_not_live`, by temporary server unavailability, and by exhausting
  the retry bound.
- A ladder exhausted while every snapshot probe succeeds reaches the
  open-in-another-window state, not the generic retry state, and leaves the
  transcript rendered.
- End to end: send a message and observe streamed text; stop generation;
  recover from a dropped stream; recover from a session the server has already
  unloaded; open one session in two browser contexts and confirm the second
  explains itself.

### Done when

The chat workflow works end to end, including after the machine has slept long
enough for the server to unload the conversation.

---

## Block 7 — Hardening, packaging, and launcher

**Goal.** A directory that can be copied to a clean laptop and used.

### Work

1. **Complete the operational states** listed in
   [webapp.md](webapp.md#loading-and-failure-behavior): initial loading, empty
   session list, request in progress, recoverable error, stream reconnecting,
   session no longer live, session open in another window, too many live
   sessions, API unavailable, and incompatible response.
2. **Guard every pending mutation** against duplicate submission.
3. **Never render credentials or filesystem paths.** Use the server's public
   message and keep the `code` for behavior and diagnostics.
4. **Confirm no Settings remains.** The gear, the Settings view, and its
   main-area state are removed rather than deferred, which amends
   [web-ui/README.md](web-ui/README.md) and
   [behavior.md](web-ui/behavior.md); both now record the removal, though their
   screenshots still show the gear. Nothing in the sidebar should occupy that
   position instead.
5. **Write one reproducible packaging command.** Its script runs `npm ci`,
   checks generated OpenAPI types, runs frontend checks, builds
   `src/resources/webapp/dist/`, builds `chaweb`, and assembles a clean
   versioned application
   directory. It never writes a `.env` or any real key.
6. **Assemble the application directory** per
   [webapp.md](webapp.md#production-deployment-directories): the executable,
   `web/`, the launcher, and `app.toml`. Nothing a customer edits day to day
   belongs here. The workspace is not built by packaging; it is assumed to
   exist already, and how it is prepared is out of scope. Treat a release as
   one versioned unit and reject stale files from an earlier assembly. The
   packaged `app.toml` sets `host = "0.0.0.0"`; a packaging check asserts that,
   since the value is what makes the application reachable and a silent revert
   to loopback would look like a network fault. A second check asserts that no
   workspace file has leaked into the application directory, because that
   mistake stays invisible until the first upgrade destroys someone's
   conversations.
7. **Grow the Linux launcher** out of
   [`bin/start-cha.sh`](../bin/start-cha.sh), which is already the right shape:
   a shell script beside the executable whose first three lines are the host,
   port, and workspace path, and which passes them to `chaweb`. Setup is
   editing those lines. Ship `app.toml` alongside it carrying the same three
   settings, for anyone who would rather configure the application than edit a
   script; the command line wins where both are present. It keeps a visible
   terminal that stops the process, and that is all it does: the customer opens
   the printed address in a browser themselves, so the script neither launches
   one nor waits for the ready line to find out where to point it. `exec`
   therefore stays, and a failed start reports itself in the terminal without
   any timeout logic. Print the machine's LAN address next to the local one so
   the address to give someone else needs no working out, and say plainly that
   anyone on the network who opens it can read and continue the conversations.
   macOS and Windows launchers follow later, in that order; with no browser to
   open, they differ from this one only in shell syntax.
8. **Write the setup and upgrade instructions, and make them true.** Setup is:
   put the workspace somewhere, write its path and the port into the launcher's
   first three lines, create `.env` in the workspace with the provider key, run
   the launcher. Upgrade is: replace the application directory, then re-enter
   those three values in the new launcher. Verify by upgrading over an
   already-used workspace and confirming the conversations, characters, and
   provider key survive.
9. **Complete and run the existing Playwright suite** against the assembled
   package, covering the minimum flows in
   [webapp.md](webapp.md#testing-strategy), including interruption and rapid
   Recent navigation. The suite's `served` project already loads the production
   build from `chaweb` with no development server in the path, which is what
   keeps the Stage 1 criteria proved rather than remembered; extend it here
   rather than starting a separate package suite.
10. **Test on a clean Linux machine** representative of the customer's laptop,
    including placing the workspace somewhere the application directory does
    not contain, and running on a port other than 8080.

### Done when

Every item in
[webapp.md](webapp.md#definition-of-a-working-web-application) holds.

---

## Decisions, all settled

The five questions left open by [webapp.md](webapp.md#decisions-still-required)
have been answered. Nothing in this plan is now waiting on a decision.

| Question | Answer |
| --- | --- |
| Primary operating system | Linux, then macOS, then Windows. Block 7 writes the Linux launcher and tests on a Linux machine; `executable_directory()` still implements all three branches from the start. |
| Workspace ownership | Out of scope. A prepared workspace is assumed to exist; packaging neither creates nor ships one. |
| API-key setup | A `.env` text file in the workspace, described in the setup instructions. Nothing more: no configuration screen, no key validation, no first-run wizard. |
| Settings | Removed from v1 entirely, rather than shipped as an informational page. |
| Port | Not fixed. `host` and `port` come from `app.toml` or the command line; a port already in use fails at startup with the message `chaweb` already produces. |

## Out of scope

Attachments, Settings in any form, and any mutation API for personas,
characters, or forums. Preparing a workspace: one is assumed to exist.
Installers, code signing, background services, and automatic updates. Expanding
template directives inside `CHARACTER.md`, which is an accepted rough edge.

Network access is in scope, but nothing that would make it safe outside a
trusted network is: no authentication, no transport security, and no
per-user separation. Several browsers watching one session simultaneously is
also out of scope, because the server delivers one event stream per session and
fanning it out to many listeners is a change to `SseMailbox` and
`BrowserConnectionState` rather than a frontend feature. Block 6 explains the
one open session to the second browser instead.
