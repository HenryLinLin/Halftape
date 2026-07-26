#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .deps
fetch() {
  local name="$1" repo="$2" ref="$3"
  if [ ! -d ".deps/$name/.git" ]; then
    git init -q ".deps/$name"
    git -C ".deps/$name" remote add origin "$repo"
  fi
  if [ "$(git -C ".deps/$name" rev-parse HEAD 2>/dev/null || true)" != "$ref" ]; then
    git -C ".deps/$name" fetch --depth 1 origin "$ref"
    git -C ".deps/$name" checkout -q --detach FETCH_HEAD
  fi
}
fetch stretch https://github.com/Signalsmith-Audio/signalsmith-stretch.git 57b93f4e9206a089a45387eaa39bdc9f310d3308
fetch linear https://github.com/Signalsmith-Audio/linear.git 0.3.1
fetch r8brain https://github.com/avaneev/r8brain-free-src.git e71c31bf320f84210bb4bdcb57e296c39ce940f9
fetch demucs https://github.com/sevagh/demucs.cpp.git f1206e9adeea103aef4a636b9e62297cf1f8e34e
if [ ! -f .deps/demucs/vendor/eigen/Eigen/Core ]; then
  git -C .deps/demucs submodule update --init --depth 1 vendor/eigen
fi
ort_dir=.deps/onnxruntime-osx-arm64-1.28.0
if [ ! -f "$ort_dir/lib/libonnxruntime.dylib" ]; then
  archive=$(mktemp -t halftape-ort)
  trap 'rm -f "$archive"' EXIT
  curl -fL https://github.com/microsoft/onnxruntime/releases/download/v1.28.0/onnxruntime-osx-arm64-1.28.0.tgz -o "$archive"
  python3 - "$archive" <<'PY'
import hashlib, sys
with open(sys.argv[1], 'rb') as f:
    if hashlib.sha256(f.read()).hexdigest() != '1268b359718099bde2cedb55787f182a130067bc4f31e8c88478c445b850d3d8':
        raise SystemExit('ONNX Runtime archive checksum mismatch')
PY
  LC_ALL=C tar -xzf "$archive" -C .deps
fi
