#!/bin/sh
# Starts CHA from this directory.
#
# This directory is laid out the way a real installation is: the executable and
# web/ live here, the workspace lives somewhere else. The build copies chaweb
# and web/ into this directory; nothing here is edited by hand except the three
# settings below.
#
# Requires a chaweb that accepts --root, --workspace, --host, and --port.

HOST=127.0.0.1
PORT=8888
WORKSPACE=../workspace

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

echo "CHA on http://$HOST:$PORT/  workspace: $workspace"
echo "Press Ctrl+C to stop."

exec "$here/chaweb" \
    --root "$here" \
    --workspace "$workspace" \
    --host "$HOST" \
    --port "$PORT"
