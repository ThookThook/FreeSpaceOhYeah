cmake_minimum_required(VERSION 3.16)
project(FreeSpaceOhYeah VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fsoy src/main.cpp src/gpu_loader.cpp src/gpu_loader.h src/sha256.cpp src/sha256.h)
target_include_directories(fsoy PRIVATE include src)
if(UNIX)
  target_link_libraries(fsoy PRIVATE pthread dl)
endif()

option(FSOY_BUILD_GPU_SUITES "Build optional GPU acceleration suite libraries" ON)
option(FSOY_BUILD_NVIDIA_GPU_SUITE "Build the optional NVIDIA GPU Suite library" ON)

# Backward-compatible aliases for earlier local build scripts.
if(DEFINED FSOY_BUILD_GPU_PLUGINS)
  set(FSOY_BUILD_GPU_SUITES "${FSOY_BUILD_GPU_PLUGINS}" CACHE BOOL "Build optional GPU acceleration suite libraries" FORCE)
endif()
if(DEFINED FSOY_BUILD_NVIDIA_GPU_PLUGIN)
  set(FSOY_BUILD_NVIDIA_GPU_SUITE "${FSOY_BUILD_NVIDIA_GPU_PLUGIN}" CACHE BOOL "Build the optional NVIDIA GPU Suite library" FORCE)
endif()

if(FSOY_BUILD_GPU_SUITES AND FSOY_BUILD_NVIDIA_GPU_SUITE)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    add_library(fsoy_gpu_nvidia SHARED src/plugins/nvidia_gpu.cu)
    target_include_directories(fsoy_gpu_nvidia PRIVATE include)
    set_target_properties(fsoy_gpu_nvidia PROPERTIES
      OUTPUT_NAME fsoy_gpu_nvidia
      CUDA_STANDARD 17
      CUDA_STANDARD_REQUIRED ON)
  else()
    add_library(fsoy_gpu_nvidia SHARED src/plugins/nvidia_gpu.cpp)
    target_include_directories(fsoy_gpu_nvidia PRIVATE include)
    set_target_properties(fsoy_gpu_nvidia PROPERTIES
      OUTPUT_NAME fsoy_gpu_nvidia
      WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(fsoy_gpu_nvidia PRIVATE FSOY_NVIDIA_STUB=1)
  endif()
endif()

install(TARGETS fsoy RUNTIME DESTINATION bin)
if(TARGET fsoy_gpu_nvidia)
  install(TARGETS fsoy_gpu_nvidia
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
endif()
