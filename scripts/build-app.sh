#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build_dir="${1:-build}"
cmake --build "$build_dir" --target wsbridge -j3
app="$build_dir/Halftape.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources/licenses" "$app/Contents/Frameworks"
ort_dir=.deps/onnxruntime-osx-arm64-1.28.0
cp "$ort_dir/lib/libonnxruntime.1.28.0.dylib" "$app/Contents/Frameworks/"
ln -sf libonnxruntime.1.28.0.dylib "$app/Contents/Frameworks/libonnxruntime.1.dylib"
cp HalftapeApp/Info.plist "$app/Contents/Info.plist"
xcrun swiftc -O -parse-as-library -target arm64-apple-macos14.4 \
  -module-cache-path "$build_dir/swift-cache" \
  -import-objc-header wsbridge/include/wsbridge.h HalftapeApp/*.swift \
  "$build_dir/libwsbridge.a" "$build_dir/libmactap.a" "$build_dir/libwscore.a" "$build_dir/libdemucs.a" \
  -L "$ort_dir/lib" -lonnxruntime -Xlinker -rpath -Xlinker @executable_path/../Frameworks \
  -lc++ -framework AppKit -framework SwiftUI -framework Carbon -framework CoreAudio \
  -framework AudioUnit -framework Accelerate -framework ApplicationServices -framework IOKit \
  -o "$app/Contents/MacOS/Halftape"
cp LICENSE THIRD_PARTY.md "$app/Contents/Resources/"
cp .deps/stretch/LICENSE.txt "$app/Contents/Resources/licenses/Signalsmith-Stretch.txt"
cp .deps/linear/LICENSE.txt "$app/Contents/Resources/licenses/Signalsmith-Linear.txt"
cp .deps/r8brain/LICENSE "$app/Contents/Resources/licenses/r8brain.txt"
cp .deps/demucs/LICENSE "$app/Contents/Resources/licenses/demucs.cpp.txt"
cp .deps/demucs/vendor/eigen/COPYING.MPL2 "$app/Contents/Resources/licenses/Eigen-MPL2.txt"
cp .deps/demucs/vendor/eigen/COPYING.BSD "$app/Contents/Resources/licenses/Eigen-BSD.txt"
cp "$ort_dir/LICENSE" "$app/Contents/Resources/licenses/ONNX-Runtime.txt"
cp "$ort_dir/ThirdPartyNotices.txt" "$app/Contents/Resources/licenses/ONNX-ThirdPartyNotices.txt"
codesign --force --sign - "$app/Contents/Frameworks/libonnxruntime.1.28.0.dylib"
codesign --force --deep --sign - "$app"
echo "$app"
