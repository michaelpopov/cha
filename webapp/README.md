# CHA browser application

The browser source is built with the pinned Node.js version in `.node-version`.
The packaged desktop hosts load the generated files. Domain calls use the
native bridge; Vite never proxies application RPC.

From this directory:

```sh
npm ci
npm run api-types
npm run check
npm run build
```

Native development serves only frontend assets/HMR at `http://127.0.0.1:5173/`:

```sh
make run-native-dev CONFIG=/path/to/cha-config
```

## Browser tests

`npm run check` runs schema generation checks, TypeScript, and Vitest. Native
host automation lives under `tests/native/`. The old HTTP Playwright suite
that launched `chaweb` has been removed.

Presentation tests use a fake `ChaClient` or fake native bridge. They do not
start an application listener. Component tests verify that Stop and draft
editing remain available while a live stream is replaced.
