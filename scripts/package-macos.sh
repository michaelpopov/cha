#!/bin/sh

set -eu

if [ "$(uname -s)" != "Darwin" ]; then
    echo "package: the macOS package must be built on macOS" >&2
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

repository=$(cd -- "$(dirname -- "$0")/.." && pwd)
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
    echo "package: install the pinned Node.js version and try again" >&2
    exit 2
fi

destination="$output_parent/cha-macos-$version"
temporary="$output_parent/.cha-macos-$version.tmp.$$"
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

echo "==> Installing the Playwright Chromium browser"
(cd "$webapp" && npx playwright install chromium)

echo "==> Checking generated API types and browser application"
(cd "$webapp" && npm run check)

echo "==> Building production browser files"
(cd "$webapp" && npm run build)

echo "==> Building macOS chaweb"
cmake -S "$repository" -B "$native_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DOPENSSL_USE_STATIC_LIBS=TRUE
cmake --build "$native_build" --target chaweb_app

echo "==> Assembling clean application directory"
cmake -E remove_directory "$temporary"
mkdir -p "$temporary/web"
cp "$native_build/chaweb" "$temporary/chaweb"
cp "$repository/bin/start-cha.sh" "$temporary/start-cha.sh"
cp "$repository/packaging/linux/cha.toml.example" "$temporary/cha.toml.example"
cp -R "$repository/packaging/linux/import-seed" "$temporary/import-seed"
cp -R "$webapp/dist/." "$temporary/web/"
chmod 755 "$temporary"
chmod 755 "$temporary/chaweb" "$temporary/start-cha.sh"
chmod -R u=rwX,go=rX "$temporary/web" "$temporary/import-seed"
chmod 600 "$temporary/import-seed/.env"

"$repository/scripts/check-linux-package.sh" "$temporary"

# A Homebrew OpenSSL path would make the archive work only on the build host.
# The package may depend on libraries shipped with macOS, but nothing else.
external_dependency=$(otool -L "$temporary/chaweb" | awk '
    NR == 1 { next }
    {
        path = $1
        if (path !~ /^\/usr\/lib\// && path !~ /^\/System\/Library\//) {
            print path
            exit
        }
    }
')
if [ -n "$external_dependency" ]; then
    echo "package check: chaweb has an external dependency: $external_dependency" >&2
    exit 1
fi

echo "==> Testing the assembled application through production chaweb"
(cd "$webapp" && \
    CHA_E2E_APPLICATION_ROOT="$temporary" \
    npx playwright test --project=served)

echo "==> Verifying application replacement preserves an existing database"
"$repository/scripts/test-linux-package-upgrade.sh" "$temporary"

if [ -e "$destination" ]; then
    cmake -E remove_directory "$destination"
fi
mv "$temporary" "$destination"
trap - EXIT HUP INT TERM

echo "==> Writing the distribution archive"
COPYFILE_DISABLE=1 tar -czf "$destination.tar.gz" \
    -C "$output_parent" "cha-macos-$version"

echo "macOS application package: $destination"
echo "macOS distribution archive: $destination.tar.gz"
