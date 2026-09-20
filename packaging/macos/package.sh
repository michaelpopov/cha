#!/bin/sh

set -eu

if [ "$(uname -s)" != "Darwin" ] || [ "$(uname -m)" != "arm64" ]; then
    echo "package: CHA.app must be built on an Apple Silicon Mac" >&2
    exit 2
fi

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 <version> [output-parent]" >&2
    exit 2
fi

version=$1
case "$version" in
    ''|*[!A-Za-z0-9._-]*)
        echo "package: version may contain only letters, digits, dots, underscores, and hyphens" >&2
        exit 2
        ;;
esac

# Homebrew's static OpenSSL is built for the host macOS generation. Advertising
# an older target would produce an application that may fail on the other Mac,
# so CHA.app runs on the macOS generation that built it, or newer, and building
# for an older Mac means building on one.
macos_major=$(sw_vers -productVersion | cut -d. -f1)
if [ "$macos_major" -lt 13 ]; then
    echo "package: CHA.app must be built on macOS 13.3 or newer" >&2
    exit 2
elif [ "$macos_major" -eq 13 ]; then
    deployment_target=13.3
else
    deployment_target="$macos_major.0"
fi

echo "==> Building CHA.app for macOS $deployment_target and newer"

repository=$(cd -- "$(dirname -- "$0")/../.." && pwd)
output_parent=${2:-"$repository/packages"}
mkdir -p "$output_parent"
output_parent=$(cd -- "$output_parent" && pwd)

if [ "$output_parent" = "/" ]; then
    echo "package: refusing to use the filesystem root as the output parent" >&2
    exit 2
fi

if ! command -v node >/dev/null 2>&1 \
    || ! command -v npm >/dev/null 2>&1 \
    || ! command -v npx >/dev/null 2>&1; then
    node_version=$(tr -d '[:space:]' < "$repository/webapp/.node-version")
    echo "package: Node.js $node_version with npm and npx is required" >&2
    exit 2
fi
if ! command -v xcrun >/dev/null 2>&1; then
    echo "package: the Xcode command line tools are required" >&2
    exit 2
fi

destination="$output_parent/CHA.app"
archive="$output_parent/CHA-macos-$version.tar.gz"
legacy_archive="$output_parent/CHA-macos-$version.zip"
temporary="$output_parent/.cha-macos-$version.tmp.$$"
archive_contents="$temporary/archive"
application="$archive_contents/CHA.app"
contents="$application/Contents"
resources="$contents/Resources"
native_build="$repository/build/package-macos"
webapp="$repository/webapp"

cleanup() {
    if [ -d "$temporary" ]; then
        cmake -E remove_directory "$temporary"
    fi
}
trap cleanup EXIT HUP INT TERM

echo "==> Installing locked browser build dependencies"
(cd "$webapp" && npm ci --no-audit)

echo "==> Checking generated API types and browser application"
(cd "$webapp" && npm run check)

echo "==> Building production browser files"
(cd "$webapp" && npm run build)

echo "==> Building Apple Silicon CHA runtime"
cmake -S "$repository" -B "$native_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
    -DBUILD_TESTING=OFF \
    -DCHA_PACKAGE_VERSION="$version" \
    -DOPENSSL_USE_STATIC_LIBS=TRUE
cmake --build "$native_build" --target cha_macos_runtime

echo "==> Assembling CHA.app"
cmake -E remove_directory "$temporary"
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$resources/web"

xcrun swiftc \
    -swift-version 5 \
    -parse-as-library \
    -O \
    -target "arm64-apple-macos$deployment_target" \
    -framework AppKit \
    -framework WebKit \
    -import-objc-header "$repository/packaging/macos/runtime_bridge.h" \
    -L "$native_build" \
    -lChaRuntime \
    -Xlinker -rpath \
    -Xlinker @executable_path/../Frameworks \
    "$repository/packaging/macos/main.swift" \
    "$repository/packaging/macos/feasibility.swift" \
    "$repository/packaging/macos/native_bridge.swift" \
    -o "$contents/MacOS/CHA"

sed -e "s/@VERSION@/$version/g" \
    -e "s/@MINIMUM_SYSTEM_VERSION@/$deployment_target/g" \
    "$repository/packaging/macos/Info.plist.in" \
    > "$contents/Info.plist"
cp "$native_build/libChaRuntime.dylib" "$contents/Frameworks/libChaRuntime.dylib"
cp -R "$webapp/dist/." "$resources/web/"

iconset="$temporary/AppIcon.iconset"
xcrun swift "$repository/packaging/macos/make-icon.swift" "$iconset"
iconutil -c icns "$iconset" -o "$resources/AppIcon.icns"

chmod 755 "$contents/MacOS/CHA" "$contents/Frameworks/libChaRuntime.dylib"
chmod -R u=rwX,go=rX "$resources/web"

echo "==> Checking application bundle"
plutil -lint "$contents/Info.plist"
for executable in "$contents/MacOS/CHA" "$contents/Frameworks/libChaRuntime.dylib"; do
    if ! file "$executable" | grep -q 'arm64'; then
        echo "package check: $(basename "$executable") is not an Apple Silicon binary" >&2
        exit 1
    fi
done
icon_width=$(sips -g pixelWidth "$iconset/icon_16x16.png" \
    | awk '/pixelWidth/ { print $2 }')
if [ "$icon_width" != "16" ]; then
    echo "package check: icon representations are not their named pixel size" >&2
    exit 1
fi
if [ ! -f "$resources/web/index.html" ]; then
    echo "package check: application resources are incomplete" >&2
    exit 1
fi
if find "$application" -type f \( \
    -name '*.sqlite3' -o -name '*.sqlite' -o -name '*.db' \
    -o -name '*-wal' -o -name '*-shm' -o -name '*-journal' \
    -o -name '*.cha-lock' -o -name '*.openai-auth.json' \
    -o -name 'openai-auth.json' \) -print -quit | grep -q .; then
    echo "package check: a database, sidecar, journal, or lock leaked into CHA.app" >&2
    exit 1
fi
if find "$application" \( \
    -type d -name node_modules \
    -o -type f \( -name node -o -name npm -o -name npx -o -name pi -o -name codex \) \
    \) -print -quit | grep -q .; then
    echo "package check: a Node, Pi, or Codex runtime dependency leaked into CHA.app" >&2
    exit 1
fi

# A Homebrew library path would make the application work only on the build host.
for executable in "$contents/MacOS/CHA" "$contents/Frameworks/libChaRuntime.dylib"; do
    external_dependency=$(otool -L "$executable" | awk '
        NR == 1 { next }
        {
            path = $1
            if (path !~ /^\/usr\/lib\// && path !~ /^\/System\/Library\// \
                && path != "@rpath/libChaRuntime.dylib") {
                print path
                exit
            }
        }
    ')
    if [ -n "$external_dependency" ]; then
        echo "package check: $(basename "$executable") has an external dependency: $external_dependency" >&2
        exit 1
    fi
done

if find "$application" -name chaweb -print -quit | grep -q .; then
    echo "package check: chaweb leaked into CHA.app" >&2
    exit 1
fi

echo "==> Testing the embedded native runtime from the assembled bundle"
bundle_test="$temporary/bundle-test"
bundle_config="$bundle_test/config"
mkdir -p "$bundle_config"
xcrun clang \
    -target "arm64-apple-macos$deployment_target" \
    -I "$repository/packaging/macos" \
    -L "$contents/Frameworks" \
    -lChaRuntime \
    "$repository/packaging/macos/runtime-smoke.c" \
    -o "$bundle_test/runtime-smoke"
DYLD_LIBRARY_PATH="$contents/Frameworks" "$bundle_test/runtime-smoke" \
    "$bundle_config" \
    "$resources"

echo "==> Rejecting development and automation hooks in the shipping binary"
if "$contents/MacOS/CHA" --dev-origin "http://127.0.0.1:5173" \
        >/dev/null 2>"$bundle_test/reject-dev.err"; then
    echo "package check: shipping CHA accepted --dev-origin" >&2
    exit 1
fi
if ! grep -q "does not accept command-line arguments" "$bundle_test/reject-dev.err"; then
    echo "package check: shipping CHA did not reject --dev-origin" >&2
    cat "$bundle_test/reject-dev.err" >&2
    exit 1
fi
if "$contents/MacOS/CHA" --http >/dev/null 2>"$bundle_test/reject-http.err"; then
    echo "package check: shipping CHA accepted --http" >&2
    exit 1
fi
if CHA_DEV_ORIGIN="http://127.0.0.1:5173" "$contents/MacOS/CHA" --feasibility \
        >/dev/null 2>"$bundle_test/reject-feasibility.err"; then
    echo "package check: shipping CHA accepted --feasibility" >&2
    exit 1
fi

echo "==> Exercising assembled assets through the native test host"
CHA_NATIVE_ASSETS="$resources/web" \
CHA_RUNTIME_LIB="$contents/Frameworks/libChaRuntime.dylib" \
    "$repository/tests/native/macos/run.sh" media --timeout-ms 20000
prepare="$repository/build/ninja/cha_prepare_test_vault"
if [ ! -x "$prepare" ]; then
    cmake --preset ninja
    cmake --build --preset ninja --target cha_prepare_test_vault
fi
CHA_NATIVE_ASSETS="$resources/web" \
CHA_RUNTIME_LIB="$contents/Frameworks/libChaRuntime.dylib" \
CHA_PREPARE_TEST_VAULT="$prepare" \
    "$repository/tests/native/macos/run.sh" parity --timeout-ms 60000
CHA_NATIVE_ASSETS="$resources/web" \
CHA_RUNTIME_LIB="$contents/Frameworks/libChaRuntime.dylib" \
CHA_PREPARE_TEST_VAULT="$prepare" \
    "$repository/tests/native/macos/run.sh" flow --timeout-ms 25000
CHA_NATIVE_ASSETS="$resources/web" \
CHA_RUNTIME_LIB="$contents/Frameworks/libChaRuntime.dylib" \
CHA_PREPARE_TEST_VAULT="$prepare" \
    "$repository/tests/native/macos/run.sh" reload --timeout-ms 60000
CHA_NATIVE_ASSETS="$resources/web" \
CHA_RUNTIME_LIB="$contents/Frameworks/libChaRuntime.dylib" \
CHA_PREPARE_TEST_VAULT="$prepare" \
    "$repository/tests/native/macos/run.sh" quit --timeout-ms 25000
cmake -E remove_directory "$bundle_test"

echo "==> Ad-hoc signing application"
codesign --force --sign - --timestamp=none "$contents/Frameworks/libChaRuntime.dylib"
codesign --force --sign - --timestamp=none "$application"
codesign --verify --deep --strict "$application"

echo "==> Writing distribution archive"
rm -f "$archive" "$legacy_archive"
COPYFILE_DISABLE=1 tar -czf "$archive" -C "$archive_contents" "CHA.app"

if [ -e "$destination" ]; then
    cmake -E remove_directory "$destination"
fi
mv "$application" "$destination"
cmake -E remove_directory "$temporary"
trap - EXIT HUP INT TERM

echo "macOS application: $destination"
echo "macOS archive: $archive"
echo "Runs on macOS $deployment_target and newer, on Apple Silicon."
