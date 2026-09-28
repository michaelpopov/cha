# CHA daemon on Linux

The tarball is built on a compatible Linux machine, then copied to the server.
It contains the daemon, a seeded example vault, and deployment scripts. The
server needs systemd and nginx.

On the server, extract the archive and run `install.sh` from the extracted
directory:

```sh
export CHA_DEPLOY_PATH=/home/YOUR_USER/opt/cha
export CHA_DATA_PATH=/home/YOUR_USER/var/cha
./install.sh
```

Both paths must be absolute and must contain no spaces. The installer copies
`cha-daemon`, `add_user.sh`, and the example vault to `CHA_DEPLOY_PATH`. It
installs the systemd unit templates only if absent, creates a dedicated nginx
site and an empty API-key map only if absent, then reloads systemd and nginx.
It invokes `sudo` for system changes when run as a regular user. The daemon
runs as the user who installs the package. Existing unit
files and nginx configuration are preserved; check their paths if you change
the deployment directories later. The nginx site listens on
`127.0.0.1:8086` by default. Set `CHA_LISTEN` before the first install if you
need another address. The socket template uses group `www-data`; if
nginx runs under another group, change `SocketGroup` in the installed
`cha@.socket` and run `systemctl daemon-reload` before adding a user.

You can now remove the extracted archive directory. To add a user later, export
`CHA_DEPLOY_PATH` and `CHA_DATA_PATH` again, then run:

```sh
"$CHA_DEPLOY_PATH/add_user.sh" alice
```

This copies the example vault to `$CHA_DATA_PATH/alice/config`, owned by the
installing user, adds a randomly generated API key to nginx, and enables
`cha@alice.socket`. Save the printed API key. Repeating the command keeps an
existing user configuration and key. Each user's daemon starts on its first
request through nginx. To reach the default localhost listener from another
machine, use an SSH tunnel such as `ssh -L 8086:127.0.0.1:8086 SERVER`.
Bearer API keys travel in plain text over HTTP, so use a trusted connection.

The example vault contains the bundled characters and forum. Configure a
provider with working credentials before sending chat requests. You can use an
existing CHA configuration and vault in place of the example; the daemon
requires its database to exist before startup.

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
