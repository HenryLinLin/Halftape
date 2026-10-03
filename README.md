# Halftape

A practice looper for audio playing on your Mac. Requires Apple Silicon, macOS 14.4 or newer, CMake, and Xcode Command Line Tools.

Build and open the app:

```sh
scripts/bootstrap.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
scripts/build-app.sh
open build/Halftape.app
```

For controls and model setup, see [Using Halftape](docs/usage.md). Third-party notices are in [THIRD_PARTY.md](THIRD_PARTY.md).
