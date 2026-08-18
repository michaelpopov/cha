#!/bin/sh
HOST='0.0.0.0'
PORT='8086'
WORKSPACE='../workspace'

# Starts CHA from this directory.
#
# This directory is laid out the way a real installation is: the executable,
# web/, and (in deployment packages) the default workspace live here. The build
# copies chaweb and web/ into this directory; nothing here is edited by hand
# except the three settings above.
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

# A chaweb left over from an earlier start still holds the port, and the new one
# would die on bind with nothing but a line in the log to say why. Stop that
# server first and wait for it to go.
#
# Needs lsof or fuser to find the listener; without either, this cannot look and
# the start proceeds as it did before.
listening_pids() {
    if command -v lsof >/dev/null 2>&1; then
        lsof -t -i "TCP:$PORT" -sTCP:LISTEN 2>/dev/null || true
    elif command -v fuser >/dev/null 2>&1; then
        fuser -n tcp "$PORT" 2>/dev/null || true
    fi
}

# Only a chaweb is ours to stop. Anything else on this port is reported and left
# alone, because taking down an unrelated server would be worse than not starting.
ours=''
foreign=''
for pid in $(listening_pids); do
    case "$(ps -p "$pid" -o comm= 2>/dev/null || true)" in
        chaweb) ours="$ours $pid" ;;
        '') ;;
        *) foreign="$foreign $pid" ;;
    esac
done

if [ -n "$foreign" ]; then
    echo "start-cha: port $PORT is held by another program (PID$foreign)" >&2
    echo "start-cha: stop it, or set PORT at the top of this script" >&2
    exit 1
fi

for pid in $ours; do
    echo "start-cha: stopping the chaweb already on port $PORT (PID $pid)"
    kill "$pid" 2>/dev/null || continue
    waited=0
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$waited" -ge 100 ]; then
            echo "start-cha: PID $pid did not exit after 10 seconds" >&2
            exit 1
        fi
        sleep 0.1
        waited=$((waited + 1))
    done
done

url_host=$HOST
case "$url_host" in
    *:*) url_host="[$url_host]" ;;
esac
# Started with nohup in the background so the server keeps running after the
# terminal that launched it is closed. Output goes to the log beside it.
log="$here/chaweb.log"
nohup "$here/chaweb" \
    --root "$here" \
    --workspace "$workspace" \
    --host "$HOST" \
    --port "$PORT" \
    >>"$log" 2>&1 &
server_pid=$!

echo "CHA: http://$url_host:$PORT/"
echo "Running in the background as PID $server_pid; log: $log"
echo "Stop it with: kill $server_pid"
