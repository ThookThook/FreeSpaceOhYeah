# Free Space? Oh Yeah! (F.S.O.Y.)

`fsoy` is a C++20 CLI prototype for HARE v1 file-to-video storage. It encodes an input file into H.264 MP4 frames using Channel A luma-only 4x4 blocks, and decodes those videos back to the original bytes with SHA-256 verification.

## Requirements

- A C++20 compiler and CMake 3.16 or newer
- `ffmpeg` available on `PATH` (both `encode` and `decode` call `ffmpeg` at runtime)
- Optional GPU Suite support:
  - The main CLI does **not** require CUDA to build or run
  - If CMake finds NVCC/CUDA, the NVIDIA GPU Suite (`fsoy_nvidia_gpu`) shared library builds with a CUDA luma-frame kernel
  - If CUDA is not found, the same target builds as a driver-detecting stub so the CLI still compiles
  - Runtime GPU frame synthesis requires the plugin shared library and a CUDA-capable NVIDIA driver/device
  - Runtime GPU video encoding requires an FFmpeg build with NVENC support and compatible NVIDIA hardware/driver

## Build

| Command | Purpose | Requirements | Notes |
|---|---|---|---|
| `cmake -S . -B build` | Configure the project | CMake 3.16+, C++20 compiler | Tries to build the optional NVIDIA GPU Suite plugin by default |
| `cmake -S . -B build -DFSOY_BUILD_NVIDIA_PLUGIN=OFF` | Configure without the GPU plugin | Same as above | Use when you don't want the GPU plugin built at all; CLI still works via CPU |
| `cmake --build build` | Compile `fsoy` (and the GPU plugin, unless disabled) | Configured `build/` directory | If CUDA is unavailable but the plugin target is enabled, it's built from the CPU stub source instead |
| `cmake --install build` | Install the CLI and optional plugin | Built `build/` directory | Optional — you can also just run `./build/fsoy` directly |

## CLI overview

```
./build/fsoy encode <input-file> -o <output.mp4> [options]
./build/fsoy decode <input.mp4> [more-inputs.mp4 ...] -o <output-file> [options]
```

`encode` creates an MP4 containing the source file, a HARE header (original size + SHA-256), checksummed data chunks, and ~10% XOR repair chunks.

`decode` reads one or more encoded MP4 files, reconstructs the chunk stream, uses repair chunks when a group has one missing/corrupt data chunk, writes the output file, and verifies the SHA-256 before reporting success.

---

## Encode

### Basic CPU encode

```
./build/fsoy encode archive.7z -o archive.mp4
```

Encodes `archive.7z` into `archive.mp4` using CPU luma-frame synthesis and FFmpeg libx264 video encoding. This is the default, most portable way to create an FSOY HARE v1 MP4.

- **Requirements:** `archive.7z` must exist, the output path must be writable, `ffmpeg` must be on `PATH`.
- **Output side effect:** writes `archive.mp4.encode.checkpoint` alongside the MP4, recording source path, source size, SHA-256, HARE parameters, ECC settings, output list, and completion status.
- **Fails if:** `ffmpeg` is missing, the input can't be opened, or the output path can't be written.

### GPU Suite: process mode

```
./build/fsoy encode archive.7z -o archive.mp4 --gpu process
# aliases with identical behavior:
./build/fsoy encode archive.7z -o archive.mp4 --gpu do-all
./build/fsoy encode archive.7z -o archive.mp4 --gpu auto
```

Attempts NVIDIA GPU Suite luma-frame synthesis plus NVENC H.264 encoding. Falls back to CPU wherever a GPU path is unavailable. This is the "do as much GPU processing as this prototype supports" mode — `process`, `do-all`, and `auto` all mean the same thing.

- **Requirements:** same as basic encode, plus a loadable plugin + compatible CUDA driver/device for frame synthesis, and FFmpeg NVENC support for encoding.
- **Fallback:** missing/unavailable plugin -> CPU frame synthesis. Failed NVENC -> retries with CPU libx264.
- **Effectively disabled when:** the plugin can't load, reports no CUDA device, FFmpeg lacks NVENC, or NVIDIA hardware/driver support is absent. Encode still works as long as CPU fallback requirements are met.

### GPU Suite: frame synthesis only

```
./build/fsoy encode archive.7z -o archive.mp4 --gpu frames --gpu-plugin ./build/libfsoy_nvidia_gpu.so
```

Uses the NVIDIA GPU Suite plugin for luma-frame synthesis only; video encoding stays on CPU libx264. Useful for testing GPU frame generation without relying on NVENC.

- **Requirements:** same as basic encode, plus a loadable plugin path and compatible CUDA runtime.
- **Plugin path:** `--gpu-plugin` is optional — omit it to use the built-in default lookup. When supplied, point it at the built shared library:
  - Linux: `./build/libfsoy_nvidia_gpu.so`
  - Windows: `./build/fsoy_nvidia_gpu.dll`
  - macOS: `./build/libfsoy_nvidia_gpu.dylib`
- **Fallback:** if the plugin can't load or reports unavailable, CPU frame synthesis is used.
- **Disabled when:** `--gpu off` is passed, the plugin target wasn't built, `--gpu-plugin` points to a missing/incompatible library, or no compatible CUDA driver/device is present.

### GPU Suite: video encoding only

```
./build/fsoy encode archive.7z -o archive.mp4 --gpu encode
```

CPU luma-frame synthesis, then FFmpeg encodes with `h264_nvenc`. Retries with CPU libx264 if NVENC fails.

- **Requirements:** same as basic encode, plus FFmpeg compiled with `h264_nvenc` support and compatible NVIDIA hardware/driver.
- **Disabled/falls back when:** FFmpeg lacks `h264_nvenc`, the NVIDIA driver/device is unavailable, or FFmpeg rejects the encoder settings.

### Size / checkpoint scaffolding

```
./build/fsoy encode archive.7z -o archive.mp4 --max-output-size 4G
```

Parses and records a requested max output size, then prints a notice that v1 currently writes a single feed volume. This is scaffolding for a future multi-volume/checkpoint workflow — **true multi-volume splitting isn't implemented yet.**

- **Value format:** integer with optional `K`, `M`, or `G` suffix (e.g. `512M`, `4G`, `1024K`, `1000`).

### Resume from checkpoint

```
./build/fsoy encode --resume archive.mp4.encode.checkpoint
```

Reads an encode checkpoint, restores the recorded source/output paths, and re-runs the job. Useful for recovering from an interrupted or failed encode without retyping paths. (Current prototype restarts the encode from scratch — the checkpoint is just the job record.)

- **Requirements:** checkpoint must be a `.encode.checkpoint` file with `type: "encode"`, `source_path`, and `output_path` fields. The original source file must still exist.
- **Output side effect:** updates the same checkpoint file to `started`, `failed`, or `complete` as the job runs.

---

## Decode

### Basic decode

```
./build/fsoy decode archive.mp4 -o restored.7z
```

Decodes `archive.mp4`, reconstructs the payload, writes `restored.7z`, and verifies its SHA-256 against the embedded header.

- **Requirements:** input must be an FSOY HARE v1 stream, output path must be writable, `ffmpeg` must be on `PATH`.
- **Succeeds only if:** the reconstructed output's SHA-256 matches the embedded SHA-256.
- **Fails if:** `ffmpeg` is missing, stream magic is invalid, the stream is truncated, or SHA-256 verification fails.

### Decode multiple input videos

```
./build/fsoy decode part1.mp4 part2.mp4 -o restored.7z
```

Reads decoded bytes from each input video in the order given, then reconstructs and verifies one output file. This is mainly scaffolding for the planned multi-volume workflow, since encode currently only writes a single feed volume.

- **Requirements:** every input must be readable by FFmpeg, and together they must contain a complete FSOY HARE v1 stream.

### GPU Suite: process mode

```
./build/fsoy decode archive.mp4 -o restored.7z --gpu process
# aliases:
./build/fsoy decode archive.mp4 -o restored.7z --gpu do-all
./build/fsoy decode archive.mp4 -o restored.7z --gpu auto
```

Attempts FFmpeg CUDA hardware-accelerated video decode, falling back to CPU decode if CUDA hwaccel is unavailable or fails. HARE bit reconstruction always happens on CPU after FFmpeg emits the grayscale luma stream.

- **Requirements:** same as basic decode, plus FFmpeg CUDA hwaccel support and compatible NVIDIA hardware/driver.

### GPU Suite: video decode only

```
./build/fsoy decode archive.mp4 -o restored.7z --gpu decode
```

Requests FFmpeg CUDA hardware-accelerated video decode explicitly (no implied frame-synthesis behavior).

- **Requirements:** FFmpeg CUDA hwaccel support and compatible NVIDIA hardware/driver.
- **Fallback:** retries with CPU decode if GPU decode fails.

### Decode checkpoint reuse

```
./build/fsoy decode archive.mp4 -o restored.7z --checkpoint restored.7z.decode.checkpoint
```

Reads an existing decode checkpoint, reuses its recorded output path when available, runs decode, and updates the checkpoint.

- **Requirements:** checkpoint must be a `.decode.checkpoint` file with `type: "decode"`. Input video paths still need to be supplied on the command line.
- **Output side effect:** every decode writes `<output-file>.decode.checkpoint` with inputs, output path, decoded payload size, SHA-256, HARE parameters, and status.

---

## GPU Suite option reference

| Option | Active with | Purpose | Requirements | Fallback / disabled behavior |
|---|---|---|---|---|
| `--gpu off` (default) | encode, decode | Forces CPU frame synthesis/encoding, CPU FFmpeg decode | `ffmpeg` CPU codecs | Disables all optional GPU paths |
| `--gpu process` / `--gpu do-all` / `--gpu auto` | encode, decode | Encode: GPU frame synthesis + NVENC. Decode: FFmpeg CUDA hwaccel decode | Plugin/CUDA runtime + FFmpeg NVENC (encode); FFmpeg CUDA hwaccel (decode); compatible NVIDIA hardware/driver | Encode falls back to CPU frame synthesis + libx264; decode retries CPU FFmpeg decode |
| `--gpu frames` | encode | GPU luma-frame synthesis only | Loadable plugin + compatible CUDA driver/device | Falls back to CPU frame synthesis; encoding stays CPU libx264 |
| `--gpu encode` | encode | NVENC video encoding only | FFmpeg with `h264_nvenc` + compatible NVIDIA hardware/driver | Retries with CPU libx264 if NVENC fails |
| `--gpu decode` | decode | FFmpeg CUDA hardware-accelerated decode only | FFmpeg CUDA hwaccel + compatible NVIDIA hardware/driver | Retries with CPU FFmpeg decode if GPU decode fails |
| `--gpu-plugin <path>` | encode with `process`/`do-all`/`auto`/`frames` | Selects the GPU plugin shared library to load | Path to a compatible `fsoy_nvidia_gpu` shared library | Ignored unless GPU frame synthesis is requested; falls back to CPU frame synthesis if missing/invalid |

---

## HARE v1 design notes

- **Channel A only:** one bit per 4x4 luma block, thresholded as black/white
- **Video format:** 1920x1080, 24 fps, H.264 Main Profile MP4 via FFmpeg
- **Streaming:** frames are streamed directly to FFmpeg — never materialized as individual image files
- **Error correction:** per-chunk checksums, one XOR repair symbol per 10 data chunks (~10% ECC), single-missing/corrupt-chunk recovery per ECC group, file-level SHA-256 verification on decode
- **GPU Suites:** GPU acceleration is organized into vendor-specific "suites." The NVIDIA GPU Suite is the current work-in-progress; future suites (e.g. an AMD GPU Suite) could follow the same pattern.
  - `--gpu process` / `do-all` / `auto` / `frames` -> loads the NVIDIA GPU Suite plugin for luma-frame synthesis when a CUDA driver is present
  - `--gpu encode` -> attempts FFmpeg NVENC, falls back to CPU libx264
  - `--gpu decode` -> attempts FFmpeg CUDA hardware-accelerated decode, falls back to CPU decode
