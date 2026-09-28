#!/bin/sh
set -eu

fail() { echo "add_user.sh: $*" >&2; exit 2; }

[ "$#" -eq 1 ] || fail "usage: add_user.sh USER"
name=$1
case "$name" in
    ''|[!a-z]*|*[!a-z0-9_-]*) fail "USER must start with a lowercase letter and contain only lowercase letters, digits, underscores, or hyphens" ;;
esac
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

if [ "$(id -u)" -ne 0 ]; then
    exec sudo env CHA_DEPLOY_PATH="$CHA_DEPLOY_PATH" \
        CHA_DATA_PATH="$CHA_DATA_PATH" "$0" "$name"
fi

[ -x "$CHA_DEPLOY_PATH/cha-daemon" ] || fail "run install.sh first"
[ -d "$CHA_DEPLOY_PATH/cha-config.example/config" ] || fail "missing example vault"
[ -f /etc/systemd/system/cha@.service ] || fail "run install.sh first"
[ -f /etc/nginx/cha-users.map ] || fail "run install.sh first"
[ -f /etc/nginx/conf.d/cha.conf ] || fail "run install.sh first"

owner=$(sed -n 's/^User=//p' /etc/systemd/system/cha@.service | head -n 1)
[ -n "$owner" ] && [ "$owner" != 'cha-%i' ] || \
    fail "cha@.service needs an installing user in its User= setting"
owner_uid=$(id -u "$owner") || fail "unknown service user: $owner"
[ "$owner_uid" -ne 0 ] || fail "cha@.service must run as a regular user"

user_data="$CHA_DATA_PATH/$name"
config="$user_data/config"
if [ ! -e "$config" ]; then
    mkdir -p -- "$config"
    cp -R "$CHA_DEPLOY_PATH/cha-config.example/config/." "$config/"
fi
[ -d "$config" ] || fail "configuration path is not a directory: $config"
chown -R "$owner:" "$user_data"
chmod 700 "$user_data" "$config"
runuser -u "$owner" -- test -x "$CHA_DEPLOY_PATH/cha-daemon" || \
    fail "$owner cannot execute cha-daemon"
runuser -u "$owner" -- test -w "$config" || \
    fail "$owner cannot write its configuration directory"

map=/etc/nginx/cha-users.map
key=$(sed -n "s/^\"Bearer \([0-9a-f]\{64\}\)\"[[:space:]][[:space:]]*$name;[[:space:]]*$/\1/p" "$map" | head -n 1)
if [ -z "$key" ]; then
    key=$(od -An -N32 -tx1 /dev/urandom | tr -d ' \n')
    printf '"Bearer %s" %s;\n' "$key" "$name" >> "$map"
fi
nginx -t
if systemctl is-active --quiet nginx; then
    systemctl reload nginx
fi
systemctl enable --now "cha@$name.socket"
echo "API key for $name: $key"
echo "Ready: cha@$name.socket"
