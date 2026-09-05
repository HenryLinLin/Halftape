#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .cache/models
fetch_model() {
  local url="$1" file="$2" digest="$3"
  curl -fL "$url" -o "$file.partial"
  python3 - "$file.partial" "$digest" <<'PY'
import hashlib, sys
with open(sys.argv[1], 'rb') as f:
    if hashlib.sha256(f.read()).hexdigest() != sys.argv[2]:
        raise SystemExit('Model checksum mismatch')
PY
  mv "$file.partial" "$file"
}
printf '%s\n' 'Downloading local research weights; see THIRD_PARTY.md for licensing. They are excluded from app bundles and Git.'
fetch_model https://huggingface.co/StemSplitio/htdemucs-6s-onnx/resolve/main/htdemucs_6s_fp16weights.onnx .cache/models/htdemucs_6s.onnx 7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869
fetch_model https://huggingface.co/StemSplitio/htdemucs-onnx/resolve/main/htdemucs_fp16weights.onnx .cache/models/htdemucs.onnx d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a
