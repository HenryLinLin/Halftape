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
