# PS3DMVP reference implementation

This repository contains the HM and VVenC/VVdeC implementations used to evaluate phase-shift-based 3D motion-vector prediction for complex hologram video. Test sequences, encoded streams, reconstruction images, experiment results, and prebuilt binaries are intentionally omitted.

## Repository layout

```text
.
├── README.md
├── config
│   ├── hm_RA.cfg
│   ├── hm_profiles.json
│   ├── dynamic.json
│   ├── static.json
│   ├── vvenc_RA.cfg
│   ├── vvenc_profiles.json
│   └── suite.json
└── source
    ├── build.ps1
    ├── cpp
    ├── HM-PS3DMVP-Direct
    └── VVenC-PS3DMVP
        └── vvdec
```

The repository contains two codec projects. `HM-PS3DMVP-Direct` provides the modified HM encoder and decoder. `VVenC-PS3DMVP` provides the modified VVenC encoder and contains the matching VVdeC decoder in `vvdec/`. Both baseline and proposed-method experiments use these PS-enabled builds: `PS3DMVP=0` selects the baseline and `PS3DMVP=1` enables PS3DMVP.

## Method

Each complex hologram channel is packed as planar 8-bit 4:4:4 video:

- plane 0: real component;
- plane 1: imaginary component;
- plane 2: zero.

The real and imaginary components share one amplitude value and use the symmetric mapping

```text
mapped = round((value / amplitude + 1) * 127.5), clipped to [0, 255].
```

The baseline uses the native codec prediction modes. When PS3DMVP is enabled, native prediction and direct complex-domain PS prediction compete in rate-distortion optimization. PS motion estimation uses integer-pixel full search, complex SSE over the real and imaginary components, VVC-style spatial/temporal motion-vector predictors, and a configurable phase search. `PS3DMVPThetaDivisions=1` evaluates only zero phase; `PS3DMVPThetaDivisions=16` evaluates phases separated by `2π/16`.

The anchor codec stream and the PS side stream are separate files. The side stream stores the selected PS leaf-CU decisions, reference indices, MVP indices, MVD values, and phase indices with fixed fields and signed/unsigned Exp-Golomb codes. It is experimental syntax and requires the matching modified decoder.

## Supported experiment constraints

The supplied configuration uses the following conditions:

- random-access GOP 16 and 16 input frames;
- 1920 × 1080, 30 fps, 8-bit planar 4:4:4;
- QP 8, 12, 16, and 20;
- maximum CU size 16 and minimum CU size 8;
- native integer motion search range 32;
- PS integer full-search range 32 and four residual-RDO candidates;
- deblocking, SAO, ALF, temporal filtering, weighted prediction, and adaptive QP disabled;
- transform skip enabled;
- one tile, one slice, WPP disabled;
- VVC dual tree disabled.

The baseline and PS configurations use the same native codec settings. PS enable and phase divisions are the only method-level changes.

## Build

The code requires CMake 3.21 or later, a C++17 compiler, and PowerShell for the provided Windows build helper. The current snapshot was developed on Windows with MSVC.

From the repository root:

```powershell
& .\source\build.ps1 -Jobs 8
```

To select a CMake generator explicitly:

```powershell
& .\source\build.ps1 -Generator 'Visual Studio 17 2022' -Jobs 8
```

The script produces:

```text
build/bin/hm/TAppEncoder.exe
build/bin/hm/TAppDecoder.exe
build/bin/vvenc/vvencFFapp.exe
build/bin/vvdec/vvdecapp.exe
build/tools/Release/ra_suite.exe
```

VVenC is built with x86 SIMD enabled. VVdeC SIMD is disabled in the supplied build command because that is the configuration validated with the 4:4:4 PS path.

## Input data

No test sequence is distributed. The experiment runner expects the following structure:

```text
data/holo/dynamic/dataset.json
data/holo/dynamic/<sequence>/frame_000000.holo
...
data/holo/dynamic/<sequence>/frame_000015.holo
data/holo/static/dataset.json
data/holo/static/<sequence>/frame_000000.holo
...
data/holo/static/<sequence>/frame_000015.holo
```

The `.holo` format is implemented in `source/cpp/include/holo.hpp` and `source/cpp/src/io.cpp`. It stores unmapped complex samples; forward mapping is performed by the experiment runner. `config/dynamic.json` and `config/static.json` hold the reconstruction settings and public data paths for the two datasets. Update these files together with `config/suite.json` when using different data.

## Run the experiment suite

Check the configuration and all required inputs without starting an encoder:

```powershell
& .\build\tools\Release\ra_suite.exe --root . --config config/suite.json --check
```

Run codec validation followed by all configured experiments:

```powershell
& .\build\tools\Release\ra_suite.exe --root . --config config/suite.json --all
```

Run one group after its codec validation has passed:

```powershell
& .\build\tools\Release\ra_suite.exe --root . --config config/suite.json --run --group hm_dynamic
```

Completed checkpoints are reused when the same command is run again. Results are written below the `output_root` configured in `config/suite.json`.

## Direct codec options

The HM encoder and the VVenC encoder accept the following PS options:

| Option | Meaning |
|---|---|
| `--PS3DMVP=0|1` | Disable or enable PS3DMVP |
| `--PSBitstream=<path>` | PS side-stream output or input |
| `--PSStats=<path>` | Optional encoder statistics CSV |
| `--PS3DMVPThetaDivisions=<N>` | Number of equally spaced phase candidates |
| `--PS3DMVPSearchRange=<N>` | Integer-pixel search radius |
| `--PS3DMVPCandidates=<N>` | Number of candidates sent to residual RDO |
| `--PS3DMVPFullSearch=1` | Enable exhaustive integer search |
| `--PS3DMVPOptimizedSearch=0|1` | Select the reference or optimized equivalent search path |
| `--ModeStats=<path>` | Optional prediction-mode area statistics |

The decoder needs `--PS3DMVP=1` and the same `--PSBitstream` file when PS is enabled. Phase divisions are parsed from the PS side-stream header.

## Metrics

The experiment runner reports total rate as the sum of the three anchor streams, three PS side streams when enabled, and common mapping metadata. Quality is measured from numerical hologram reconstruction against the original complex hologram. Codec-log PSNR is not used as the primary quality metric. The runner exports reconstruction PSNR/SSIM, complex-domain metrics, prediction-mode area ratios, and BD comparisons.

## Upstream software and licensing

The codec directories retain their upstream notices and license files:

- HM 18.0 under `source/HM-PS3DMVP-Direct`;
- VVenC 1.15.0-dev under `source/VVenC-PS3DMVP`;
- VVdeC 3.2.0 under `source/VVenC-PS3DMVP/vvdec`.

Third-party notices remain in their original subdirectories. Review the component licenses before redistribution. This snapshot does not add a separate project-level license.
