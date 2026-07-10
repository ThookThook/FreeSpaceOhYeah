#include "gpu_loader.h"
#include <cstdlib>
#include <filesystem>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
using namespace std;
namespace {
void* open_library(const string& path) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
  return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
void close_library(void* h) {
  if (!h) return;
#if defined(_WIN32)
  FreeLibrary(reinterpret_cast<HMODULE>(h));
#else
  dlclose(h);
#endif
}
void* symbol(void* h, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), name));
#else
  return dlsym(h, name);
#endif
}
vector<string> candidates(const string& explicit_path) {
  if (!explicit_path.empty()) return {explicit_path};
  vector<string> out;
  if (const char* env = getenv("FSOY_GPU_PLUGIN")) out.emplace_back(env);
#if defined(_WIN32)
  out.emplace_back("fsoy_nvidia_gpu.dll");
#elif defined(__APPLE__)
  out.emplace_back("./libfsoy_nvidia_gpu.dylib");
  out.emplace_back("libfsoy_nvidia_gpu.dylib");
#else
  out.emplace_back("./libfsoy_nvidia_gpu.so");
  out.emplace_back("libfsoy_nvidia_gpu.so");
#endif
  return out;
}
}
GpuPlugin::~GpuPlugin(){ close_library(handle_); }
GpuPlugin::GpuPlugin(GpuPlugin&& o) noexcept { handle_=o.handle_; api_=o.api_; o.handle_=nullptr; o.api_=nullptr; }
GpuPlugin& GpuPlugin::operator=(GpuPlugin&& o) noexcept { if(this!=&o){ close_library(handle_); handle_=o.handle_; api_=o.api_; o.handle_=nullptr; o.api_=nullptr;} return *this; }
GpuPlugin GpuPlugin::load(const string& explicit_path) { GpuPlugin p; for (const auto& c : candidates(explicit_path)) { void* h=open_library(c); if(!h) continue; auto fn=reinterpret_cast<FsoyGetGpuPluginApiFn>(symbol(h,"fsoy_get_gpu_plugin_api")); if(!fn){ close_library(h); continue; } const auto* api=fn(); if(!api || api->abi_version!=FSOY_GPU_PLUGIN_ABI_VERSION){ close_library(h); continue; } p.handle_=h; p.api_=api; return p; } return p; }
string GpuPlugin::status() const { if(!api_) return "no GPU plugin loaded"; if(api_->status) return api_->status(); return available()?"available":"unavailable"; }
bool GpuPlugin::synthesize_luma_frame(const FsoyGpuFrameJob& job) const { return api_ && api_->synthesize_luma_frame && api_->synthesize_luma_frame(&job); }
