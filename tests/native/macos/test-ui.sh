#!/bin/sh

set -eu

if [ "$(uname -s)" != Darwin ]; then
    echo "test-ui is currently supported only on macOS" >&2
    exit 2
fi

repository=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repository"

npm --prefix webapp run build
cmake --preset ninja
cmake --build --preset ninja --target cha_macos_runtime cha_prepare_test_vault

temporary=$(mktemp -d "${TMPDIR:-/tmp}/cha-ui.XXXXXX")
trap 'rm -rf "$temporary"' EXIT
trap 'exit 1' INT TERM

export CHA_NATIVE_ASSETS="$repository/webapp/dist"
export CHA_NATIVE_PARITY_SCRIPT="$repository/tests/native/ui.js"
export CHA_NATIVE_TEST_HOST="$temporary/ChaNativeTestHost.app"
export CHA_RUNTIME_LIB="$repository/build/ninja/libChaRuntime.dylib"

for width in 1200 430; do
    echo "UI smoke test: ${width}px"
    export CHA_NATIVE_TEST_CONFIG="$temporary/vault-$width"
    "$repository/build/ninja/cha_prepare_test_vault" --folder-stores "$CHA_NATIVE_TEST_CONFIG"
    CHA_NATIVE_TEST_WIDTH="$width" ./tests/native/macos/run.sh parity
    export CHA_NATIVE_TEST_CONFIG="$temporary/references-$width"
    "$repository/build/ninja/cha_prepare_test_vault" --file-references "$CHA_NATIVE_TEST_CONFIG"
    CHA_NATIVE_PARITY_SCRIPT="$repository/tests/native/file-references.js" \
        CHA_NATIVE_TEST_WIDTH="$width" ./tests/native/macos/run.sh parity
done
