# CHA daemon on Linux

The tarball is built on a compatible Linux machine, then copied to the server.
It contains the daemon, the ChaWeb browser application, a seeded example vault,
and deployment scripts. The server needs systemd and nginx. It does not need
Node.js, npm, or a JavaScript server.

## Building the package

On the Linux build machine, install the repository's C++ build prerequisites
(CMake, Ninja, a C++ compiler, and static OpenSSL) and the Node.js/npm versions
pinned in `webapp/package.json` (see also `webapp/.node-version`). Then run, in
the repository directory:

```sh
make package-linux VERSION=<version>
```

The script installs the locked browser dependencies with `npm ci`, typechecks
and builds ChaWeb, builds the daemon, and writes
`packages/cha-linux-<version>.tar.gz`. It stops if one of these steps fails.
The archive contains:

```text
cha-linux-<version>/
  cha-daemon                  the daemon
  chaweb/                     static ChaWeb files: index.html and assets/
  nginx-chaweb.conf.example   per-user ChaWeb server block
  cha@.service, cha@.socket   systemd unit templates
  install.sh, add_user.sh     installation scripts
  cha-config.example/         example vault
  README.md                   this file
```

## Installing

On the server, extract the archive and run `install.sh` from the extracted
directory as a regular user, not as root:

```sh
export CHA_DEPLOY_PATH=/srv/cha
export CHA_DATA_PATH=/srv/cha-data
./install.sh
```

Both paths must be absolute and must contain no spaces. `CHA_DATA_PATH` must
be outside `$CHA_DEPLOY_PATH/chaweb`. The installer invokes `sudo` for system
changes. The daemon runs as the user who installs the package, and that user
owns both directories.

The installer:

- copies `cha-daemon`, `add_user.sh`, `nginx-chaweb.conf.example`, and the
  example vault to `CHA_DEPLOY_PATH`;
- replaces `$CHA_DEPLOY_PATH/chaweb` with the packaged ChaWeb files, readable
  by all users;
- installs the systemd unit templates only if they are absent;
- reloads systemd.

It does not create ChaWeb listeners. You add them by hand (see below). After
the installation, you can remove the extracted archive directory.

Existing unit files and nginx configuration are kept; check their paths if
you change the deployment directories later. `CHA_LISTEN` is obsolete and
is ignored. Configure each user's listener as described below.

### Socket group

Each user's socket is `/run/cha/<user>.sock` with mode `0660`, group
`www-data`, and `Accept=no`. nginx workers must be in that group. The nginx
worker user is the non-root user in `ps -o user= -C nginx`, or the `user`
directive in `/etc/nginx/nginx.conf`. If nginx does not run as `www-data`,
change `SocketGroup` in `/etc/systemd/system/cha@.socket`, then run
`sudo systemctl daemon-reload` before you add a user. Keep the mode `0660`
and the service user.

### Static file access

nginx workers must read the files in `$CHA_DEPLOY_PATH/chaweb` and search
(`x` permission) each parent directory. The installer makes the ChaWeb files
readable and tests access as the nginx worker user. If it prints a warning,
the usual cause is a private home directory. Prefer a path such as `/srv/cha`,
or give the nginx user search permission only on each parent directory:

```sh
sudo setfacl -m u:www-data:x /home/YOUR_USER /home/YOUR_USER/opt
```

Do not make the vault directories in `CHA_DATA_PATH` readable to nginx.

### Why the asset names contain hashes

The build names each JavaScript and CSS file after a hash of its content, for
example `assets/index-Y8z9LPoS.js`, and `index.html` refers to these names. The
ChaWeb example sends `index.html` with `Cache-Control: no-cache`, so the
browser checks it on each load. The hashed files use
`Cache-Control: public, max-age=31536000, immutable`, because a changed file
gets a new name. API responses use `Cache-Control: no-store`.

## Adding users

Export `CHA_DEPLOY_PATH` and `CHA_DATA_PATH` again, then run:

```sh
"$CHA_DEPLOY_PATH/add_user.sh" alice
```

This copies the example vault to `$CHA_DATA_PATH/alice/config`, owned by the
installing user, and enables `cha@alice.socket`. Repeating the command keeps
an existing user configuration. Each user's daemon starts on its first
request through nginx.

To use an existing CHA vault, put its configuration directory at
`$CHA_DATA_PATH/<user>/config` before you run `add_user.sh`. The daemon
requires its database to exist before startup. The vault's providers, forums,
characters, and provider API keys must already work in CHA. Server providers
can use API keys or a ChatGPT subscription. To connect ChatGPT in ChaWeb, open
Assistant in Entrance's Welcome conversation and ask it to connect your account.
Open the verification link, enter the code, approve access, and reply "Done".
Assistant then completes login inside the daemon.

If Assistant cannot answer because its own provider needs that login, run this
as the daemon's user before starting the daemon:

```sh
"$CHA_DEPLOY_PATH/cha-daemon" --config "$CHA_DATA_PATH/<user>/config" --openai-login
```

The command prints the link and code and waits for approval, then exits. The
daemon reads the credentials only when it starts, so if it is already running,
restart it after login. Both methods save credentials to the fixed
`openai-auth.json` in that user's config directory. There is no credential-file
argument.

The example vault contains the bundled characters and forum; configure a provider
with working credentials before sending chat requests.

For a protected vault, put its password in
`$CHA_DATA_PATH/<user>/config/password`. The file must be
a regular file owned by the installing user with mode `0600`.

ChaWeb and the daemon have no settings editor. To change the configuration
with the native CHA application, the daemon must release the vault. Stop both
units first, because the socket starts a stopped service again:

```sh
sudo systemctl stop cha@alice.socket cha@alice.service
# edit the vault, then:
sudo systemctl start cha@alice.socket
```

## ChaWeb listeners

Each user gets one private HTTPS port. The port selects the user's daemon
socket; it does not authenticate. Anyone who can reach the port can use that
user's data, so listen only on a trusted private network. There is no ChaWeb
login or API key.

Copy `$CHA_DEPLOY_PATH/nginx-chaweb.conf.example` into the nginx `http`
context, for example as `/etc/nginx/conf.d/chaweb.conf`. For each user, make
one server block and replace:

- the private address and port in `listen`, for example `8443` for Alice and
  `8444` for Bob;
- `server_name` with the hostname the iPhone resolves;
- the TLS certificate: add `ssl_certificate` and `ssl_certificate_key` lines;
  nginx does not accept an `ssl` listener without them;
- `root` with `$CHA_DEPLOY_PATH/chaweb` as a literal path, the same for all
  users;
- the literal socket in `scgi_pass`, for example `unix:/run/cha/alice.sock`
  for Alice and `unix:/run/cha/bob.sock` for Bob.

Keep the buffered SCGI settings, the 256 KiB body limit, JSON gzip, and
`Cache-Control: no-store` in the API location. Do not add CORS headers or
bearer-key routing to ChaWeb listeners.

The enclosing `http` block must include `mime.types`, and nginx must have the
standard `scgi_params` file; the default Debian and Fedora `nginx.conf` do
both. The `scgi_params` file forwards `CONTENT_TYPE`, which ChaWeb POST
requests need.

The iPhone must resolve the hostname, reach the port, and trust the
certificate. Validate before you reload:

```sh
sudo nginx -t
sudo systemctl reload nginx
```

## Checks

Check that the socket is active and that the service uses the right vault:

```sh
systemctl status cha@alice.socket
systemctl cat cha@alice.service
ls -l /run/cha/alice.sock
```

Then request the application and the API through the user's port. Use `-k`
only if this machine does not trust the certificate:

```sh
curl -sI https://cha.example.test:8443/
curl -s https://cha.example.test:8443/api/cha/v1/bootstrap
```

The first response must be `200` with `Content-Type: text/html` and
`Cache-Control: no-cache`. Load a file named in `index.html` from `/assets/`;
it must have the JavaScript or CSS content type and the immutable cache header.
The bootstrap response must be JSON with this user's forums. At first setup,
check the second user's port once for its own forums and sessions. Then open
the address in Safari, send a short message, and reload the page to see the
stored session.

## Upgrading

Install the daemon and ChaWeb from the same package, because the browser files
must match the daemon API. Extract the new archive and run `install.sh` with
the same `CHA_DEPLOY_PATH` and `CHA_DATA_PATH`. The installer replaces the
daemon, `add_user.sh`, the example vault, the ChaWeb files, and the ChaWeb
example. It keeps the systemd units, nginx configuration, ChaWeb server
blocks, TLS setup, port assignments, and user vaults. Then
restart the running daemons, so that they use the new binary:

```sh
sudo systemctl try-restart 'cha@*.service'
```

Stopped daemons start with the new binary on their next request. Compare the
new `nginx-chaweb.conf.example` with your server blocks and copy the changes
you need. Reload browser tabs that were open during the upgrade. A reload
loses unsent drafts, so finish or copy them first. Check bootstrap and a
conversation after the upgrade.

The daemon serves only the custom `/api/cha/v1` API. The former `/v1/models`
and `/v1/chat/completions` endpoints return `404`. If an older installation
has an OpenAI nginx site, remove that server block and its unused
`/etc/nginx/cha-users.map` file, then validate and reload nginx. The installer
keeps existing nginx configuration.

## Troubleshooting

- nginx errors: `sudo tail /var/log/nginx/error.log` and `sudo nginx -t`.
- `403` or `404` for `/` or `/assets/`: check the `root` path and the static
  file access above, for example
  `sudo -u www-data test -r /srv/cha/chaweb/index.html`.
- `502` from the API: check the socket group and nginx worker group, then
  `systemctl status cha@alice.socket cha@alice.service` and
  `journalctl -u cha@alice.service`.
- The daemon log is `$CHA_DATA_PATH/alice/config/logs/cha.log`, as set in
  `app.toml`.
- After repeated startup failures, systemd stops the socket too. After the
  repair, run
  `sudo systemctl reset-failed cha@alice.socket cha@alice.service` and
  `sudo systemctl start cha@alice.socket`.

## Migrating from `cha-<user>` accounts

If an earlier package created `cha-alice`, migrate that installation before
running this package's `install.sh`:

```sh
sudo systemctl stop cha@alice.socket cha@alice.service
sudo chown -R "$(id -un):" "$CHA_DATA_PATH/alice"
sudo sed -i "s/^User=cha-%i$/User=$(id -un)/" /etc/systemd/system/cha@.service
sudo systemctl daemon-reload
sudo systemctl enable --now cha@alice.socket
./install.sh
```

Repeat the stop and ownership steps for every existing CHA user before
changing the service template. Once the migrated daemons work, the old
`cha-*` system accounts and their directory ACL entries can be removed.
