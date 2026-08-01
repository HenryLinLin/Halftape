# Halftape

A practice looper for audio playing on your Mac.
Requires Apple Silicon, macOS 14.4 or newer, CMake, and Xcode Command Line Tools.

Dependencies: `scripts/bootstrap.sh`.
Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`.
Open the app: `scripts/build-app.sh && open build/Halftape.app`.
Licenses: [Third-party notices](THIRD_PARTY.md).
