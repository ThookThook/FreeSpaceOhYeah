# Free Space? Oh Yeah!

`fsoy` is a C++20 CLI prototype for HARE v1 file-to-video storage. It encodes an input file into H.264 MP4 frames using Channel A luma-only 4×4 blocks and decodes those videos back to the original bytes with SHA-256 verification.

## Build

```bash
cmake -S . -B build
cmake --build build
```

Requires `ffmpeg` on `PATH` for MP4 encode/decode. The optional NVIDIA GPU add-on is built as a shared library (`libfsoy_nvidia_gpu.so`, `fsoy_nvidia_gpu.dll`, or `libfsoy_nvidia_gpu.dylib`) and loaded at runtime when requested.

## Usage

```bash
./build/fsoy encode archive.7z -o archive.mp4
./build/fsoy decode archive.mp4 -o restored.7z
./build/fsoy encode archive.7z -o archive.mp4 --gpu auto
./build/fsoy encode archive.7z -o archive.mp4 --gpu frames --gpu-plugin ./build/libfsoy_nvidia_gpu.so
```

Every encode writes a sidecar checkpoint named `<output>.checkpoint` containing the source path, size, SHA-256, HARE parameters, output list, and completion status.

## HARE v1 choices in this prototype

- Channel A only: one bit per 4×4 luma block, thresholded as black/white.
- 1920×1080, 24 fps, H.264 Main Profile MP4 through FFmpeg.
- Streaming feed to FFmpeg; the full set of video frames is not materialized as image files.
- File-level SHA-256 verification on decode.
- GPU support is an optional NVIDIA add-on library. `--gpu auto` / `--gpu frames` loads the add-on for luma frame synthesis when a CUDA driver is present; `--gpu encode` attempts FFmpeg NVENC and falls back to CPU `libx264`.

## Current limitations

The repository now contains the runnable scaffold for v1, but full rateless/Tornado ECC recovery, executable checkpoint resume, and true multi-volume splitting are still implementation TODOs. When CMake finds NVCC/CUDA, the NVIDIA add-on builds a CUDA kernel for luma frame synthesis; otherwise it builds a driver-detecting stub so the main CLI still compiles without a GPU toolchain.
