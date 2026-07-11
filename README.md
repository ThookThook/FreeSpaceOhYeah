# Free Space? Oh Yeah!

`fsoy` is a C++20 CLI prototype for HARE v1 file-to-video storage. It encodes an input file into H.264 MP4 frames using Channel A luma-only 4×4 blocks and decodes those videos back to the original bytes with SHA-256 verification.

## Requirements

- A C++20 compiler and CMake 3.16 or newer.
- `ffmpeg` available on `PATH`. Both `encode` and `decode` call `ffmpeg` at runtime.
- Optional GPU Suite support:
  - The main CLI does **not** require CUDA to build or run.
  - If CMake finds NVCC/CUDA, the current NVIDIA GPU Suite (`fsoy_nvidia_gpu`) shared library builds with a CUDA luma-frame kernel.
  - If CUDA is not found, the same NVIDIA GPU Suite shared-library target builds as a driver-detecting stub so the CLI still compiles.
  - Runtime NVIDIA GPU Suite frame synthesis requires the plugin shared library and a CUDA-capable NVIDIA driver/device.
  - Runtime NVIDIA GPU Suite video encoding requires an FFmpeg build with NVENC support and compatible NVIDIA hardware/driver.

## Build commands

| Command | Purpose | Requirements | Notes / disabling conditions |
| --- | --- | --- | --- |
| `cmake -S . -B build` | Configures the project and generates build files in `build/`. | CMake 3.16+ and a C++20 compiler. | By default, CMake tries to build the optional NVIDIA GPU Suite plugin target. Add `-DFSOY_BUILD_NVIDIA_PLUGIN=OFF` to disable that target completely. |
| `cmake -S . -B build -DFSOY_BUILD_NVIDIA_PLUGIN=OFF` | Configures the project without the optional NVIDIA GPU Suite add-on library. | CMake 3.16+ and a C++20 compiler. | Use this when you do not want any GPU plugin library built. The CLI remains usable with CPU frame synthesis and CPU FFmpeg encoding. |
| `cmake --build build` | Compiles the `fsoy` CLI and, unless disabled, the optional NVIDIA GPU Suite plugin shared library. | A successfully configured `build/` directory. | If CUDA is unavailable but the plugin target is enabled, the plugin is built from the CPU stub source instead of the CUDA source. |
| `cmake --install build` | Installs the compiled CLI and optional plugin to the configured install prefix. | A successfully built `build/` directory. | Optional. The CLI can also be run directly from `./build/fsoy`. |

## CLI command overview

The executable has two active modes:

```bash
./build/fsoy encode <input-file> -o <output.mp4> [options]
./build/fsoy decode <input.mp4> [more-inputs.mp4 ...] -o <output-file> [options]
```

`encode` creates an MP4 video containing the source file and a header with the original size and SHA-256. `decode` reads one or more encoded MP4 files, reconstructs the original bytes, writes the requested output file, and verifies the SHA-256 before reporting success.

## Encode commands and options

### Basic CPU encode

```bash
./build/fsoy encode archive.7z -o archive.mp4
```

- **What it does:** Encodes `archive.7z` into `archive.mp4` using CPU luma-frame synthesis and FFmpeg `libx264` video encoding.
- **What it is for:** The default and most portable way to create an FSOY HARE v1 MP4.
- **Requirements:** `archive.7z` must exist, the output path must be writable, and `ffmpeg` must be available on `PATH`.
- **Output side effect:** Writes `archive.mp4.encode.checkpoint` next to the MP4. The checkpoint records the source path, source size, SHA-256, HARE parameters, output list, and completion status.
- **Disable conditions:** This command is not available if `ffmpeg` is missing. It also fails if the input cannot be opened or the output path cannot be written.

### Encode with the NVIDIA GPU Suite process mode

```bash
./build/fsoy encode archive.7z -o archive.mp4 --gpu process
# Alias with the same behavior:
./build/fsoy encode archive.7z -o archive.mp4 --gpu do-all
# Existing compatibility spelling:
./build/fsoy encode archive.7z -o archive.mp4 --gpu auto
```

- **What it does:** Attempts to use the NVIDIA GPU Suite plugin for luma-frame synthesis and attempts NVENC for H.264 encoding. If either GPU path is unavailable, the CLI falls back to CPU behavior where possible.
- **What it is for:** The “do as much GPU processing as this prototype currently supports” mode. `process`, `do-all`, and `auto` currently mean the same thing.
- **Requirements:** Same as basic encode. NVIDIA GPU Suite acceleration additionally requires the plugin shared library to be loadable and a compatible NVIDIA driver/device. NVENC additionally requires FFmpeg NVENC support.
- **Fallback behavior:** If the luma-frame plugin is missing or unavailable, CPU frame synthesis is used. If NVENC encoding fails, the encode is retried with CPU `libx264`.
- **Disable conditions:** NVIDIA GPU Suite acceleration is effectively disabled when the plugin cannot be loaded, the plugin reports no available CUDA device/driver, FFmpeg lacks NVENC support, or compatible NVIDIA hardware/driver support is absent. The overall encode still works if CPU fallback requirements are met.

### Encode with NVIDIA GPU Suite frame synthesis only

```bash
./build/fsoy encode archive.7z -o archive.mp4 --gpu frames --gpu-plugin ./build/libfsoy_nvidia_gpu.so
```

- **What it does:** Attempts to load the named NVIDIA GPU Suite plugin and use it for luma-frame synthesis. Video encoding remains CPU `libx264`.
- **What it is for:** Testing or using GPU frame generation without relying on NVENC video encoding.
- **Requirements:** Same as basic encode. NVIDIA GPU Suite frame synthesis additionally requires a loadable plugin path and compatible NVIDIA runtime support.
- **Plugin path:** `--gpu-plugin` is optional. When omitted, the loader uses its built-in default lookup behavior. When supplied, it should point to the built shared library, such as `./build/libfsoy_nvidia_gpu.so` on Linux, `./build/fsoy_nvidia_gpu.dll` on Windows, or `./build/libfsoy_nvidia_gpu.dylib` on macOS.
- **Fallback behavior:** If the plugin cannot be loaded or reports unavailable, CPU frame synthesis is used.
- **Disable conditions:** GPU frame synthesis is disabled when `--gpu off` is used, when the plugin target was not built, when `--gpu-plugin` points to a missing/incompatible library, or when no compatible CUDA driver/device is available.

### Encode with NVIDIA GPU Suite video encoding only

```bash
./build/fsoy encode archive.7z -o archive.mp4 --gpu encode
```

- **What it does:** Uses CPU luma-frame synthesis, then asks FFmpeg to encode with `h264_nvenc`. If NVENC fails, the CLI retries with CPU `libx264`.
- **What it is for:** Using NVIDIA hardware video encoding without using the frame-synthesis plugin.
- **Requirements:** Same as basic encode. NVENC acceleration additionally requires FFmpeg compiled with `h264_nvenc` support and compatible NVIDIA hardware/driver support.
- **Disable conditions:** NVENC is disabled or falls back when FFmpeg lacks `h264_nvenc`, the NVIDIA driver/device is unavailable, or FFmpeg rejects the encoder settings.

### Encode size/checkpoint scaffolding options

```bash
./build/fsoy encode archive.7z -o archive.mp4 --max-output-size 4G
```

- **What it does:** Parses and records the requested maximum output size intent, then prints a notice that v1 currently writes one feed volume.
- **What it is for:** Future multi-volume/checkpoint workflow compatibility.
- **Requirements:** The value must be an integer with an optional `K`, `M`, or `G` suffix. Examples: `512M`, `4G`, `1024K`, `1000`.
- **Current limitation:** True multi-volume splitting is not implemented yet, so this option does not currently split output files.

### Encode checkpoint resume

```bash
./build/fsoy encode --resume archive.mp4.encode.checkpoint
```

- **What it does:** Reads an encode checkpoint, restores the recorded source and output paths, and re-runs the encode job.
- **What it is for:** Recovering from an interrupted or failed encode without retyping the original paths. The current prototype restarts the encode from the beginning, but uses the checkpoint as the job record.
- **Requirements:** The checkpoint must be a `.encode.checkpoint` file with `type: "encode"`, `source_path`, and `output_path` fields. The original source file must still exist.
- **Output side effect:** Updates the same `.encode.checkpoint` to `started`, `failed`, or `complete` as the resumed job runs.

## Decode commands and options

### Basic decode

```bash
./build/fsoy decode archive.mp4 -o restored.7z
```

- **What it does:** Decodes `archive.mp4`, reconstructs the original payload, writes `restored.7z`, and verifies its SHA-256 against the embedded header.
- **What it is for:** Restoring a file from an FSOY HARE v1 MP4.
- **Requirements:** The input MP4 must be an FSOY HARE v1 stream, the output path must be writable, and `ffmpeg` must be available on `PATH`.
- **Success condition:** The command succeeds only when the reconstructed output SHA-256 matches the embedded SHA-256.
- **Disable conditions:** Decode is not available if `ffmpeg` is missing. It fails if the stream magic is invalid, the stream is truncated, or SHA-256 verification fails.

### Decode multiple input videos

```bash
./build/fsoy decode part1.mp4 part2.mp4 -o restored.7z
```

- **What it does:** Reads decoded bytes from each listed input video in the order provided, then reconstructs and verifies one output file.
- **What it is for:** Compatibility with the planned multi-volume workflow.
- **Requirements:** Every listed input must be readable by FFmpeg, and together they must contain a complete FSOY HARE v1 stream.
- **Current limitation:** The encoder currently writes one feed volume, so multiple-input decode is mainly scaffolding for future split outputs.

### Decode with the NVIDIA GPU Suite process mode

```bash
./build/fsoy decode archive.mp4 -o restored.7z --gpu process
# Alias with the same behavior:
./build/fsoy decode archive.mp4 -o restored.7z --gpu do-all
# Existing compatibility spelling:
./build/fsoy decode archive.mp4 -o restored.7z --gpu auto
```

- **What it does:** Attempts FFmpeg CUDA hardware-accelerated video decode, then falls back to CPU FFmpeg decode if CUDA hwaccel is unavailable or fails.
- **What it is for:** The decode-side equivalent of the NVIDIA GPU Suite process mode. Decode still reconstructs HARE bits on CPU after FFmpeg emits the grayscale luma stream.
- **Requirements:** Same as basic decode. GPU decode acceleration additionally requires FFmpeg CUDA hwaccel support and compatible NVIDIA hardware/driver support.
- **Fallback behavior:** If FFmpeg CUDA decode fails, the CLI retries with CPU decode.

### Decode with NVIDIA GPU Suite video decode only

```bash
./build/fsoy decode archive.mp4 -o restored.7z --gpu decode
```

- **What it does:** Requests FFmpeg CUDA hardware-accelerated video decode only.
- **What it is for:** Explicitly testing decode acceleration without implying frame-synthesis behavior.
- **Requirements:** FFmpeg CUDA hwaccel support and compatible NVIDIA hardware/driver support.
- **Fallback behavior:** If GPU decode fails, the CLI retries with CPU decode.

### Decode checkpoint reuse

```bash
./build/fsoy decode archive.mp4 -o restored.7z --checkpoint restored.7z.decode.checkpoint
```

- **What it does:** Reads an existing decode checkpoint and reuses its recorded output path when available, then runs decode and updates the decode checkpoint for the current job.
- **What it is for:** Keeping decode attempts tied to a durable `.decode.checkpoint` job record.
- **Requirements:** The checkpoint must be a `.decode.checkpoint` file with `type: "decode"`. The input video paths still need to be supplied on the command line.
- **Output side effect:** Every decode writes `<output-file>.decode.checkpoint` with inputs, output path, decoded payload size, SHA-256, HARE parameters, and `started`, `failed`, or `complete` status.

## GPU Suite option reference

| Option | Active with | Purpose | Requirements | Fallback / disabled behavior |
| --- | --- | --- | --- | --- |
| `--gpu off` | `encode`, `decode` | Forces CPU frame synthesis/encoding for encode and CPU FFmpeg video decode for decode. This is also the default when `--gpu` is omitted. | `ffmpeg` CPU codecs. | Disables all optional GPU paths. |
| `--gpu process` / `--gpu do-all` / `--gpu auto` | `encode`, `decode` | Encode: tries NVIDIA GPU Suite frame synthesis plus NVENC encoding. Decode: tries FFmpeg CUDA hardware-accelerated video decode. | Encode requires the plugin/CUDA runtime for frame synthesis and FFmpeg NVENC for encode acceleration. Decode requires FFmpeg CUDA hwaccel. Both require compatible NVIDIA hardware/driver support. | Encode falls back to CPU frame synthesis and CPU `libx264`; decode retries CPU FFmpeg decode. |
| `--gpu frames` | `encode` | Tries NVIDIA GPU Suite luma-frame synthesis only. | Loadable plugin and compatible NVIDIA CUDA driver/device. | Falls back to CPU frame synthesis; video encoding remains CPU `libx264`. |
| `--gpu encode` | `encode` | Tries NVIDIA GPU Suite FFmpeg NVENC video encoding only. | FFmpeg with `h264_nvenc` and compatible NVIDIA hardware/driver. | Retries with CPU `libx264` if NVENC fails. |
| `--gpu decode` | `decode` | Tries NVIDIA GPU Suite FFmpeg CUDA hardware-accelerated video decode only. | FFmpeg CUDA hwaccel and compatible NVIDIA hardware/driver. | Retries with CPU FFmpeg decode if GPU decode fails. |
| `--gpu-plugin <path>` | `encode` with `--gpu process`, `--gpu do-all`, `--gpu auto`, or `--gpu frames` | Selects the GPU plugin shared library to load. | Path to a compatible `fsoy_nvidia_gpu` shared library. | Ignored unless GPU frame synthesis is requested. If missing or invalid, CPU frame synthesis is used. |

## HARE v1 choices in this prototype

- Channel A only: one bit per 4×4 luma block, thresholded as black/white.
- 1920×1080, 24 fps, H.264 Main Profile MP4 through FFmpeg.
- Streaming feed to FFmpeg; the full set of video frames is not materialized as image files.
- File-level SHA-256 verification on decode.
- GPU acceleration is organized as optional GPU Suites. The current work-in-progress suite is the NVIDIA GPU Suite; future suites could use the same idea for other vendors, such as an AMD GPU Suite. `--gpu process` / `--gpu do-all` / `--gpu auto` / `--gpu frames` loads the NVIDIA GPU Suite add-on for encode luma frame synthesis when a CUDA driver is present; `--gpu encode` attempts FFmpeg NVENC and falls back to CPU `libx264`; `--gpu decode` attempts FFmpeg CUDA hardware-accelerated decode and falls back to CPU decode.

## Current limitations

The repository contains the runnable scaffold for v1, but full rateless/Tornado ECC recovery, true byte-offset resume and true multi-volume splitting are still implementation TODOs. When CMake finds NVCC/CUDA, the NVIDIA GPU Suite add-on builds a CUDA kernel for luma frame synthesis; otherwise it builds a driver-detecting stub so the main CLI still compiles without a GPU toolchain.
