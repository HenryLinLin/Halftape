#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

app="${1:-build/Halftape.app}"
if [[ ! -d "$app" ]]; then
  echo "App bundle not found: $app (build it with scripts/build-app.sh first)." >&2
  exit 1
fi
if ! command -v hdiutil >/dev/null 2>&1 || ! command -v codesign >/dev/null 2>&1; then
  echo "Packaging requires macOS tools hdiutil and codesign." >&2
  exit 1
fi

mkdir -p dist
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
cp -R "$app" "$stage/Halftape.app"
ln -s /Applications "$stage/Applications"

identity="${HALFTAPE_SIGN_IDENTITY:-}"
profile="${HALFTAPE_NOTARY_PROFILE:-}"
if [[ -n "$profile" && -z "$identity" ]]; then
  echo "HALFTAPE_NOTARY_PROFILE requires HALFTAPE_SIGN_IDENTITY for notarization." >&2
  exit 1
fi
if [[ -n "$identity" ]]; then
  if [[ ! -f packaging/entitlements.plist ]]; then
    echo "Missing signing entitlements: packaging/entitlements.plist" >&2
    exit 1
  fi
  while IFS= read -r -d '' library; do
    codesign --force --options runtime --timestamp --sign "$identity" "$library"
  done < <(find "$stage/Halftape.app/Contents" -type f -name '*.dylib' -print0)
  codesign --force --options runtime --timestamp --entitlements packaging/entitlements.plist \
    --sign "$identity" "$stage/Halftape.app"
else
  while IFS= read -r -d '' library; do
    codesign --force --sign - "$library"
  done < <(find "$stage/Halftape.app/Contents" -type f -name '*.dylib' -print0)
  codesign --force --sign - "$stage/Halftape.app"
fi
codesign --verify --deep --strict "$stage/Halftape.app"

notarize() {
  local item="$1"
  if ! command -v xcrun >/dev/null 2>&1; then
    echo "Notarization requires xcrun; install Xcode Command Line Tools or Xcode." >&2
    exit 1
  fi
  xcrun notarytool submit "$item" --keychain-profile "$profile" --wait
}
if [[ -n "$profile" ]]; then
  if ! command -v ditto >/dev/null 2>&1; then
    echo "Notarizing requires the macOS ditto tool." >&2
    exit 1
  fi
  ditto -c -k --keepParent "$stage/Halftape.app" "$stage/Halftape-notary.zip"
  notarize "$stage/Halftape-notary.zip"
  rm "$stage/Halftape-notary.zip"
  xcrun stapler staple "$stage/Halftape.app"
fi

dmg="dist/Halftape-0.1.0.dmg"
hdiutil create -volname Halftape -srcfolder "$stage" -ov -format UDZO "$dmg"
if [[ -n "$profile" ]]; then
  notarize "$dmg"
  xcrun stapler staple "$dmg"
  echo "Created and notarized $dmg"
else
  echo "Created locally signed (not notarized) $dmg"
fi
