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
mkdir -p "$contents/MacOS"
xcrun swiftc \
    -swift-version 5 \
    -parse-as-library \
    -O \
    -target "arm64-apple-macos$deployment_target" \
    -framework AppKit \
    -framework WebKit \
    "$repository/packaging/macos/feasibility.swift" \
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
exec "$contents/MacOS/ChaNativeTestHost" --assets "$assets" --expect "$expect" "$@"
