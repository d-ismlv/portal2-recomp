#!/bin/bash
# Construit la sonde iOS (ios/probe) : build/ios/<cible>/probe.app.
#   tools/build_ios_probe.sh [sim|device]
# Appareil : signée par ios/signing/sign.py (profil obtenu par Xcode).
# Lancement : P2_PROBE_CLIP=1 (test de découpe au plan proche, ES 3 brut).
set -euo pipefail
cd "$(dirname "$0")/.."
TARGET=${1:-sim}
case "$TARGET" in
  sim)    SDK=iphonesimulator; TRIPLE=arm64-apple-ios17.0-simulator; PLATFORM=ios-simulator; PLIST=iPhoneSimulator ;;
  device) SDK=iphoneos; TRIPLE=arm64-apple-ios17.0; PLATFORM=ios; PLIST=iPhoneOS ;;
  *) echo "usage: $0 [sim|device]"; exit 2 ;;
esac
DEPS=${P2_IOS_DEPS:-$(cd .. && pwd)/work/ios-deps}/$TARGET
APP=build/ios/$TARGET/probe.app
mkdir -p "$APP"
xcrun --sdk $SDK clang -target $TRIPLE -isysroot "$(xcrun --sdk $SDK --show-sdk-path)" -O1 -DGLES_SILENCE_DEPRECATION \
  -I"$DEPS/include/SDL2" -I"$DEPS/include/gl4es" ios/probe/probe.c ios/probe/clip_test.c \
  "$DEPS/lib/libSDL2main.a" "$DEPS/lib/libSDL2.a" "$DEPS/lib/libgl4es.a" \
  $(for f in UIKit Foundation CoreGraphics QuartzCore OpenGLES Metal AVFoundation AudioToolbox CoreAudio CoreMotion \
             GameController CoreHaptics CoreBluetooth; do echo -framework $f; done) \
  -Wl,-platform_version,$PLATFORM,17.0,18.0 -o "$APP/probe"
sed -e 's/@EXECUTABLE@/probe/' -e 's/@IDENTIFIER@/$(python3 tools/ios_bundle_id.py --probe)/' -e 's/@NAME@/P2 Probe/' \
    -e "s/@PLATFORM@/$PLIST/" ios/Info.plist.in > "$APP/Info.plist"
if [ "$TARGET" = sim ]; then codesign -s - -f "$APP" >/dev/null 2>&1; else python3 ios/signing/sign.py "$APP"; fi
echo "$APP"
