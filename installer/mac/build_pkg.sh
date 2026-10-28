#!/bin/bash
# Builds "ours-blanc Drum Kit-<version>.pkg": app → /Applications, VST3 / AU → /Library/Audio/Plug-Ins.
# Signing / notarization run only when the Developer ID secrets are present (see .github/workflows/desktop.yml).
#   usage: build_pkg.sh <artefacts dir> <version> <out dir>
set -euo pipefail
ART="$1"; VERSION="$2"; OUT="$3"
NAME="ours-blanc Drum Kit"
WORK="$(mktemp -d)"
mkdir -p "$OUT" "$WORK/app" "$WORK/vst3" "$WORK/au"

cp -R "$ART/Standalone/$NAME.app" "$WORK/app/"
cp -R "$ART/VST3/$NAME.vst3" "$WORK/vst3/"
cp -R "$ART/AU/$NAME.component" "$WORK/au/"

if [ -n "${MAC_APP_IDENTITY:-}" ]; then
  for b in "$WORK/app/$NAME.app" "$WORK/vst3/$NAME.vst3" "$WORK/au/$NAME.component"; do
    codesign --force --deep --options runtime --timestamp --sign "$MAC_APP_IDENTITY" "$b"
  done
fi

pkgbuild --root "$WORK/app" --install-location /Applications \
  --identifier app.ours-blanc.drumkit.app --version "$VERSION" "$WORK/app.pkg"
pkgbuild --root "$WORK/vst3" --install-location /Library/Audio/Plug-Ins/VST3 \
  --identifier app.ours-blanc.drumkit.vst3 --version "$VERSION" "$WORK/vst3.pkg"
pkgbuild --root "$WORK/au" --install-location /Library/Audio/Plug-Ins/Components \
  --identifier app.ours-blanc.drumkit.au --version "$VERSION" "$WORK/au.pkg"

cat > "$WORK/distribution.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
  <title>ours-blanc Drum Kit $VERSION</title>
  <options customize="allow" require-scripts="false" hostArchitectures="arm64,x86_64"/>
  <domains enable_localSystem="true"/>
  <choices-outline>
    <line choice="app"/><line choice="vst3"/><line choice="au"/>
  </choices-outline>
  <choice id="app" title="ours-blanc Drum Kit（スタンドアローン）"><pkg-ref id="app.ours-blanc.drumkit.app"/></choice>
  <choice id="vst3" title="VST3 プラグイン"><pkg-ref id="app.ours-blanc.drumkit.vst3"/></choice>
  <choice id="au" title="Audio Unit プラグイン"><pkg-ref id="app.ours-blanc.drumkit.au"/></choice>
  <pkg-ref id="app.ours-blanc.drumkit.app">app.pkg</pkg-ref>
  <pkg-ref id="app.ours-blanc.drumkit.vst3">vst3.pkg</pkg-ref>
  <pkg-ref id="app.ours-blanc.drumkit.au">au.pkg</pkg-ref>
</installer-gui-script>
EOF

PKG="$OUT/ours-blanc-DrumKit-$VERSION-mac.pkg"
if [ -n "${MAC_INSTALLER_IDENTITY:-}" ]; then
  productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" --sign "$MAC_INSTALLER_IDENTITY" "$PKG"
else
  productbuild --distribution "$WORK/distribution.xml" --package-path "$WORK" "$PKG"
fi

if [ -n "${NOTARY_APPLE_ID:-}" ] && [ -n "${MAC_INSTALLER_IDENTITY:-}" ]; then
  xcrun notarytool submit "$PKG" --apple-id "$NOTARY_APPLE_ID" --team-id "$NOTARY_TEAM_ID" \
    --password "$NOTARY_PASSWORD" --wait
  xcrun stapler staple "$PKG"
fi
echo "$PKG"
