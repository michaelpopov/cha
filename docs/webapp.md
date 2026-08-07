# CHA web application proposal

> Historical proposal. The browser-only architecture after removal of the
> earlier application variants is recorded in [removal-plan.md](removal-plan.md).

Status: accepted, 2026-08-06.

## Purpose

CHA should be easy to place on a customer's laptop, start, and use in a browser
at the address it prints on startup, `http://127.0.0.1:8080/` by default
configuration. The customer opens that address themselves; CHA does not open a
browser for them.

A single-file executable is not required. The intended deliverable is a pair of
directories: an application directory holding the native executable and browser
assets, and a separate workspace holding the customer's configuration,
characters, and stored conversations. Keeping them apart is what makes an
upgrade safe. The customer should not need Node.js, a web server, or a frontend
development tool.

This proposal connects the two pieces that already exist:

- the implemented HTTP and Server-Sent Events API, described by
  [`../resources/cha.yaml`](../resources/cha.yaml); and
- the visual and behavioral design in [`web-ui/`](web-ui/README.md).

The missing piece is the production browser client and the machinery for
serving and packaging it.

## Proposed result

The finished application has two cooperating parts:

1. `chaweb`, the existing native C++ process. It owns the workspace, sessions,
   model communication, HTTP API, event streams, and shutdown behavior.
2. A static browser application made from HTML, CSS, JavaScript, icons, and
   other local assets. It presents the designed UI and calls the C++ API.

The browser application is not another server. It is a set of files downloaded
by the browser from `chaweb`.

```text
Browser UI
    │
    │ HTTP requests and one live event stream
    ▼
chaweb on the configured port, reachable locally and on the local network
    │
    ├── workspace and stored sessions
    └── configured model provider
```

The browser and API use the same origin. This preserves the server's current
Host and Origin protections and avoids cross-origin configuration.

## Recommended technical decisions

### Browser application

Use:

- TypeScript for browser code;
- React for UI components and state-driven rendering;
- Vite for the development and production build;
- the existing visual design as the source for layout and styling; and
- local, bundled dependencies only.

The initial production browser target is current Chrome and Edge. Routine
browser automation uses Playwright Chromium. Firefox and Safari may be added to
the supported matrix when a customer platform requires them.

React is useful here because Chat, navigation, session state, streaming text,
forms, and responsive layout must update together. No server-rendering framework
is needed: CHA is a local application whose data comes from its local API.

Vite provides a development server and produces ordinary static production
assets. Its production output is suitable for placement in the deployment
directory's `web/` directory. See the
[Vite production build documentation](https://vite.dev/guide/build).

Do not use CDNs at runtime. Fonts, icons, JavaScript, and CSS must be included in
the application directory. The customer-facing application should continue to
work without Internet access except when `chaweb` contacts the configured model
provider.

### Source and output directories

Frontend source lives under `src/resources/`, beside the C++ source it ships
with but in its own tree:

```text
src/resources/webapp/
├── package.json
├── package-lock.json
├── vite.config.ts
├── index.html
└── src/
    ├── components/
    ├── api/
    ├── state/
    ├── styles/
    └── main.tsx
```

The Vite build produces generated files under `src/resources/webapp/dist/`.
Those generated files are copied into a deployment as `web/`. They are not
maintained by hand.

The repository's top-level `resources/` directory remains the location for
source resources such as the application guide and OpenAPI description. It is
not the production web root.

### The local deployment directory

The repository also has a `bin/` directory beside `src/` and `workspace/`,
which is a real deployment rather than a build tree: the executable, `web/`,
and `start-cha.sh`. Building copies the freshly built `chaweb` into it, and
copies the current frontend build in as `web/` when one exists, so
`./bin/start-cha.sh` runs the application exactly the way a customer's launcher
will. Its contents are generated apart from the script itself, which is the
same shape as the launcher a release ships.

This matters more than convenience. The two-directory split, the application
configuration file, and the asset root are all things a build tree cannot
exercise, so without `bin/` the first genuine test of the deployment layout
would be the customer's laptop.

### Production deployment directories

A release is two directories that must never be merged. Linux is the first
target:

```text
cha/                        # the application directory: replaced by an upgrade
├── chaweb
├── app.toml                # host, port, and the workspace path
├── start-cha.sh
└── web/
    ├── index.html
    └── assets/
        ├── app-<hash>.js
        ├── app-<hash>.css
        └── bundled icons

<anywhere>/cha-workspace/   # kept across upgrades; its path is set in app.toml
├── workspace.toml          # provider and logging settings
├── .env                    # created locally by the customer
├── characters/
├── forums/                 # stored conversations live here, per forum
├── personas/
└── logs/
```

The executable name changes by platform, but the logical division does not. The
application directory holds only what ships with a release. The workspace holds
everything the customer owns or accumulates: their provider key, their
characters and personas, every stored conversation, and the log.

Packaging builds the application directory only. A workspace is assumed to
exist; how it is prepared is outside this proposal.

The split exists so that upgrading is safe. A new release replaces the
application directory outright; if the workspace lived inside it, that would
delete the customer's conversations, characters, and API key. Keeping them apart
makes the upgrade procedure "replace one directory, leave the other alone".

The workspace path is not fixed by the layout. It is written into `app.toml`
during setup, so the workspace may sit anywhere, including a drive shared with
other machines. Neither directory has to contain the other.

The customer creates `.env` in the workspace, holding the provider key named by
`workspace.toml`. A plain text file and a line of written instruction is the
whole of key setup; anything more belongs to a configuration feature that does
not exist. A real API key must never be included in a distributed package.

A release should be treated as one versioned unit. Mixing a `chaweb` binary from
one release with browser files from another is unsupported.

### Finding application files

`chaweb` currently resolves everything it needs from the process's current
working directory: `.env`, `app.toml`, the log path configured inside `app.toml`,
and the workspace directories. That is fragile for a double-clickable
application because the working directory depends on how the program was
launched. Starting from a shortcut, a pinned taskbar item, or another drive can
leave `chaweb` unable to find its workspace, or writing logs somewhere the
customer will not think to look.

The application should instead resolve two explicit roots.

The **application root** defaults to the directory containing the executable
and holds only `web/`, beside the executable, launcher, and `app.toml` that
define it.

The **workspace root** holds `workspace.toml`, `.env`, `characters/`,
`personas/`, `forums/` with their stored conversations, and `logs/`. Its path is
set during setup, so it can live anywhere and does not have to sit near the
application. A relative `logging.file` resolves beneath it, which puts the log
with the data it describes rather than in a directory an upgrade will replace.

Together these make the package behave consistently whether it is started from
a terminal, a shortcut, or a file browser, and they keep customer data outside
everything a release overwrites.

### Two configuration files

The single `app.toml` in the workspace should become two files, divided the same
way the directories are.

`app.toml`, in the application directory, holds `host`, `port`, and the
workspace path. Only `chaweb` reads it. `--config` names it, and `--host`,
`--port`, and `--workspace` override individual values on the command line.
These settings have to be readable before a workspace is located, because one
of them says where the workspace is.

`workspace.toml`, in the workspace, holds the provider and logging settings.
All three frontends read it, and the terminal frontends need nothing else — they
have no listener and never wanted `host` or `port`.

Neither the host nor the port is fixed. A port already in use is reported at
startup by the existing bind failure, which is sufficient: automatic port
selection can be added if customer machines turn out to need it.

This is a prerequisite for the packaged application rather than a finishing
touch, so it belongs in Stage 1. Leaving `chaweb` working-directory-relative
through the early stages would mean revisiting file loading, logging
initialization, and the asset handler's root after they are already built.

## Serving browser files

The current `AssetHandler` returns placeholder HTML. It should be changed into a
restricted static-file handler.

The production server should map:

| Request | Response |
| --- | --- |
| `/` | `web/index.html` |
| `/s/{forum}/{session}/` | `web/index.html` |
| `/assets/...` | Matching file below `web/assets/` |
| `/api/...` | Existing root-scoped C++ API |
| `/s/.../api/...` | Existing live-session C++ API |

Only known files below `web/` may be served. The handler must reject absolute
paths, `..` traversal, symlink escapes, and unsupported files. It must assign
correct content types for HTML, CSS, JavaScript, images, fonts, and source maps
if source maps are included.

`index.html` should not be cached across application upgrades. Vite's
content-hashed assets may use long-lived immutable caching because a content
change gives them a new URL.

The server should add a restrictive Content Security Policy. The browser UI
needs local scripts and styles and same-origin API connections; it does not need
arbitrary remote scripts, frames, plugins, or images.

## The small HTTP client

The frontend needs a small TypeScript module that is the only browser code which
knows the API URLs and wire format. It is called an HTTP client because it is the
client-side counterpart of the C++ HTTP server. It is not another process and it
is not a general-purpose networking framework.

Visual components should ask this module to perform application operations such
as:

- load bootstrap data;
- read character detail;
- list, create, and open sessions;
- load a live-session snapshot;
- submit input;
- stop generation; and
- change the default character.

The module is responsible for constructing URLs, sending JSON with the proper
headers, parsing successful responses, and turning the API's error envelope into
one consistent frontend error type. Components remain concerned with what the
user is doing, not with `fetch`, headers, status codes, or JSON parsing.

Event streaming belongs beside this module. It opens the browser's native
`EventSource`, distinguishes `snapshot` from `append` events, and passes typed
events to the application state layer. Its error callback reports only that the
stream failed. Recovery uses an ordinary snapshot request to determine whether
the session remains live; it never expects `EventSource` to reveal an HTTP error
body.

### OpenAPI's role

The OpenAPI document is the authoritative description of the wire contract. A
build-time tool should generate TypeScript type declarations from it. Those
types describe request bodies, successful responses, error responses, snapshots,
and append events.

Only types should be generated initially. The eleven operations are simple
enough that a manually written client remains easier to inspect and debug than a
large generated runtime client.

Generated types provide compile-time checking; they do not validate arbitrary
JSON at runtime. The API remains responsible for producing valid responses. A
small number of defensive checks should still protect the UI from an invalid or
incompatible bootstrap response and malformed event data.

## Browser state and navigation

Implement the state model already defined in [`web-ui/behavior.md`](web-ui/behavior.md):

- sidebar open or closed;
- current main-area view;
- selected persona;
- selected forum;
- active forum and session;
- inspected character; and
- current default character from live session state.

Use React's built-in state and reducer facilities initially. A separate global
state library is unnecessary unless the implementation demonstrates a concrete
need for one.

Only the active conversation needs a durable browser URL:

- `/` starts from the bootstrap defaults;
- `/s/{forum}/{session}/` identifies an active conversation; and
- switching sessions updates the browser history without reloading the page.

Sidebar position, the selected navigation screen, and the inspected character
can remain in memory. A page reload may reset them. Persona persistence can also
be deferred; the documented startup persona is Guest.

No routing library is needed for this limited URL model. The browser History API
and a small route parser are sufficient.

## Startup flow

On a normal visit to `/` the browser should:

1. Load bootstrap data.
2. Select the initial Guest, Entrance, and Welcome IDs returned by bootstrap.
3. Open or reattach Welcome.
4. Load the authoritative session snapshot.
5. Open its event stream.
6. Display Chat.

On a deep link under `/s/{forum}/{session}/`, it should load bootstrap, open or
reattach the identified session, load its snapshot, and connect its event
stream. If the session cannot be opened, the UI should show a useful error and
offer a way back to the initial Welcome session.

The UI should keep at most one active `EventSource`. Switching sessions closes
the old stream before connecting the new one. Every new stream starts with an
authoritative snapshot, so the client does not need to reconstruct missed
events.

### Recovering an interrupted stream

A dropped stream is normal operation, not an edge case: a sleeping laptop, a
wifi change, or a VPN transition all end it. Two server behaviors make recovery
more than reconnecting.

A session accepts one event stream at a time and answers a second attempt with
`409 browser_stream_in_use`. A non-successful HTTP response is fatal to that
`EventSource`, and its generic error event exposes neither the status nor the
JSON error body. An immediate retry can also race the server's own disconnect
handling. Reconnection must therefore be owned by the application.

That one-stream rule also decides what happens when two browsers open the same
session, which network access makes routine. For the first release the second
browser is told the session is open elsewhere and shows the snapshot it already
loaded, rather than retrying at a slot that will not free. Letting several
browsers watch one session at once is a server change and is not part of this
release.

On a stream error, the browser closes that `EventSource` and calls the ordinary
session-snapshot endpoint. A successful snapshot proves the session is live, so
the browser waits briefly and creates a new stream. A `409 session_not_live`
snapshot response means the runtime has unloaded; the browser re-opens the
session, loads a fresh snapshot, and then creates a new stream. Network and
server failures remain in the retry path.

Recovery uses delays of 250 milliseconds, 500 milliseconds, 1 second, 2
seconds, and 4 seconds. After those attempts it stops automatically and shows a
visible Retry action. This bounds load on the local server and prevents an
infinite reconnect loop.

## UI implementation scope

The first production browser UI should implement the agreed screens:

- Chat;
- Personas;
- Characters;
- Character detail;
- Forums;
- Sessions;
- New session; and
- Recent sessions in the sidebar.

Session creation follows the existing design: validate a non-empty trimmed name,
create the stored session, open it, select it as active, refresh Recent, and show
Chat.

The visual mockup is not production source. It contains hard-coded content and
loads its icon script externally. Its structure and CSS are useful references,
but production components must render API data and bundle icons locally.

The production composer makes two explicit changes to the mockup. The position
occupied by the unsupported attachment button becomes a target-character
chooser listing the active session's characters. The Send arrow becomes a Stop
square while authoritative session state says generation is active. The compact
Forum/From/To line below the composer remains read-only and reflects the chosen
target only after the server returns it in session state.

### Features without backend support

Attachments and functional Settings are outside the current API contract.
Controls that appear usable but do nothing should not ship.

For the first release:

- omit attachment functionality and use the former attachment-button position
  for the target chooser; and
- remove Settings entirely — no gear, no view, no main-area state. An
  informational page was the alternative and was rejected as work that buys the
  customer nothing.

Adding provider or workspace configuration through Settings is a separate
feature because it requires API design, secure file updates, validation, and
restart behavior. Until then, configuration is the two text files described
above.

## Markdown safety

Character detail contains server-provided Markdown which ultimately originates
in workspace files. It must be treated as untrusted browser content.

The UI should support the restricted subset required by the design: headings,
paragraphs, emphasis, lists, inline code, and code blocks. It must not execute
raw HTML, fetch images, or create interactive external links.

If a general Markdown parser is used, sanitize its HTML output before inserting
it into the document. For example, Marked explicitly states that it does not
sanitize its own output; see the [Marked security guidance](https://marked.js.org/).
The sanitizer configuration should allow only the elements and attributes in
CHA's documented subset.

## Loading and failure behavior

The design documents the successful flow, but a usable application also needs a
small, consistent set of operational states:

- initial loading;
- empty forum session list;
- request in progress;
- recoverable request error;
- event stream reconnecting;
- session no longer live;
- session already open in another browser window;
- too many live sessions;
- application API unavailable; and
- incompatible or malformed response.

"Too many live sessions" needs care because a customer can reach it during
ordinary use. The server keeps a bounded number of sessions live, and a session
the user has navigated away from holds its place for about thirty seconds
afterwards. Moving briskly through Recent can therefore exhaust the bound and
produce `503 session_limit_reached` for no reason the customer can perceive.
Because the places normally free themselves, the initial response is a
transient `Waiting for another session to close` state and the same bounded
retry schedule used for stream recovery. If the retries are exhausted, the UI
shows Retry and return-to-Welcome actions rather than an endless spinner.
Raising the server's bound is a fallback if testing shows this is not enough; it
is coupled to the HTTP worker pool size and is not a single-value change.

While an operation is pending, prevent duplicate creates, opens, or submissions.
Errors should use the server's public message when appropriate and retain the
structured error code for behavior and diagnostics. They must never display
provider credentials or private filesystem paths.

An API error should not normally erase the last valid screen state. For example,
a failed send should preserve the user's draft, and a failed session open should
leave navigation usable.

## Development workflow

Frontend development should not require rebuilding C++ after every CSS or
component change.

Development uses the Node.js version recorded by the frontend project. npm
installs all frontend packages locally under `src/resources/webapp/node_modules/`; no React,
Vite, TypeScript, OpenAPI, or browser-test package is installed globally. The
standard frontend commands are `npm ci`, `npm run dev`, `npm run build`,
`npm run check`, and `npm run e2e`. These are build-time tools only and never
enter the customer directory.

During development:

1. Run `chaweb` on its normal local port.
2. Run the Vite development server on a separate development port.
3. Configure Vite to forward only API requests to `chaweb`, rewriting both the
   `Host` and `Origin` headers to `chaweb`'s address.
4. Open the Vite URL in the browser and use its fast reload behavior.

Step 3 has two requirements that are easy to get wrong, and both fail in ways
that look like server bugs.

**Forward the API routes, not the page.** `/s` is not an API prefix. It holds
the shell route `/s/{forum}/{session}/` as well as the live-session API routes
beneath it. A plain `/s` proxy key also captures the shell, so a session deep
link opened against the development server is answered by `chaweb` with the
production HTML instead of the page being edited, without fast reload. The
symptom is confusing because `/` is unaffected: the application appears to work
until the first reload on a session URL.

**Present `chaweb`'s address in both headers.** `chaweb` does not evaluate the
browser's notion of origin. It compares the `Host` header it receives against
its configured listener authority, and for JSON mutations it additionally
requires the `Origin` header's authority to equal that `Host`. A proxy sits
between the two and must satisfy both checks. Vite's default preserves the
original `Host`, so every forwarded request is rejected with `forbidden_host`.
Setting `changeOrigin` alone fixes reads but leaves `Origin` pointing at the
development server, so every create, open, submit, stop, and default-character
request is rejected with `forbidden_origin`.

Both requirements are met by one proxy entry:

```ts
// vite.config.ts
const target = 'http://127.0.0.1:8080';

export default defineConfig({
  server: {
    proxy: {
      // A leading '^' makes the key a regular expression. The shell route
      // /s/{forum}/{session}/ is deliberately unmatched, so Vite keeps
      // serving the page itself.
      '^/(api/|s/[^/]+/[^/]+/api/)': {
        target,
        changeOrigin: true,
        configure: (proxy) =>
          proxy.on('proxyReq', (request) => request.setHeader('origin', target)),
      },
    },
  },
});
```

Event streaming needs no additional proxy configuration, but it should be
exercised early: a buffering proxy breaks streaming silently, delivering a
whole answer at once instead of progressively.

For a production build:

1. Install the locked frontend dependencies.
2. Generate TypeScript API types from `resources/cha.yaml`.
3. Type-check and test the frontend.
4. Run the Vite production build.
5. Build `chaweb` with CMake.
6. Run the packaging script, which creates a clean versioned application
   directory holding the executable, `src/resources/webapp/dist/` copied in as
   `web/`, the
   launcher, and `app.toml`. It does not build a workspace.

Node.js and frontend packages are build-time dependencies only. They are not
included in the customer package.

npm owns browser generation and tests, CMake owns the native build, and the
packaging script is the only step which assembles the two outputs. One packaging
command performs the complete sequence so a release cannot accidentally contain
stale browser files.

## Testing strategy

Use three layers:

1. **C++ API tests.** Keep the existing route, protocol, SSE, lifecycle, and
   process tests.
2. **Frontend unit and component tests.** Test state transitions, rendering,
   form validation, error handling, snapshot replacement, and append events.
3. **End-to-end browser tests.** Start a real test `chaweb` with a deterministic
   test-mode workspace, open the application in Playwright Chromium, and cover
   the principal user flows. These tests require no provider key, Internet
   connection, or paid model request.

The minimum end-to-end flows are:

- startup into Welcome;
- sidebar and navigation behavior;
- persona selection followed by message submission;
- character detail rendering;
- forum and session browsing;
- session creation and opening;
- live transcript updates and stopping generation;
- session deep-link reload;
- responsive layout at desktop and iPhone-sized viewports;
- recovery after an interruption longer than the server's idle grace, which
  requires re-opening the session rather than only reconnecting its stream;
- moving quickly through several Recent entries without a visible error; and
- useful behavior when API calls or the event stream fail.

Playwright is a suitable end-to-end runner because it supports Chromium,
Firefox, WebKit, and mobile viewport/device emulation; see the
[Playwright browser documentation](https://playwright.dev/docs/browsers).

## Customer startup and shutdown

The first distributable version does not need an installer or background
service. The Linux launcher, a shell script beside the executable, does two
things:

1. starts `chaweb` with its own directory as the application root and the host,
   port, and workspace path it carries in its first three lines; and
2. keeps a visible terminal window through which the process can be stopped.

The application never opens a browser. It prints the address it is serving on
and the customer opens that address themselves, in whichever browser and at
whichever moment they choose. Launching a browser would be wrong here for the
same reason it is wrong in a development run: CHA is restarted often, and each
restart would steal focus and leave another tab behind.

Printing the address still matters, because the port is configurable and a
customer should never have to work out what to type from a configuration file.

Closing that window or pressing Ctrl+C stops the process through its existing
bounded shutdown path. A tray application, OS service, installer, signing, and
automatic updates can be considered after the basic packaged application is
proven.

Bind to `0.0.0.0` so the application can also be opened from another machine on
the same network, amending this proposal's original loopback-only decision.
This is deliberate and its consequence is accepted: there is no authentication
and no transport security, so anyone who can reach the port can read and
continue the stored conversations. It suits a trusted home or office network
and must not be exposed to the Internet.

The server's existing `Host` and `Origin` checks stay in force. They need one
adjustment, because both derive from the configured listener host and a
wildcard address matches nothing a browser would send; see Block 1 of
[webapp-plan.md](webapp-plan.md).

## Recommended implementation stages

### Stage 1 — Production shell

- Give `chaweb` an explicit application root for `web/` and `app.toml`, and a
  separate workspace root for `.env`, `workspace.toml`, logging, and the
  workspace contents.
- Create the TypeScript, React, and Vite project.
- Reproduce the static visual shell with production components and local icons.
- Make `chaweb` serve the production HTML and assets from `web/`.
- Establish the development proxy and production build.
- Establish Playwright and its deterministic local test server before later
  stages add browser scenarios.

Done when the real shell loads from `chaweb` at both `/` and a session deep
link, the same deep link reloads correctly against the development server, one
API mutation succeeds through the development proxy, and `chaweb` starts
correctly from a working directory other than its own. The mutation matters:
it is the only check that proves the proxy's header rewriting is correct, and
without it a misconfiguration stays hidden until Stage 3.

### Stage 2 — Discovery and navigation

- Add generated OpenAPI types and the small HTTP client.
- Load bootstrap.
- Implement sidebar, Personas, Characters, Character detail, and Forums.
- Add safe Markdown rendering.

Done when all read-only navigation uses real workspace data.

### Stage 3 — Stored sessions

- Implement session listing.
- Implement New session validation, creation, and opening.
- Implement Recent and session URL updates.

Done when a user can browse, create, open, and revisit sessions.

### Stage 4 — Live Chat

- Load snapshots and connect SSE.
- Render transcripts and generation state.
- Submit input using the selected persona.
- Put target-character selection in the attachment position and change Send to
  Stop while generation is active.
- Add session-switch cleanup and application-owned stream recovery, including
  re-opening a session that the server has already unloaded.
- Handle a temporarily exhausted session bound with bounded automatic retries
  followed by visible Retry and return-to-Welcome actions.

Done when the principal chat workflow operates end to end, including after the
machine has slept long enough for the server to unload the conversation.

### Stage 5 — Hardening and package

- Complete loading, empty, and failure states.
- Add the Content Security Policy and production cache behavior.
- Complete and run the component and browser tests accumulated in earlier
  stages.
- Build the application directory, the starting workspace, and the platform
  launcher.
- Test on a clean machine representative of the customer's laptop, and verify
  that replacing the application directory leaves an existing workspace intact.

Done when the package can be copied to that machine, configured with an API key,
started, used in the browser, and stopped without development tools.

## Decisions, now settled

The product and packaging choices this proposal left open have been answered.

1. **Primary operating system.** Linux first, then macOS, then Windows. The
   first launcher and the clean-machine test are Linux; executable-root support
   is implemented for all three from the start.
2. **Workspace ownership.** Out of scope. A prepared workspace is assumed to
   exist, and packaging neither creates nor ships one.
3. **API-key setup.** A `.env` text file in the workspace, explained in written
   instructions. Nothing further is built for it.
4. **Settings.** Removed from the first release rather than shipped as an
   informational page.
5. **Host and port.** Not fixed. Both come from `app.toml` or the command line.
   A port already in use is reported by the existing startup failure.

## Definition of a working web application

The proposal is complete when a clean customer-like laptop can:

- receive or extract the application directory and its starting workspace;
- put the workspace where it wants it and point the launcher at it;
- add its provider API key without installing development tools;
- start CHA using the supplied launcher;
- open the printed address in a browser and reach the application;
- navigate the designed workspace screens;
- create, open, and revisit sessions;
- conduct a live streaming conversation under a selected persona;
- stop generation and recover from an interrupted event stream;
- reload a session deep link; and
- stop CHA cleanly without losing stored sessions.
