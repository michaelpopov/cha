#!/bin/sh
set -eu

fail() { echo "install.sh: $*" >&2; exit 2; }

if [ "$#" -ne 0 ]; then fail "usage: ./install.sh"; fi
: "${CHA_DEPLOY_PATH:?set CHA_DEPLOY_PATH to an absolute path}"
: "${CHA_DATA_PATH:?set CHA_DATA_PATH to an absolute path}"
for path in "$CHA_DEPLOY_PATH" "$CHA_DATA_PATH"; do
    case "$path" in
        /*) ;;
        *) fail "paths must be absolute: $path" ;;
    esac
    [ "$path" != / ] || fail "paths must not be the filesystem root"
    case "$path" in
        *[!A-Za-z0-9_./+-]*) fail "path contains unsupported characters: $path" ;;
    esac
done

if [ "${CHA_LISTEN+x}" = x ]; then
    echo "Warning: ignoring obsolete CHA_LISTEN; configure each user's ChaWeb listener in nginx." >&2
fi

if [ "$(id -u)" -ne 0 ]; then
    install_user=$(id -un)
    exec sudo env \
        CHA_DEPLOY_PATH="$CHA_DEPLOY_PATH" CHA_DATA_PATH="$CHA_DATA_PATH" \
        CHA_INSTALL_USER="$install_user" "$0"
fi
install_user=${CHA_INSTALL_USER:-${SUDO_USER:-}}
[ -n "$install_user" ] || fail "run install.sh from a regular user account"
install_uid=$(id -u "$install_user") || fail "unknown installing user: $install_user"
[ "$install_uid" -ne 0 ] || fail "run install.sh from a regular user account"

if [ -f /etc/systemd/system/cha@.service ] && \
    grep -qx 'User=cha-%i' /etc/systemd/system/cha@.service; then
    fail "the existing cha@.service uses per-user system accounts; migrate its User= setting and data ownership before reinstalling"
fi

source_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
for file in cha-daemon add_user.sh cha@.service cha@.socket \
    nginx-chaweb.conf.example chaweb/index.html; do
    [ -f "$source_dir/$file" ] || fail "missing $file from the extracted package"
done
[ -d "$source_dir/cha-config.example/config" ] || fail "missing example vault"
[ -d "$source_dir/chaweb/assets" ] || fail "missing chaweb/assets from the extracted package"
command -v nginx >/dev/null 2>&1 || fail "nginx is required"

mkdir -p -- "$CHA_DEPLOY_PATH" "$CHA_DATA_PATH"
# nginx serves everything under the static root, and the installer replaces it.
static="$(cd -- "$CHA_DEPLOY_PATH" && pwd -P)/chaweb"
case "$(cd -- "$CHA_DATA_PATH" && pwd -P)/" in
    "$static"/*) fail "CHA_DATA_PATH must be outside $static" ;;
esac
install -m 755 "$source_dir/cha-daemon" "$CHA_DEPLOY_PATH/cha-daemon"
install -m 755 "$source_dir/add_user.sh" "$CHA_DEPLOY_PATH/add_user.sh"
install -m 644 "$source_dir/nginx-chaweb.conf.example" \
    "$CHA_DEPLOY_PATH/nginx-chaweb.conf.example"
mkdir -p -- "$CHA_DEPLOY_PATH/cha-config.example"
cp -R "$source_dir/cha-config.example/." "$CHA_DEPLOY_PATH/cha-config.example/"
rm -rf -- "$static"
cp -R "$source_dir/chaweb" "$static"
chmod -R u=rwX,go=rX "$static"
chown -R "$install_user:" "$CHA_DEPLOY_PATH"
chown "$install_user:" "$CHA_DATA_PATH"

temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
sed -e "s|@CHA_DEPLOY_PATH@|$CHA_DEPLOY_PATH|g" \
    -e "s|@CHA_DATA_PATH@|$CHA_DATA_PATH|g" \
    -e "s|@CHA_INSTALL_USER@|$install_user|g" \
    "$source_dir/cha@.service" > "$temporary/cha@.service"
for unit in cha@.service cha@.socket; do
    destination="/etc/systemd/system/$unit"
    if [ -e "$destination" ] || [ -L "$destination" ]; then
        echo "Keeping existing $destination"
        if [ "$unit" = cha@.service ] && \
            ! grep -Fq "$CHA_DEPLOY_PATH/cha-daemon --config $CHA_DATA_PATH/%i/config" "$destination"; then
            echo "Check ExecStart in $destination: it differs from the requested paths" >&2
        fi
    else
        if [ "$unit" = cha@.service ]; then
            install -m 644 "$temporary/$unit" "$destination"
        else
            install -m 644 "$source_dir/$unit" "$destination"
        fi
    fi
done
systemctl daemon-reload

# nginx workers need search permission on every parent of the static root.
nginx_user=$(nginx -T 2>/dev/null |
    sed -n 's/^[[:space:]]*user[[:space:]][[:space:]]*\([^[:space:];]*\).*/\1/p' |
    head -n 1)
nginx_user=${nginx_user:-nobody}
if ! runuser -u "$nginx_user" -- test -r "$static/index.html"; then
    echo "Warning: nginx user $nginx_user cannot read $static/index.html." >&2
    echo "Give it search permission on each parent directory, for example:" >&2
    echo "  sudo setfacl -m u:$nginx_user:x DIRECTORY" >&2
fi

echo "Installed CHA. Run CHA_DEPLOY_PATH=$CHA_DEPLOY_PATH CHA_DATA_PATH=$CHA_DATA_PATH $CHA_DEPLOY_PATH/add_user.sh USER"
echo "ChaWeb files: $static"
echo "ChaWeb needs one nginx server block per user: adapt $CHA_DEPLOY_PATH/nginx-chaweb.conf.example"
echo "After an upgrade, restart running daemons: sudo systemctl try-restart 'cha@*.service'"
