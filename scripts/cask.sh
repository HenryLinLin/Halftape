#!/bin/bash
set -euo pipefail
export LC_ALL=C
repo_dir="$(cd "$(dirname "$0")/.." && pwd)"

if [[ $# -eq 1 ]]; then
  dmg="$(python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$1")"
  if [[ ! -f "$dmg" ]]; then
    echo "DMG file not found: $dmg" >&2
    exit 1
  fi
  url="file://$(python3 -c 'from urllib.parse import quote; import sys; print(quote(sys.argv[1], safe="/"))' "$dmg")"
elif [[ $# -eq 2 ]]; then
  url="$1"
  dmg="$(python3 -c 'import os, sys; print(os.path.abspath(sys.argv[1]))' "$2")"
  if [[ "$url" != https://* || "$url" == *'"'* || "$url" == *"'"* || "$url" == *'\'* || "$url" =~ [[:space:][:cntrl:]] ]]; then
    echo "Supply the actual HTTPS URL for the published DMG." >&2
    exit 2
  fi
else
  echo "Usage: scripts/cask.sh path/to/Halftape-0.1.0.dmg | ACTUAL_HTTPS_DMG_URL path/to/Halftape-0.1.0.dmg" >&2
  exit 2
fi
if [[ ! -f "$dmg" ]]; then
  echo "DMG file not found: $dmg" >&2
  exit 1
fi

cd "$repo_dir"
sha="$(shasum -a 256 "$dmg" | awk '{print $1}')"
url_ruby="${url//\\/\\\\}"
url_ruby="${url_ruby//\'/\\\'}"
mkdir -p dist
{
  printf '%s\n' 'cask "halftape" do' '  version "0.1.0"'
  printf '  sha256 "%s"\n\n' "$sha"
  printf "  url '%s'\n" "$url_ruby"
  printf '%s\n' '  name "Halftape"' '  desc "A practice looper for the music playing on your Mac"' \
    '  homepage "https://github.com"' '' '  depends_on arch: :arm64' \
    '  depends_on macos: ">= :sonoma"' '' '  app "Halftape.app"' 'end'
} > dist/halftape.rb
echo "Wrote dist/halftape.rb (SHA-256: $sha)"
