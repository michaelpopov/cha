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

need_site=0
if [ ! -e /etc/nginx/conf.d/cha.conf ] && [ ! -L /etc/nginx/conf.d/cha.conf ]; then
    need_site=1
    CHA_LISTEN=${CHA_LISTEN:-127.0.0.1:8086}
    case "$CHA_LISTEN" in
        *[!0-9.:]*) fail "invalid CHA_LISTEN" ;;
    esac
fi

if [ "$(id -u)" -ne 0 ]; then
    install_user=$(id -un)
    exec sudo env \
        CHA_DEPLOY_PATH="$CHA_DEPLOY_PATH" CHA_DATA_PATH="$CHA_DATA_PATH" \
        CHA_LISTEN="${CHA_LISTEN-}" CHA_INSTALL_USER="$install_user" "$0"
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
for file in cha-daemon add_user.sh cha@.service cha@.socket; do
    [ -f "$source_dir/$file" ] || fail "missing $file from the extracted package"
done
[ -d "$source_dir/cha-config.example/config" ] || fail "missing example vault"
if [ "$need_site" -eq 1 ]; then
    [ -f "$source_dir/nginx.conf.install" ] || fail "missing nginx.conf.install"
fi
command -v nginx >/dev/null 2>&1 || fail "nginx is required"

mkdir -p -- "$CHA_DEPLOY_PATH" "$CHA_DATA_PATH"
install -m 755 "$source_dir/cha-daemon" "$CHA_DEPLOY_PATH/cha-daemon"
install -m 755 "$source_dir/add_user.sh" "$CHA_DEPLOY_PATH/add_user.sh"
mkdir -p -- "$CHA_DEPLOY_PATH/cha-config.example"
cp -R "$source_dir/cha-config.example/." "$CHA_DEPLOY_PATH/cha-config.example/"

temporary=$(mktemp -d)
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM
sed -e "s|@CHA_DEPLOY_PATH@|$CHA_DEPLOY_PATH|g" \
    -e "s|@CHA_DATA_PATH@|$CHA_DATA_PATH|g" \
    -e "s|@CHA_INSTALL_USER@|$install_user|g" \
    "$source_dir/cha@.service" > "$temporary/cha@.service"
if [ "$need_site" -eq 1 ]; then
    sed -e "s|@CHA_LISTEN@|$CHA_LISTEN|g" \
        "$source_dir/nginx.conf.install" > "$temporary/cha.conf"
fi

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

if [ ! -e /etc/nginx/cha-users.map ]; then
    install -m 600 /dev/null /etc/nginx/cha-users.map
fi
if [ "$need_site" -eq 0 ]; then
    echo "Keeping existing /etc/nginx/conf.d/cha.conf"
else
    install -m 644 "$temporary/cha.conf" /etc/nginx/conf.d/cha.conf
fi
nginx -t
if systemctl is-active --quiet nginx; then
    systemctl reload nginx
fi
echo "Installed CHA. Run CHA_DEPLOY_PATH=$CHA_DEPLOY_PATH CHA_DATA_PATH=$CHA_DATA_PATH $CHA_DEPLOY_PATH/add_user.sh USER"
