# Halftape

Halftape captures music from other Mac apps so you can practice a passage on repeat. You can slow it down without changing the pitch, or adjust the key to suit your instrument. Stem controls let you hear a part on its own or mute it to play along. For difficult passages, the trainer gradually brings the loop back up to speed.

Live Sing turns down vocals while the music plays, with Clean and Low latency modes. Clips last for the session; nothing is saved or exported.

Requires Apple Silicon, macOS 14.4 or newer, CMake, and Xcode Command Line Tools. Build and open:
```sh
scripts/bootstrap.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
scripts/build-app.sh
open build/Halftape.app
```
For controls and model setup, see [Using Halftape](docs/usage.md). Third-party notices are in [THIRD_PARTY.md](THIRD_PARTY.md).
