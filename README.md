# Free Space? Oh Yeah! (F.S.O.Y.)

`fsoy` is a C++20 CLI prototype for file-to-video storage. It encodes an input file into video frames and decodes those videos back to the original bytes with SHA-256 verification.

**This is FSOY v2.** v2 replaces the v1 "HARE" 4x4-luma-block encoding with a much denser, self-describing scheme: `--tile 1 --palette 2` by default (one payload bit per pixel, black/white), encoded losslessly. See [`v2 design notes`](#fsoy-v2-design-notes) below for why, and [`Migrating from v1`](#migrating-from-v1) if you have old HARE-encoded videos.

## Requirements

- A C++20 compiler and CMake 3.16 or newer
- `ffmpeg` available on `PATH` (both `encode` and `decode` call `ffmpeg` at runtime), built with FFV1 support (standard in most distributions)
- Optional GPU Suite support:
  - The main CLI does **not** require CUDA to build or run
  - If CMake finds NVCC/CUDA, the NVIDIA GPU Suite (`fsoy_nvidia_gpu`) shared library builds with a CUDA luma-frame kernel
  - If CUDA is not found, the same target builds as a driver-detecting stub so the CLI still compiles
  - Runtime GPU frame synthesis requires the plugin shared library and a CUDA-capable NVIDIA driver/device
  - Runtime GPU video encoding requires an FFmpeg build with NVENC support and compatible NVIDIA hardware/driver — note this path is **lossy H.264**, not FFV1 (see below)

## Build

| Command | Purpose | Requirements | Notes |
|---|---|---|---|
| `cmake -S . -B build` | Configure the project | CMake 3.16+, C++20 compiler | Tries to build the optional NVIDIA GPU Suite plugin by default |
| `cmake -S . -B build -DFSOY_BUILD_NVIDIA_PLUGIN=OFF` | Configure without the GPU plugin | Same as above | Use when you don't want the GPU plugin built at all; CLI still works via CPU |
| `cmake --build build` | Compile `fsoy` (and the GPU plugin, unless disabled) | Configured `build/` directory | If CUDA is unavailable but the plugin target is enabled, it's built from the CPU stub source instead |
| `cmake --install build` | Install the CLI and optional plugin | Built `build/` directory | Optional — you can also just run `./build/fsoy` directly |

## CLI overview

```
./build/fsoy encode <input-file> -o <output.mkv> [--tile N] [--palette 2] [options]
./build/fsoy decode <input.mkv> [more-inputs.mkv ...] -o <output-file> [options]
```

`encode` creates a video containing the source file: a self-describing FSOY v2 header frame, checksummed data chunks, and ~10% XOR repair chunks.

`decode` reads one or more encoded video files, reconstructs the chunk stream, uses repair chunks when a group has one missing/corrupt data chunk, writes the output file, and verifies the SHA-256 before reporting success. It reads `tile`/`palette`/geometry from the video itself — no external metadata is required.

---

## Encode

### Basic CPU encode (default: tile=1, palette=2, lossless)

```
./build/fsoy encode archive.7z -o archive.mkv
```

Encodes `archive.7z` into `archive.mkv` using CPU luma-frame synthesis (1 payload bit per pixel, hard black/white threshold) and FFmpeg FFV1 lossless video encoding. This is the default, most portable, most space-efficient way to create an FSOY v2 video.

- **Requirements:** `archive.7z` must exist, the output path must be writable, `ffmpeg` must be on `PATH` with FFV1 support.
- **Output side effect:** writes `archive.mkv.encode.checkpoint` alongside the video, recording source path, source size, SHA-256, tile/palette, ECC settings, output list, and completion status.
- **Fails if:** `ffmpeg` is missing, the input can't be opened, or the output path can't be written.
- **Output container:** `.mkv` is the natural container for FFV1; you can name the output anything, but non-Matroska containers may not support FFV1 muxing depending on your ffmpeg build.

### Tile size

```
./build/fsoy encode archive.7z -o archive.mkv --tile 4
```

Controls how many payload bits map to how large a solid-color square. `--tile 1` (default) is one bit per pixel — maximum density. `--tile 4` reproduces v1 HARE's spatial density (16x less dense than tile=1) and is the safer choice if you intend to run the output through a lossy re-encode (see design notes below) rather than keep it as a lossless local artifact.

- `--tile N` must evenly divide 1920x1080 (e.g. 1, 2, 4, 5, 8...).
- Recorded in the video's own header frame — decode doesn't need to be told the tile size.

### Palette

```
./build/fsoy encode archive.7z -o archive.mkv --palette 2
```

`--palette 2` (the only supported value in this build) is explicit for forward-compatibility with a future N-color palette; today it's also simply the default.

### GPU Suite: process mode

```
./build/fsoy encode archive.7z -o archive.mkv --gpu process
# aliases with identical behavior:
./build/fsoy encode archive.7z -o archive.mkv --gpu do-all
./build/fsoy encode archive.7z -o archive.mkv --gpu auto
```

Attempts NVIDIA GPU Suite luma-frame synthesis plus NVENC H.264 encoding. Falls back to CPU wherever a GPU path is unavailable. **Note:** there is no lossless NVENC equivalent to FFV1, so this GPU path is inherently lossy H.264, unlike the default CPU/FFV1 path. Use `--gpu off` (default) if losslessness matters and you don't specifically need GPU throughput.

- **Requirements:** same as basic encode, plus a loadable plugin + compatible CUDA driver/device for frame synthesis, and FFmpeg NVENC support for encoding.
- **Fallback:** missing/unavailable plugin -> CPU frame synthesis. Failed NVENC -> retries with CPU FFV1.
- **Effectively disabled when:** the plugin can't load, reports no CUDA device, FFmpeg lacks NVENC, or NVIDIA hardware/driver support is absent. Encode still works as long as CPU fallback requirements are met.

### GPU Suite: frame synthesis only

```
./build/fsoy encode archive.7z -o archive.mkv --gpu frames --gpu-plugin ./build/libfsoy_nvidia_gpu.so
```

Uses the NVIDIA GPU Suite plugin for luma-frame synthesis only; video encoding stays on CPU FFV1 (lossless). Useful for testing GPU frame generation without relying on NVENC.

- **Requirements:** same as basic encode, plus a loadable plugin path and compatible CUDA runtime.
- **Plugin path:** `--gpu-plugin` is optional — omit it to use the built-in default lookup. When supplied, point it at the built shared library:
  - Linux: `./build/libfsoy_nvidia_gpu.so`
  - Windows: `./build/fsoy_nvidia_gpu.dll`
  - macOS: `./build/libfsoy_nvidia_gpu.dylib`
- **Fallback:** if the plugin can't load or reports unavailable, CPU frame synthesis is used.
- **Disabled when:** `--gpu off` is passed, the plugin target wasn't built, `--gpu-plugin` points to a missing/incompatible library, or no compatible CUDA driver/device is present.

### GPU Suite: video encoding only

```
./build/fsoy encode archive.7z -o archive.mkv --gpu encode
```

CPU luma-frame synthesis, then FFmpeg encodes with `h264_nvenc` (lossy). Retries with CPU FFV1 (lossless) if NVENC fails.

- **Requirements:** same as basic encode, plus FFmpeg compiled with `h264_nvenc` support and compatible NVIDIA hardware/driver.
- **Disabled/falls back when:** FFmpeg lacks `h264_nvenc`, the NVIDIA driver/device is unavailable, or FFmpeg rejects the encoder settings.

### Size / checkpoint scaffolding

```
./build/fsoy encode archive.7z -o archive.mkv --max-output-size 4G
```

Parses and records a requested max output size, then prints a notice that v2 currently writes a single feed volume. This is scaffolding for a future multi-volume/checkpoint workflow — **true multi-volume splitting isn't implemented yet.**

- **Value format:** integer with optional `K`, `M`, or `G` suffix (e.g. `512M`, `4G`, `1024K`, `1000`).

### Resume from checkpoint

```
./build/fsoy encode --resume archive.mkv.encode.checkpoint
```

Reads an encode checkpoint, restores the recorded source/output paths, and re-runs the job. Useful for recovering from an interrupted or failed encode without retyping paths. (Current prototype restarts the encode from scratch — the checkpoint is just the job record.)

- **Requirements:** checkpoint must be a `.encode.checkpoint` file with `type: "encode"`, `source_path`, and `output_path` fields. The original source file must still exist.
- **Output side effect:** updates the same checkpoint file to `started`, `failed`, or `complete` as the job runs.

---

## Decode

### Basic decode

```
./build/fsoy decode archive.mkv -o restored.7z
```

Decodes `archive.mkv`, reconstructs the payload, writes `restored.7z`, and verifies its SHA-256 against the embedded header. Tile size, palette, and frame geometry are read from the video's own header frame — you never need to pass them on the decode side, even if the checkpoint is missing (e.g. after the video has been uploaded/downloaded from a third-party service and lost its sidecar files).

- **Requirements:** input must be an FSOY v2 stream, output path must be writable, `ffmpeg` must be on `PATH`.
- **Succeeds only if:** the reconstructed output's SHA-256 matches the embedded SHA-256.
- **Fails if:** `ffmpeg` is missing, stream magic is invalid, the stream is truncated, or SHA-256 verification fails.

### Decode multiple input videos

```
./build/fsoy decode part1.mkv part2.mkv -o restored.7z
```

Reads decoded bytes from each input video in the order given, then reconstructs and verifies one output file. Only the first input needs the header frame; this is mainly scaffolding for the planned multi-volume workflow, since encode currently only writes a single feed volume.

- **Requirements:** every input must be readable by FFmpeg, and together they must contain a complete FSOY v2 stream.

### GPU Suite: process mode

```
./build/fsoy decode archive.mkv -o restored.7z --gpu process
# aliases:
./build/fsoy decode archive.mkv -o restored.7z --gpu do-all
./build/fsoy decode archive.mkv -o restored.7z --gpu auto
```

Attempts FFmpeg CUDA hardware-accelerated video decode, falling back to CPU decode if CUDA hwaccel is unavailable or fails. Chunk/ECC reconstruction always happens on CPU after FFmpeg emits the grayscale luma stream.

- **Requirements:** same as basic decode, plus FFmpeg CUDA hwaccel support and compatible NVIDIA hardware/driver.

### GPU Suite: video decode only

```
./build/fsoy decode archive.mkv -o restored.7z --gpu decode
```

Requests FFmpeg CUDA hardware-accelerated video decode explicitly (no implied frame-synthesis behavior).

- **Requirements:** FFmpeg CUDA hwaccel support and compatible NVIDIA hardware/driver.
- **Fallback:** retries with CPU decode if GPU decode fails.

### Decode checkpoint reuse

```
./build/fsoy decode archive.mkv -o restored.7z --checkpoint restored.7z.decode.checkpoint
```

Reads an existing decode checkpoint, reuses its recorded output path when available, runs decode, and updates the checkpoint.

- **Requirements:** checkpoint must be a `.decode.checkpoint` file with `type: "decode"`. Input video paths still need to be supplied on the command line.
- **Output side effect:** every decode writes `<output-file>.decode.checkpoint` with inputs, output path, decoded payload size, SHA-256, tile/palette, and status.

---

## GPU Suite option reference

| Option | Active with | Purpose | Requirements | Fallback / disabled behavior |
|---|---|---|---|---|
| `--gpu off` (default) | encode, decode | Forces CPU frame synthesis + lossless FFV1 encoding, CPU FFmpeg decode | `ffmpeg` CPU codecs | Disables all optional GPU paths |
| `--gpu process` / `--gpu do-all` / `--gpu auto` | encode, decode | Encode: GPU frame synthesis + NVENC (lossy H.264). Decode: FFmpeg CUDA hwaccel decode | Plugin/CUDA runtime + FFmpeg NVENC (encode); FFmpeg CUDA hwaccel (decode); compatible NVIDIA hardware/driver | Encode falls back to CPU frame synthesis + FFV1; decode retries CPU FFmpeg decode |
| `--gpu frames` | encode | GPU luma-frame synthesis only | Loadable plugin + compatible CUDA driver/device | Falls back to CPU frame synthesis; encoding stays CPU FFV1 (lossless) |
| `--gpu encode` | encode | NVENC video encoding only (lossy H.264) | FFmpeg with `h264_nvenc` + compatible NVIDIA hardware/driver | Retries with CPU FFV1 (lossless) if NVENC fails |
| `--gpu decode` | decode | FFmpeg CUDA hardware-accelerated decode only | FFmpeg CUDA hwaccel + compatible NVIDIA hardware/driver | Retries with CPU FFmpeg decode if GPU decode fails |
| `--gpu-plugin <path>` | encode with `process`/`do-all`/`auto`/`frames` | Selects the GPU plugin shared library to load | Path to a compatible `fsoy_nvidia_gpu` shared library | Ignored unless GPU frame synthesis is requested; falls back to CPU frame synthesis if missing/invalid |

---

## FSOY v2 design notes

- **Tile size (`--tile`, default 1):** each payload bit maps to an N×N solid-color square of pixels. Default `--tile 1` means one payload bit per pixel — 16x denser than v1's fixed 4×4 blocks.
- **Palette (`--palette`, default and only supported value 2):** hard black (0) / white (255) luma threshold at 127, no dithering. This build supports 2-color only; the flag exists for future extension.
- **Video format:** 1920×1080, 24 fps. Default codec is **FFV1 (lossless) in an MKV container** — a genuinely lossless codec (range-coded, no DCT/quantization step), not just a "lossless mode" of a lossy codec. See *"Why lossless, and why it matters more at tile=1"* below.
- **Self-describing stream:** frame 0 of every encode is a dedicated header frame, **always** pixel-sampled at tile=1 regardless of the body's tile size, containing an 84-byte header: magic, original file size, SHA-256, chunk/ECC parameters, `tile`, `palette`, and frame geometry (`width`/`height`/`fps`). Frame 1 onward is the ECC-protected chunk body, sampled at the tile size recorded in that header. This means decode never needs a sidecar checkpoint to know how the video was encoded — it bootstraps entirely from the video itself. (The header's own tile is pinned to 1 specifically to break the chicken-and-egg problem of needing to know the tile size before you can read the field that tells you the tile size.)
- **Streaming:** frames are streamed directly to FFmpeg — never materialized as individual image files.
- **Error correction:** per-chunk checksums, one XOR repair symbol per 10 data chunks (~10% ECC), single-missing/corrupt-chunk recovery per ECC group, file-level SHA-256 verification on decode. Unchanged from v1.
- **GPU Suites:** GPU acceleration is organized into vendor-specific "suites." The NVIDIA GPU Suite is the current work-in-progress; future suites (e.g. an AMD GPU Suite) could follow the same pattern.
  - `--gpu process` / `do-all` / `auto` / `frames` -> loads the NVIDIA GPU Suite plugin for luma-frame synthesis when a CUDA driver is present
  - `--gpu encode` -> attempts FFmpeg NVENC (lossy H.264), falls back to CPU FFV1 (lossless)
  - `--gpu decode` -> attempts FFmpeg CUDA hardware-accelerated decode, falls back to CPU decode

### Why lossless, and why it matters more at tile=1

v1 already used lossless `libx264 -crf 0`. v2 switches to FFV1 instead, and it's a bigger deal than a codec swap: at `--tile 1`, payload pixels form effectively random black/white noise with no spatial or temporal redundancy. That's close to the worst-case input for any block-transform video codec (H.264 included, even at `-crf 0`) — high-frequency, uncorrelated content is expensive to represent exactly, and under any *lossy* rate control the encoder will spend its bit budget elsewhere and quantize that content away, flipping some pixels. FFV1 has no quantization step to begin with, so this isn't a concern for the local `.mkv` artifact this tool produces — it's genuinely bit-exact by construction, independent of how noise-like the payload looks.

### Why this doesn't (and can't) protect you from YouTube specifically

If you upload this `.mkv` to YouTube and download it back, YouTube re-transcodes every upload through its own (lossy) codec ladder — it does not preserve the original bitstream, regardless of what codec you fed it. Nothing on the encode side changes that. `--tile 1`'s single-pixel payload granularity is, if anything, a worse case for surviving that kind of re-encode than v1's 4×4 blocks were — there's no margin for the pixel value to drift before it crosses the black/white threshold. The 10% XOR ECC gives some protection against a small number of cleanly corrupted chunks, but it is **not** validated against real-world lossy re-encoding cascades, which tend to corrupt many chunks at once rather than one cleanly. If you plan to route a file through an actual lossy re-encode step (YouTube or otherwise), consider `--tile 4` or larger for more margin, and treat the result as something to test empirically rather than assume.

## Migrating from v1

v1 (HARE) used a fixed 4×4 block, hardcoded `libx264 -crf 0` encoding, and a `FSOYHARE1` stream magic with no in-stream tile/palette metadata. v2 uses a different (`FSOYV2`) stream magic and is not wire-compatible with v1 — a v2 build cannot decode v1-encoded videos and vice versa. If you have existing HARE-encoded videos, decode them with a v1 build before re-encoding with v2 if you want the density/losslessness improvements.
