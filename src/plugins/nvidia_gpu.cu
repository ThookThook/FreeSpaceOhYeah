#include "fsoy/gpu_plugin.h"
#include <cuda_runtime.h>
#include <cstring>
namespace {
__global__ void fill_luma(uint8_t* y, int width, int height, int block, const uint8_t* payload, size_t payload_size, size_t byte_offset) {
  const int blocks_x = width / block;
  const int blocks_y = height / block;
  const int bi = blockIdx.x * blockDim.x + threadIdx.x;
  if (bi >= blocks_x * blocks_y) return;
  const size_t byte = byte_offset + static_cast<size_t>(bi) / 8;
  const bool bit = byte < payload_size && ((payload[byte] >> (7 - (bi % 8))) & 1);
  const uint8_t val = bit ? 255 : 0;
  const int bx = (bi % blocks_x) * block;
  const int by = (bi / blocks_x) * block;
  for (int yy = 0; yy < block; ++yy) {
    for (int xx = 0; xx < block; ++xx) {
      y[(by + yy) * width + bx + xx] = val;
    }
  }
}
bool available() { int count = 0; return cudaGetDeviceCount(&count) == cudaSuccess && count > 0; }
const char* status() { return available() ? "NVIDIA CUDA device detected; CUDA luma synthesis active" : "NVIDIA CUDA device not detected"; }
bool synthesize(const FsoyGpuFrameJob* job) {
  if (!job || !job->y_plane || !job->payload || !available()) return false;
  const size_t y_size = static_cast<size_t>(job->width) * job->height;
  uint8_t* d_y = nullptr;
  uint8_t* d_payload = nullptr;
  if (cudaMalloc(&d_y, y_size) != cudaSuccess) return false;
  if (cudaMalloc(&d_payload, job->payload_size) != cudaSuccess) { cudaFree(d_y); return false; }
  bool ok = cudaMemcpy(d_payload, job->payload, job->payload_size, cudaMemcpyHostToDevice) == cudaSuccess;
  if (ok) ok = cudaMemset(d_y, 128, y_size) == cudaSuccess;
  if (ok) {
    const int total_blocks = (job->width / job->block_size) * (job->height / job->block_size);
    const int threads = 256;
    fill_luma<<<(total_blocks + threads - 1) / threads, threads>>>(d_y, job->width, job->height, job->block_size, d_payload, job->payload_size, job->byte_offset);
    ok = cudaGetLastError() == cudaSuccess && cudaDeviceSynchronize() == cudaSuccess;
  }
  if (ok) ok = cudaMemcpy(job->y_plane, d_y, y_size, cudaMemcpyDeviceToHost) == cudaSuccess;
  cudaFree(d_payload);
  cudaFree(d_y);
  return ok;
}
FsoyGpuPluginApi api{FSOY_GPU_PLUGIN_ABI_VERSION, "fsoy-nvidia-gpu-cuda", status, available, synthesize};
}
extern "C" const FsoyGpuPluginApi* fsoy_get_gpu_plugin_api() { return &api; }
