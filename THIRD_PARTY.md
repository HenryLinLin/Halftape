# Third-party notices

Halftape uses the following third-party components. Their notices and license texts remain with the corresponding sources or app bundle.

| Component | License | Notice and source |
| --- | --- | --- |
| Signalsmith Stretch | MIT | Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd. [Source](https://github.com/Signalsmith-Audio/signalsmith-stretch) |
| Signalsmith Linear | MIT | Copyright (c) 2025 Signalsmith Audio. [Source](https://github.com/Signalsmith-Audio/linear) |
| r8brain-free-src | MIT | Copyright (c) 2013–2026 Aleksey Vaneev. Documentation credit: “Sample rate converter designed by Aleksey Vaneev of Voxengo”. [Source](https://github.com/avaneev/r8brain-free-src) |
| demucs.cpp | MIT | Copyright (c) 2023 Sevag H. [Source](https://github.com/sevagh/demucs.cpp) |
| Eigen | MPL 2.0 | Vendored by demucs.cpp. [License](https://eigen.tuxfamily.org/index.php?title=Licensing_FAQ) |
| ONNX Runtime 1.30.0 | MIT | The app bundle includes the macOS arm64 runtime, license, and third-party notices. [License](https://github.com/microsoft/onnxruntime/blob/v1.30.0/LICENSE), [notices](https://github.com/microsoft/onnxruntime/blob/v1.30.0/ThirdPartyNotices.txt) |
| StemGenRT streaming parity fixture | MIT | Copyright (c) 2026 Axel Delafosse. The synthetic fixture and its license are in `tests/fixtures/`. [Pinned upstream fixture source](https://github.com/sweetspotsoundsystem/stemgen-rt/tree/61df8f4aa1555ef110308d01ea92b54ace770979/test/fixtures) |

The MIT license texts for vendored components are in `.deps/`. Eigen's MPL-2.0 text is in `.deps/demucs/vendor/eigen/COPYING.MPL2`.

## Model files

Model files are downloaded or selected separately and are not included in the app bundle.

The `htdemucs` and `htdemucs_6s` files downloaded by `scripts/models.sh` are pretrained Demucs weights, converted to ONNX by [StemSplit](https://huggingface.co/StemSplitio/htdemucs-6s-onnx). The upstream [pretrained-weight discussion](https://github.com/facebookresearch/demucs/issues/327) describes scientific-use terms. The software's MIT license does not establish a separate redistribution grant for the weights.

Low latency Sing downloads `model/model.onnx` from the pinned [StemGenRT revision](https://github.com/sweetspotsoundsystem/stemgen-rt/tree/61df8f4aa1555ef110308d01ea92b54ace770979) and verifies its SHA-256. The upstream repository includes an MIT license for its software; that license does not separately identify terms for this trained model artifact.
