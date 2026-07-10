#pragma once
#include "fsoy/gpu_plugin.h"
#include <memory>
#include <string>

class GpuPlugin {
 public:
  GpuPlugin() = default;
  ~GpuPlugin();
  GpuPlugin(const GpuPlugin&) = delete;
  GpuPlugin& operator=(const GpuPlugin&) = delete;
  GpuPlugin(GpuPlugin&& other) noexcept;
  GpuPlugin& operator=(GpuPlugin&& other) noexcept;

  static GpuPlugin load(const std::string& explicit_path = {});
  bool loaded() const { return api_ != nullptr; }
  bool available() const { return api_ && api_->is_available && api_->is_available(); }
  const char* name() const { return api_ && api_->name ? api_->name : "none"; }
  std::string status() const;
  bool synthesize_luma_frame(const FsoyGpuFrameJob& job) const;

 private:
  void* handle_ = nullptr;
  const FsoyGpuPluginApi* api_ = nullptr;
};
