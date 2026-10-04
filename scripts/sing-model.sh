#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."

url='https://media.githubusercontent.com/media/sweetspotsoundsystem/stemgen-rt/61df8f4aa1555ef110308d01ea92b54ace770979/model/model.onnx'
file='.cache/models/stemgenrt-hop128.onnx'
sha='77164d6a581fafb2a31f53fd8ffde44c07cf618472952a4cdba14e68dda3b8b9'
mkdir -p "$(dirname "$file")"

verify() {
  printf '%s  %s\n' "$sha" "$1" | shasum -a 256 -c - >/dev/null
}

if [[ -f "$file" ]] && verify "$file"; then
  printf 'Sing model already present and verified: %s\n' "$file"
  exit 0
fi

partial="$file.partial"
trap 'rm -f "$partial"' EXIT
curl -fL --retry 3 "$url" -o "$partial"
verify "$partial" || { echo 'Sing model checksum mismatch' >&2; exit 1; }
mv "$partial" "$file"
trap - EXIT
printf 'Installed verified Sing model: %s\n' "$file"
