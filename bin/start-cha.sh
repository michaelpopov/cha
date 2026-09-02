#!/bin/sh

# The real configuration is customer-owned and lives outside this replaceable
# application directory. Relative paths in it are resolved from its directory.
CONFIG='~/var/workspace/cha.toml'
IMPORT_SEED='import-seed'

set -eu

here=$(cd -- "$(dirname -- "$0")" && pwd)

case "$CONFIG" in
    \~/*) config="$HOME/${CONFIG#\~/}" ;;
    /*) config="$CONFIG" ;;
    *) config="$here/$CONFIG" ;;
esac
config_parent=$(dirname -- "$config")
if [ ! -d "$config_parent" ]; then
    echo "start-cha: no configuration parent directory at $config_parent" >&2
    exit 1
fi
config=$(cd -- "$config_parent" && pwd)/$(basename -- "$config")

case "$IMPORT_SEED" in
    /*) import_seed="$IMPORT_SEED" ;;
    *) import_seed="$here/$IMPORT_SEED" ;;
esac

if [ ! -x "$here/chaweb" ]; then
    echo "start-cha: no executable at $here/chaweb" >&2
    exit 1
fi
if [ ! -f "$config" ]; then
    echo "start-cha: no configuration file at $config" >&2
    echo "start-cha: copy and edit the packaged example:" >&2
    echo "  cp \"$here/cha.toml.example\" \"$config\"" >&2
    echo "start-cha: then initialize its database explicitly:" >&2
    echo "  \"$here/chaweb\" --config=\"$config\" --import \"$import_seed\"" >&2
    exit 1
fi

log="$here/chaweb.log"
nohup "$here/chaweb" \
    --root "$here" \
    --config="$config" \
    >>"$log" 2>&1 &
server_pid=$!

# Catch immediate failures such as a malformed config, missing database, or an
# occupied port instead of reporting a dead background process as successful.
sleep 1
if ! kill -0 "$server_pid" 2>/dev/null; then
    wait "$server_pid" || status=$?
    echo "start-cha: chaweb failed to start (exit ${status:-1}); log: $log" >&2
    tail -n 20 "$log" >&2 || true
    exit "${status:-1}"
fi

echo "CHA is running in the background as PID $server_pid; log: $log"
echo "Stop it with: kill $server_pid"
