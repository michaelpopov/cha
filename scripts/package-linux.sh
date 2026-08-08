#!/bin/sh

set -eu

if [ "$(uname -s)" != "Linux" ]; then
    echo "package: the Linux package must be built on Linux" >&2
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

destination="$output_parent/cha-linux-$version"
temporary="$output_parent/.cha-linux-$version.tmp.$$"
native_build="$repository/build/package-linux"
webapp="$repository/webapp"

cleanup() {
    if [ -d "$temporary" ]; then
        cmake -E remove_directory "$temporary"
    fi
}
trap cleanup EXIT HUP INT TERM

echo "==> Installing locked browser build dependencies"
(cd "$webapp" && npm ci)

echo "==> Checking generated API types and browser application"
(cd "$webapp" && npm run check)

echo "==> Building production browser files"
(cd "$webapp" && npm run build)

echo "==> Building Linux chaweb"
cmake -S "$repository" -B "$native_build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF
cmake --build "$native_build" --target chaweb_app

echo "==> Assembling clean application directory"
cmake -E remove_directory "$temporary"
mkdir -p "$temporary/web"
cp "$native_build/chaweb" "$temporary/chaweb"
cp "$repository/packaging/linux/app.toml" "$temporary/app.toml"

# The launcher's behavior lives in one file, bin/start-cha.sh, whose committed
# settings are the development ones. The customer's settings live in one file,
# packaging/linux/app.toml. Substituting here is what keeps the shipped launcher
# and the shipped configuration from drifting apart; check-linux-package.sh
# rejects the result if this fails to replace all three.
package_setting() {
    sed -n "s/^$1[[:space:]]*=[[:space:]]*//p" "$repository/packaging/linux/app.toml" \
        | head -1 | sed 's/^"//; s/"$//'
}

# Launcher settings are shell source, so quote them as data. Rewriting whole
# assignment lines also avoids interpreting '&', '|', or backslashes from a
# configured workspace path as sed replacement syntax.
shell_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}
package_host=$(shell_quote "$(package_setting host)")
package_port=$(shell_quote "$(package_setting port)")
package_workspace=$(shell_quote "$(package_setting workspace)")
while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
        HOST=*) printf 'HOST=%s\n' "$package_host" ;;
        PORT=*) printf 'PORT=%s\n' "$package_port" ;;
        WORKSPACE=*) printf 'WORKSPACE=%s\n' "$package_workspace" ;;
        *) printf '%s\n' "$line" ;;
    esac
done <"$repository/bin/start-cha.sh" >"$temporary/start-cha.sh"
cp -R "$webapp/dist/." "$temporary/web/"
chmod 755 "$temporary"
chmod 755 "$temporary/chaweb" "$temporary/start-cha.sh"
chmod -R u=rwX,go=rX "$temporary/web" "$temporary/app.toml"

"$repository/scripts/check-linux-package.sh" "$temporary"

echo "==> Testing the assembled application through production chaweb"
(cd "$webapp" && \
    CHA_E2E_APPLICATION_ROOT="$temporary" \
    npx playwright test --project=served)

echo "==> Verifying application replacement preserves an existing workspace"
"$repository/scripts/test-linux-package-upgrade.sh" "$temporary"

# Reassembly always replaces the exact versioned target with the freshly
# validated temporary tree, so files from an older run cannot survive.
if [ -e "$destination" ]; then
    cmake -E remove_directory "$destination"
fi
mv "$temporary" "$destination"
trap - EXIT HUP INT TERM

echo "Linux application package: $destination"
