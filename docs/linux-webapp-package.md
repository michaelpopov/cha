# Linux browser application package

CHA ships as two separate directories. The versioned application directory is
replaceable; the workspace contains the customer's configuration, provider key,
characters, and conversations and must survive every upgrade.

## Build a release

On a development machine with the recorded Node.js version, npm, CMake, Ninja,
and a C++ toolchain installed, run this at the repository root:

```sh
./scripts/package-linux.sh 0.1.0
```

The command installs the locked browser dependencies, checks generated API
types, runs frontend checks, builds the production browser files, builds
`chaweb`, assembles and validates a clean tree, and runs the full production-
served Playwright flow against that assembled executable and browser bundle.
Re-running the command replaces that exact output with a clean tree. The
package contains only:

```text
cha-linux-0.1.0/
├── chaweb
├── app.toml
├── start-cha.sh
└── web/
```

It never creates or copies a workspace or `.env` file. Validate an assembled
directory independently with `./scripts/check-linux-package.sh <directory>`.
The packaging command's final upgrade test creates a disposable external
workspace, stores a session, replaces only the application directory, and
confirms that the fake provider-key file, character definition, and stored
session all survive and reopen.

## Set up on a customer machine

1. Put the prepared workspace anywhere outside the application directory. It
   must contain `workspace.toml`, `characters/`, `forums/`, and `personas/`.
2. Edit the three settings at the top of `start-cha.sh`: `HOST`, `PORT`, and
   `WORKSPACE`. Relative
   workspace paths are resolved from the application directory. Keep
   `HOST='0.0.0.0'` for access from this machine and its trusted local network.
3. In `workspace.toml`, find `[provider].api_key_env`. Create `.env` in the
   workspace with that variable and the real provider key, for example
   `OPENAI_API_KEY=...`. Never put the key in the application directory.
4. Run `./start-cha.sh`. Open the printed local address in Chrome or Edge. The
   launcher does not open a browser. Press Ctrl+C in its terminal to stop CHA.

The package also includes `app.toml` with the same initial host, port, and
workspace values. To configure that file instead, edit it and run `./chaweb`
directly; launcher command-line values take precedence when the launcher is
used.

CHA reads the workspace once, when it starts. After editing `workspace.toml`,
personas, characters, forums, or prompts, stop CHA with Ctrl+C and start it
again for the change to take effect. Conversations are unaffected: new stored
sessions appear in the lobby without a restart.

Startup also checks every forum in the workspace, not only the ones in use. If
any forum has an invalid default character, member override, or prompt, CHA
refuses to start and prints which forum and file to fix. An upgrade can surface
a problem in a forum that older versions never reported.

`HOST='0.0.0.0'` deliberately exposes CHA to the local network. There is no
authentication or transport security: anyone who can reach the printed network
address can read and continue conversations. Use it only on a trusted home or
office network and never expose the port to the Internet.

## Upgrade without losing data

1. Stop CHA with Ctrl+C.
2. Record the old launcher's `HOST`, `PORT`, and `WORKSPACE` values.
3. Replace the entire old application directory with the new versioned one.
4. Re-enter those three values in the new `start-cha.sh` (or update the new
   `app.toml` if that is how CHA is started).
5. Start CHA and reopen an existing stored session.

Do not copy the workspace into the application directory and do not replace it
during an upgrade. Its `.env`, characters, and SQLite conversation files then
remain byte-for-byte outside the directory being replaced.
