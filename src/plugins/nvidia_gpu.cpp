#include "fsoy/gpu_plugin.h"
#include <cstring>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace {
bool nvidia_driver_present() {
#if defined(_WIN32)
  HMODULE h = LoadLibraryA("nvcuda.dll");
  if (!h) return false;
  FreeLibrary(h);
  return true;
#else
  void* h = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!h) h = dlopen("libcuda.so", RTLD_NOW | RTLD_LOCAL);
  if (!h) return false;
  dlclose(h);
  return true;
#endif
}
const char* status() { return nvidia_driver_present() ? "NVIDIA CUDA driver detected; luma synthesis plugin active" : "NVIDIA CUDA driver not detected"; }
bool synthesize(const FsoyGpuFrameJob* job) {
  if (!job || !job->y_plane || !job->payload) return false;
  if (!nvidia_driver_present()) return false;
  std::memset(job->y_plane, 128, static_cast<size_t>(job->width) * job->height);
  const size_t blocks_x = static_cast<size_t>(job->width / job->block_size);
  const size_t blocks_y = static_cast<size_t>(job->height / job->block_size);
  for (size_t bi = 0; bi < blocks_x * blocks_y; ++bi) {
    const size_t byte = job->byte_offset + bi / 8;
    const bool bit = byte < job->payload_size && ((job->payload[byte] >> (7 - (bi % 8))) & 1);
    const int bx = static_cast<int>((bi % blocks_x) * job->block_size);
    const int by = static_cast<int>((bi / blocks_x) * job->block_size);
    const uint8_t val = bit ? 255 : 0;
    for (int yy = 0; yy < job->block_size; ++yy) {
      std::memset(job->y_plane + static_cast<size_t>(by + yy) * job->width + bx, val, job->block_size);
    }
  }
  return true;
}
FsoyGpuPluginApi api{FSOY_GPU_PLUGIN_ABI_VERSION, "fsoy-nvidia-gpu", status, nvidia_driver_present, synthesize};
}
extern "C" const FsoyGpuPluginApi* fsoy_get_gpu_plugin_api() { return &api; }
