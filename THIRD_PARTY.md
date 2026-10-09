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

## Models and music

Halftape is a free personal practice project, with no plans for monetization. Model files are downloaded or selected separately; neither the original weights nor locally converted models are included in the app bundle or releases. Their licenses are separate from Halftape's code license.

**Clean Sing** uses UMX-L by Fabian-Robert Stöter and Antoine Liutkus ([original model](https://zenodo.org/records/5069601)), licensed [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/). Halftape uses it only non-commercially. The script converts it to ONNX locally without retraining, and Halftape does not distribute the converted model.

**Practice** uses [Demucs weights converted by StemSplit](https://huggingface.co/StemSplitio/htdemucs-6s-onnx). The original Demucs weights were released for research use; see the [upstream discussion](https://github.com/facebookresearch/demucs/issues/327).

**Low latency Sing** uses the model published by the [StemGenRT project](https://github.com/sweetspotsoundsystem/stemgen-rt/tree/61df8f4aa1555ef110308d01ea92b54ace770979). The project's code is MIT-licensed; the model has no separately stated license.

[MUSDB excerpts](https://github.com/sigsep/sigsep-mus-db#getting-the-data) were used for local separation tests and are not distributed. Halftape includes no songs, keeps practice clips only for the current session, and has no audio export.

If a rights holder raises a concern about a model or material used by Halftape, I will remove the affected integration or material while the concern is reviewed.
