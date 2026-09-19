#!/bin/sh

set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <config-directory>" >&2
    exit 2
fi

repository=$(cd -- "$(dirname -- "$0")/.." && pwd)
config=$(cd -- "$1" && pwd)
webapp="$repository/webapp"
origin="http://127.0.0.1:5173"

if [ ! -d "$webapp/node_modules" ]; then
    (cd "$webapp" && npm ci --no-audit)
fi

cmake --preset ninja >/dev/null
cmake --build --preset ninja --target cha_macos_runtime cha_prepare_test_vault

CHA_NATIVE_DEV=1 npm --prefix "$webapp" run dev:native &
vite_pid=$!
cleanup() {
    kill "$vite_pid" 2>/dev/null || true
    wait "$vite_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

attempt=0
while [ "$attempt" -lt 100 ]; do
    if curl --silent --fail --max-time 0.2 "$origin/" >/dev/null; then
        break
    fi
    attempt=$((attempt + 1))
    sleep 0.05
done
if [ "$attempt" -eq 100 ]; then
    echo "native dev: Vite did not become ready at $origin" >&2
    exit 1
fi

CHA_NATIVE_ASSETS="$webapp"
CHA_NATIVE_TEST_CONFIG="$config" \
    "$repository/tests/native/macos/run.sh" pass \
    --dev-origin "$origin" --config "$config"
