#!/bin/sh
HOST='0.0.0.0'
PORT='8086'
WORKSPACE='../workspace'

# Starts CHA from this directory.
#
# This directory is laid out the way a real installation is: the executable and
# web/ live here, the workspace lives somewhere else. The build copies chaweb
# and web/ into this directory; nothing here is edited by hand except the three
# settings above.
#
# These are the development values: loopback, and the repository's own
# workspace. scripts/package-linux.sh ships this same script with the three
# settings replaced by the customer ones in packaging/linux/app.toml, so the
# behavior below has one source and the settings have one source each.
#
# Requires a chaweb that accepts --root, --workspace, --host, and --port.

set -eu

here=$(cd -- "$(dirname -- "$0")" && pwd)

case "$WORKSPACE" in
    /*) workspace_path="$WORKSPACE" ;;
    *) workspace_path="$here/$WORKSPACE" ;;
esac

if [ ! -x "$here/chaweb" ]; then
    echo "start-cha: no executable at $here/chaweb" >&2
    echo "start-cha: build the project; the build copies chaweb here" >&2
    exit 1
fi

if [ ! -d "$workspace_path" ]; then
    echo "start-cha: no workspace at $workspace_path" >&2
    echo "start-cha: set WORKSPACE at the top of this script" >&2
    exit 1
fi

workspace=$(cd -- "$workspace_path" && pwd)

url_host=$HOST
case "$url_host" in
    *:*) url_host="[$url_host]" ;;
esac
echo "CHA: http://$url_host:$PORT/"
echo "Press Ctrl+C to stop."

exec "$here/chaweb" \
    --root "$here" \
    --workspace "$workspace" \
    --host "$HOST" \
    --port "$PORT"
