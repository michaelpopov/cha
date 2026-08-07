# CHA browser application

The browser source is built with the pinned Node.js version in `.node-version`.
Dependencies and browsers are development tools only; the customer receives
the generated files under `web/`.

From this directory:

```sh
npm ci
npm run api-types
npm run check
npm run build
npm run stage
npm run e2e
```

`npm run stage` builds the browser files and safely replaces the repository's
`bin/web/`. Pass `--root <application-directory>` after `--` to stage another
application root. `chaweb` reads `web/index.html` once at startup, so a staged
shell reaches the browser only after the server is restarted; the hashed asset
names under `assets/` make that the correct behavior for a release, but it does
mean `npm run stage` alone will not refresh a running server.

## Two development loops, two ports

There are two ways to run the application while working on it, and they
deliberately listen on different ports so that neither blocks the other.

| Loop | Command | Serves | Port |
| --- | --- | --- | --- |
| Staged | `make run` | `bin/web/`, exactly as a release does | 8080 |
| Editable | `make run-web-dev` plus `npm run dev` | Vite, with hot reloading | 8888 and 5173 |

`npm run dev` serves the editable shell on `http://127.0.0.1:5173/` and proxies
API requests to a `chaweb` listening on `127.0.0.1:8888`; it does not start that
server. `make run-web-dev` is the one that starts it, on the port the proxy
expects. Browse the editable shell at `http://127.0.0.1:5173/`, not at 8888.

The two loops keep separate ports so that a staged server and an editable one
can run at the same time. The browser suite is independent of both: it chooses
its own ports, so neither loop needs to be stopped before `npm run e2e`.

Both loops bind loopback. `bin/start-cha.sh` is the launcher a release ships,
but the three settings committed at its top are the development ones; the
packaging command substitutes the customer's `HOST`, `PORT`, and `WORKSPACE`
from `packaging/linux/app.toml` as it assembles the application directory. That
keeps the launcher's behavior in one file and each setting in one file, and it
is why a development run is not reachable from the network while a packaged one
is.

## Browser tests

`npm run e2e` builds the browser files first, then runs two Playwright
projects against one deterministic `chaweb`. The production-served project
runs the complete customer flow as well as the static-header checks; the
development project repeats the application flow through Vite's proxy. The
suite picks its own pair of ports
rather than taking a fixed one, so a staged server, a container, or a previous
run that has not exited yet cannot make the suite fail; set `CHA_E2E_PORT` to
pin the API port deliberately. The two projects are:

- `chromium` drives the application through the development server, which is
  where the API proxy, `Host`/`Origin` rewriting, and hot reloading are proved.
- `served` loads the production build straight from `chaweb`, with no
  development server in the path. It is what proves the shipped application
  runs: the built bundle, the asset routes, the cache headers, and the Content
  Security Policy together. When `CHA_E2E_APPLICATION_ROOT` names an assembled
  application directory, it uses that directory's `chaweb` and `web/` in place
  of build-tree outputs. The Linux packaging command uses this mode. A policy
  that blocked the bundle would show up here as a console error rather than as
  a blank page in front of a customer.

The suite starts a copied deterministic workspace, a real `chaweb`, and a tiny
local OpenAI-compatible streaming provider; it never reads an API key or uses
the external network. The projects run serially because they share that one
process and each live session accepts one event stream; a dedicated two-page
scenario verifies the rejected second viewer and its recovery.

Dropped-stream recovery is covered by refusing one event-stream request and
letting the snapshot probe and replacement stream run against the real server.
The unload scenario also refuses the stream, then holds the real recovery probe
past the server's test-only shortened idle grace. The resulting
`session_not_live`, re-open, fresh snapshot, and replacement stream are all real
server behavior; the suite does not synthesize an API response.

The local provider emits small response chunks with a short delay. That makes
generation observably active long enough for the browser suite to click Stop,
without adding a delay or special response path to the application itself.
Component tests additionally verify that Stop and draft editing remain
available while the event stream is reconnecting.

Install the Playwright Chromium browser and its host libraries once on a
development machine with:

```sh
npx playwright install chromium
sudo npx playwright install-deps chromium
```
