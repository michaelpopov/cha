#!/bin/sh

set -eu

repository=$(cd -- "$(dirname -- "$0")/../../.." && pwd)
assets=${CHA_NATIVE_ASSETS:-"$repository/webapp/dist"}
bundle=${CHA_NATIVE_TEST_HOST:-"$repository/build/ninja/ChaNativeTestHost.app"}
macos_major=$(sw_vers -productVersion | cut -d. -f1)
if [ "$macos_major" -lt 13 ]; then
    echo "native test host: macOS 13.3 or newer is required" >&2
    exit 2
elif [ "$macos_major" -eq 13 ]; then
    deployment_target=13.3
else
    deployment_target="$macos_major.0"
fi

contents="$bundle/Contents"
mkdir -p "$contents/MacOS" "$contents/Frameworks"
runtime_lib=${CHA_RUNTIME_LIB:-"$repository/build/ninja/libChaRuntime.dylib"}
if [ ! -f "$runtime_lib" ]; then
    echo "native test host: $runtime_lib is missing; build cha_macos_runtime first" >&2
    exit 2
fi
cp "$runtime_lib" "$contents/Frameworks/libChaRuntime.dylib"
xcrun swiftc \
    -swift-version 5 \
    -parse-as-library \
    -O \
    -target "arm64-apple-macos$deployment_target" \
    -framework AppKit \
    -framework WebKit \
    -import-objc-header "$repository/packaging/macos/runtime_bridge.h" \
    -L "$(dirname "$runtime_lib")" \
    -lChaRuntime \
    -Xlinker -rpath \
    -Xlinker @executable_path/../Frameworks \
    "$repository/packaging/macos/feasibility.swift" \
    "$repository/packaging/macos/native_bridge.swift" \
    "$repository/tests/native/macos/test_host.swift" \
    -o "$contents/MacOS/ChaNativeTestHost"

cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>ChaNativeTestHost</string>
    <key>CFBundleIdentifier</key>
    <string>com.michaelpopov.cha.nativetesthost</string>
    <key>CFBundleName</key>
    <string>ChaNativeTestHost</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>LSMinimumSystemVersion</key>
    <string>$deployment_target</string>
    <key>LSUIElement</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>CHA uses the microphone to transcribe speech into your message.</string>
</dict>
</plist>
EOF

expect=${1:-pass}
shift $(( $# > 0 ? 1 : 0 ))
vault=""
case "$expect" in
    flow|reload|renderer-fail|stall|quit)
        vault=${CHA_NATIVE_TEST_CONFIG:-}
        if [ -z "$vault" ]; then
            vault=$(mktemp -d "${TMPDIR:-/tmp}/cha-native-vault.XXXXXX")
            prepare=${CHA_PREPARE_TEST_VAULT:-"$repository/build/ninja/cha_prepare_test_vault"}
            if [ ! -x "$prepare" ]; then
                echo "native test host: $prepare is missing; build cha_prepare_test_vault first" >&2
                exit 2
            fi
            "$prepare" "$vault"
        fi
        ;;
esac
if [ -n "$vault" ]; then
    exec "$contents/MacOS/ChaNativeTestHost" --assets "$assets" --expect "$expect" --config "$vault" "$@"
fi
exec "$contents/MacOS/ChaNativeTestHost" --assets "$assets" --expect "$expect" "$@"
