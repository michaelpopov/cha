#!/bin/sh
set -eu

if [ "$(uname -s)" != Linux ]; then
    echo "package-linux must be run on Linux" >&2
    exit 2
fi

if [ "$#" -ne 1 ]; then
    echo "usage: $0 VERSION" >&2
    exit 2
fi
version=$1
case "$version" in
    ''|*[!A-Za-z0-9._-]*)
        echo "version may contain only letters, digits, dots, underscores, and hyphens" >&2
        exit 2
        ;;
esac

repository=$(cd -- "$(dirname -- "$0")/../.." && pwd)
build="$repository/build/package-linux"
output="$repository/packages"
name="cha-linux-$version"
mkdir -p "$output"
temporary=$(mktemp -d "$output/.cha-linux.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT HUP INT TERM

if ! command -v npm >/dev/null 2>&1; then
    node_version=$(tr -d '[:space:]' < "$repository/webapp/.node-version")
    echo "package-linux: Node.js $node_version with npm is required" >&2
    exit 2
fi
npm --prefix "$repository/webapp" ci --no-audit
npm --prefix "$repository/webapp" run build:chaweb

# index.html names content-hashed files; each one must be in the build.
chaweb="$repository/webapp/dist-chaweb"
if [ ! -f "$chaweb/index.html" ] || [ ! -d "$chaweb/assets" ]; then
    echo "The ChaWeb build did not create index.html and assets/" >&2
    exit 1
fi
for asset in $(grep -o '"/assets/[^"]*"' "$chaweb/index.html" | tr -d '"'); do
    if [ ! -f "$chaweb$asset" ]; then
        echo "index.html refers to a missing file: $asset" >&2
        exit 1
    fi
done

cmake -S "$repository" -B "$build" -G Ninja \
    -UOPENSSL_CRYPTO_LIBRARY -UOPENSSL_SSL_LIBRARY \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTING=OFF \
    -DCHA_PACKAGE_VERSION="$version" \
    -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DCMAKE_DISABLE_FIND_PACKAGE_CURL=TRUE \
    -DCMAKE_EXE_LINKER_FLAGS='-static-libstdc++ -static-libgcc'
cmake --build "$build" --target cha-daemon cha_prepare_linux_config

# Only the target machine's C runtime and loader may be needed outside the archive.
for executable in "$build/cha-daemon" "$build/cha_prepare_linux_config"; do
    readelf -d "$executable" > "$temporary/dynamic"
    sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p' "$temporary/dynamic" |
    while IFS= read -r library; do
        case "$library" in
            libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|ld-linux*.so.*) ;;
            *) echo "Unexpected runtime library: $library" >&2; exit 1 ;;
        esac
    done
done

stage="$temporary/$name"
mkdir -p "$stage/cha-config.example"
cp "$build/cha-daemon" "$stage/cha-daemon"
"$build/cha_prepare_linux_config" \
    "$repository/packaging/shared/import-seed" \
    "$repository/packaging/shared/cha-config.example" \
    "$stage/cha-config.example/config"
rm -f -- "$stage/cha-config.example/config/cha.sqlite3.cha-lock"
mkdir "$stage/chaweb"
cp -R "$chaweb/." "$stage/chaweb/"
cp "$repository/packaging/linux/nginx-chaweb.conf.example" "$stage/"
cp "$repository/packaging/linux/cha@.service" "$stage/"
cp "$repository/packaging/linux/cha@.socket" "$stage/"
cp "$repository/packaging/linux/nginx.conf.example" "$stage/"
cp "$repository/packaging/linux/nginx.conf.install" "$stage/"
cp "$repository/packaging/linux/install.sh" "$stage/"
cp "$repository/packaging/linux/add_user.sh" "$stage/"
cp "$repository/packaging/linux/README.md" "$stage/"

tar -C "$temporary" -czf "$temporary/$name.tar.gz" "$name"
mv -- "$temporary/$name.tar.gz" "$output/$name.tar.gz"
echo "Created $output/$name.tar.gz"
