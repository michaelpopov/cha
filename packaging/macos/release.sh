#!/bin/sh
# Copy this file to ~/bin/cha-update on target Macs. No checkout is needed.
set -eu
umask 077

fail() { echo "CHA release: $*" >&2; exit 1; }
config=${CHA_RELEASE_CONFIG:-"$HOME/.config/cha/releases.conf"}
[ -f "$config" ] || fail "create $config from releases.conf.example first"
. "$config"
: "${CHA_RELEASE_BUCKET_URL:?set CHA_RELEASE_BUCKET_URL in releases.conf}"
: "${CHA_RELEASE_ACCESS_KEY_ID:?set CHA_RELEASE_ACCESS_KEY_ID in releases.conf}"
: "${CHA_RELEASE_SECRET_KEY:?set CHA_RELEASE_SECRET_KEY in releases.conf}"
[ "${#CHA_RELEASE_ACCESS_KEY_ID}" -eq 32 ] || fail "R2 access-key ID in $config must be 32 characters (found ${#CHA_RELEASE_ACCESS_KEY_ID}); copy the full access-key ID from your R2 credentials"
bucket=${CHA_RELEASE_BUCKET_URL%/}
case "$bucket" in https://*) ;; *) fail "bucket URL must use HTTPS" ;; esac
mode=${1:-install}
case "$mode" in
    upload) [ "$#" -eq 3 ] || fail "usage: $0 upload VERSION ARCHIVE" ;;
    install) [ "$#" -le 1 ] || fail "usage: $0 [install]" ;;
    check) [ "$#" -eq 1 ] || fail "usage: $0 check" ;;
    *) fail "usage: $0 [install | check | upload VERSION ARCHIVE]" ;;
esac

temporary=$(mktemp -d "${TMPDIR:-/tmp}/cha-release.XXXXXX")
lock=
stage=
backup=
install_parent=${CHA_INSTALL_DIR:-/Applications}
destination="$install_parent/CHA.app"
as_admin() {
    if [ -w "$install_parent" ]; then "$@"; else sudo "$@"; fi
}
cleanup() {
    # Restore the old app if replacing it failed or was interrupted.
    if [ -n "$backup" ] && [ -d "$backup" ] && [ ! -e "$destination" ]; then
        as_admin mv "$backup" "$destination"
    fi
    [ -z "$stage" ] || as_admin rm -rf "$stage"
    [ -z "$lock" ] || as_admin rmdir "$lock"
    rm -rf "$temporary"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' HUP TERM

# Keep credentials out of the process arguments. R2 uses S3 signing, region auto.
case "$CHA_RELEASE_ACCESS_KEY_ID:$CHA_RELEASE_SECRET_KEY" in
    *[!A-Za-z0-9:/+=_-]*) fail "invalid characters in R2 credentials" ;;
esac
printf 'user = "%s:%s"\n' "$CHA_RELEASE_ACCESS_KEY_ID" "$CHA_RELEASE_SECRET_KEY" > "$temporary/curl.conf"
r2() {
    curl --config "$temporary/curl.conf" --aws-sigv4 aws:amz:auto:s3 \
        --fail --silent --show-error --retry 3 --connect-timeout 20 "$@"
}
hash() { shasum -a 256 "$1" | cut -d ' ' -f1; }
valid_version() {
    case "$1" in ''|*[!A-Za-z0-9._-]*) fail "invalid release version" ;; esac
}

if [ "$mode" = check ]; then
    curl --help all > "$temporary/curl-help"
    # Use grep here so target Macs need no developer tools.
    grep -q -- '--aws-sigv4' "$temporary/curl-help" || fail "curl 7.75 or newer is required"
    echo "Release configuration is ready"
    exit 0
fi

if [ "$mode" = upload ]; then
    version=$2
    archive=$3
    valid_version "$version"
    [ -f "$archive" ] || fail "archive not found: $archive"
    plist="$temporary/Info.plist"
    tar -xOzf "$archive" CHA.app/Contents/Info.plist > "$plist"
    [ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$plist")" = "$version" ] || fail "local app version does not match release version"
    minimum=$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$plist")
    checksum=$(hash "$archive")
    object="packages/macos/CHA-$version-$checksum.tar.gz"
    echo "Uploading CHA $version"
    r2 --header "x-amz-content-sha256: $checksum" --upload-file "$archive" "$bucket/$object"
    printf '%s\n%s\n%s\n' "$version" "$checksum" "$minimum" > "$temporary/latest.txt"
    # Publish last: readers always see a complete archive, including on retries.
    r2 --header "x-amz-content-sha256: $(hash "$temporary/latest.txt")" \
        --header 'Cache-Control: no-store' --upload-file "$temporary/latest.txt" \
        "$bucket/packages/macos/latest.txt"
    echo "Published CHA $version"
    exit 0
fi

[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || fail "CHA requires an Apple Silicon Mac"
r2 --output "$temporary/latest.txt" "$bucket/packages/macos/latest.txt"
version=$(sed -n '1p' "$temporary/latest.txt")
checksum=$(sed -n '2p' "$temporary/latest.txt")
minimum=$(sed -n '3p' "$temporary/latest.txt")
valid_version "$version"
[ "${#checksum}" -eq 64 ] || fail "invalid release checksum"
case "$checksum" in *[!a-f0-9]*) fail "invalid release checksum" ;; esac
case "$minimum" in ''|*[!0-9.]*) fail "invalid minimum macOS version" ;; esac
current=$(sw_vers -productVersion)
awk -v current="$current" -v minimum="$minimum" 'BEGIN {
    split(current, c, "."); split(minimum, m, ".");
    for (i=1; i<=3; i++) {
        if (c[i]+0 > m[i]+0) exit 0;
        if (c[i]+0 < m[i]+0) exit 1;
    }
}' || fail "CHA $version requires macOS $minimum or newer (this Mac: $current)"
echo "Downloading CHA $version"
r2 --output "$temporary/app.tar.gz" "$bucket/packages/macos/CHA-$version-$checksum.tar.gz"
[ "$(hash "$temporary/app.tar.gz")" = "$checksum" ] || fail "download checksum mismatch"
# Reject archive paths outside the bundle before extraction.
tar -tzf "$temporary/app.tar.gz" > "$temporary/entries"
while IFS= read -r entry; do
    case "$entry" in CHA.app|CHA.app/*) ;; *) fail "archive contains an unexpected path" ;; esac
    case "/$entry/" in */../*) fail "archive contains a parent path" ;; esac
done < "$temporary/entries"
tar -xzf "$temporary/app.tar.gz" -C "$temporary"
app="$temporary/CHA.app"
[ -d "$app" ] && [ ! -L "$app" ] || fail "archive does not contain CHA.app"
plist="$app/Contents/Info.plist"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$plist")" = com.michaelpopov.cha ] || fail "wrong application in archive"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$plist")" = "$version" ] || fail "archive version mismatch"
[ "$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$plist")" = "$minimum" ] || fail "archive macOS requirement mismatch"
codesign --verify --deep --strict "$app"

# Serialize installs and copy completely before touching the installed app.
[ -d "$install_parent" ] || fail "installation directory does not exist: $install_parent"
as_admin mkdir "$install_parent/.CHA-update.lock" || fail "another update is running; check $install_parent/.CHA-update.lock"
lock="$install_parent/.CHA-update.lock"
stage="$install_parent/.CHA-update.$$"
backup="$install_parent/.CHA-backup.$$"
as_admin ditto "$app" "$stage"
if pgrep -x CHA >/dev/null; then
    osascript -e 'tell application id "com.michaelpopov.cha" to quit'
    attempts=0
    while pgrep -x CHA >/dev/null; do
        attempts=$((attempts + 1))
        [ "$attempts" -lt 30 ] || fail "CHA did not quit; close it and run this script again"
        sleep 1
    done
fi
if [ -e "$destination" ]; then as_admin mv "$destination" "$backup"; fi
as_admin mv "$stage" "$destination"
stage=
as_admin rm -rf "$backup"
backup=
echo "Installed CHA $version in $destination"
open "$destination"
