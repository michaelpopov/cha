#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <application-directory>" >&2
    exit 2
fi

repository=$(cd -- "$(dirname -- "$0")/.." && pwd)
source_application=$(cd -- "$1" && pwd)
# Match the browser suite's collision-resistant default while keeping an
# explicit override for controlled packaging environments.
port=${CHA_UPGRADE_TEST_PORT:-$((20000 + ($$ % 20000)))}
test_root=$(mktemp -d)
workspace="$test_root/cha-workspace"
application="$test_root/cha"
server_pid=

stop_server() {
    if [ -n "$server_pid" ] && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid"
        wait "$server_pid"
    fi
    server_pid=
}

cleanup() {
    stop_server || true
    cmake -E remove_directory "$test_root"
}
trap cleanup EXIT HUP INT TERM

start_server() {
    "$application/chaweb" \
        --root "$application" \
        --workspace "$workspace" \
        --host 127.0.0.1 \
        --port "$port" \
        >"$test_root/chaweb.out" 2>&1 &
    server_pid=$!

    attempt=0
    while [ "$attempt" -lt 100 ]; do
        if curl --silent --fail "http://127.0.0.1:$port/health" >/dev/null; then
            return
        fi
        if ! kill -0 "$server_pid" 2>/dev/null; then
            sed -n '1,120p' "$test_root/chaweb.out" >&2
            return 1
        fi
        attempt=$((attempt + 1))
        sleep 0.05
    done
    echo "upgrade test: chaweb did not become ready" >&2
    return 1
}

cp -R "$repository/webapp/e2e/fixtures/workspace" "$workspace"
cp "$repository/webapp/e2e/fixtures/provider.env" "$workspace/.env"
cp -R "$source_application" "$application"

start_server
origin="http://127.0.0.1:$port"
created=$(curl --silent --fail \
    -H "Origin: $origin" \
    -H 'Content-Type: application/json' \
    --data '{"label":"Upgrade survivor"}' \
    "$origin/api/v1/forums/lobby/sessions")
session_id=$(printf '%s' "$created" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')
if [ -z "$session_id" ]; then
    echo "upgrade test: create response has no session ID" >&2
    exit 1
fi
curl --silent --fail \
    -H "Origin: $origin" \
    -H 'Content-Type: application/json' \
    --data '{}' \
    "$origin/api/v1/forums/lobby/sessions/$session_id/open" >/dev/null
stop_server

database="$workspace/forums/lobby/sessions/$session_id.sqlite3"
if [ ! -f "$database" ]; then
    echo "upgrade test: stored session database was not created" >&2
    exit 1
fi
owned_before=$(sha256sum \
    "$workspace/.env" \
    "$workspace/characters/guide/CHARACTER.md" \
    "$database")

# This is the documented upgrade: the whole application directory is
# replaced, while the sibling workspace is not part of the operation.
cmake -E remove_directory "$application"
cp -R "$source_application" "$application"

owned_after=$(sha256sum \
    "$workspace/.env" \
    "$workspace/characters/guide/CHARACTER.md" \
    "$database")
if [ "$owned_before" != "$owned_after" ]; then
    echo "upgrade test: customer-owned workspace files changed during replacement" >&2
    exit 1
fi

start_server
sessions=$(curl --silent --fail "$origin/api/v1/forums/lobby/sessions")
if ! printf '%s' "$sessions" | grep -q '"label":"Upgrade survivor"'; then
    echo "upgrade test: stored session was not visible after replacement" >&2
    exit 1
fi
curl --silent --fail \
    -H "Origin: $origin" \
    -H 'Content-Type: application/json' \
    --data '{}' \
    "$origin/api/v1/forums/lobby/sessions/$session_id/open" >/dev/null
curl --silent --fail \
    "$origin/s/lobby/$session_id/api/v1/session" \
    | grep -q '"session_label":"Upgrade survivor"'
stop_server

echo "Package upgrade check passed: workspace key, character, and stored session survived."
