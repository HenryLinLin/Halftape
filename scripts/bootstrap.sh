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
fetch stretch https://github.com/Signalsmith-Audio/signalsmith-stretch.git a670068d9aeb64913331d5cc29337b19a457a7df
fetch linear https://github.com/Signalsmith-Audio/linear.git 0.6.4
fetch r8brain https://github.com/avaneev/r8brain-free-src.git cb2abb9977efe2471979b380ed95daa56ab4fdb9
fetch demucs https://github.com/sevagh/demucs.cpp.git f1206e9adeea103aef4a636b9e62297cf1f8e34e
if [ ! -f .deps/demucs/vendor/eigen/Eigen/Core ]; then
  git -C .deps/demucs submodule update --init --depth 1 vendor/eigen
fi
ort_dir=.deps/onnxruntime-osx-arm64-1.30.0
if [ ! -f "$ort_dir/lib/libonnxruntime.dylib" ]; then
  archive=$(mktemp -t halftape-ort)
  trap 'rm -f "$archive"' EXIT
  curl -fL https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-osx-arm64-1.30.0.tgz -o "$archive"
  python3 - "$archive" <<'PY'
import hashlib, sys
with open(sys.argv[1], 'rb') as f:
    if hashlib.sha256(f.read()).hexdigest() != '6ebb5062a934537c352937821f9fe9718e7de1a2db1122a93dd363ffd53a7012':
        raise SystemExit('ONNX Runtime archive checksum mismatch')
PY
  LC_ALL=C tar -xzf "$archive" -C .deps
fi
