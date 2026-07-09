#pragma once
#include <cstddef>
#include <cstdint>

#define FSOY_GPU_PLUGIN_ABI_VERSION 1

extern "C" {
struct FsoyGpuFrameJob {
  uint8_t* y_plane;
  int width;
  int height;
  int block_size;
  const uint8_t* payload;
  size_t payload_size;
  size_t byte_offset;
};

struct FsoyGpuPluginApi {
  uint32_t abi_version;
  const char* name;
  const char* (*status)();
  bool (*is_available)();
  bool (*synthesize_luma_frame)(const FsoyGpuFrameJob* job);
};

typedef const FsoyGpuPluginApi* (*FsoyGetGpuPluginApiFn)();
const FsoyGpuPluginApi* fsoy_get_gpu_plugin_api();
}
