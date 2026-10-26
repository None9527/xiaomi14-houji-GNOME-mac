#!/bin/bash
# Builds XScreen.app from the SwiftPM product.
set -euo pipefail
cd "$(dirname "$0")"
swift build -c release 2>&1 | grep -E "error|Build complete"; [ ${PIPESTATUS[0]} -eq 0 ] || exit 1
app=build/XScreen.app
rm -rf "$app"
mkdir -p "$app/Contents/MacOS"
cp .build/release/XScreen "$app/Contents/MacOS/XScreen"
cat > "$app/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>XScreen</string>
  <key>CFBundleDisplayName</key><string>XScreen</string>
  <key>CFBundleIdentifier</key><string>local.xscreen.viewer</string>
  <key>CFBundleExecutable</key><string>XScreen</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
codesign --force --sign - "$app" >/dev/null 2>&1 || true
echo "built $app"
