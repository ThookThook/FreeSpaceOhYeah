cmake_minimum_required(VERSION 3.16)
project(FreeSpaceOhYeah VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fsoy src/main.cpp src/gpu_loader.cpp src/gpu_loader.h src/sha256.cpp src/sha256.h)
target_include_directories(fsoy PRIVATE include src)

if(DEFINED ENV{PREFIX} AND "$ENV{PREFIX}" MATCHES "com\\.termux")
  set(IS_TERMUX TRUE)
else()
  set(IS_TERMUX FALSE)
endif()

if(UNIX AND NOT IS_TERMUX)
  target_link_libraries(fsoy PRIVATE pthread dl)
elseif(IS_TERMUX)
  target_link_libraries(fsoy PRIVATE pthread)
endif()

option(FSOY_BUILD_NVIDIA_PLUGIN "Build the optional NVIDIA GPU add-on library" ON)
if(FSOY_BUILD_NVIDIA_PLUGIN)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    add_library(fsoy_nvidia_gpu SHARED src/plugins/nvidia_gpu.cu)
    target_include_directories(fsoy_nvidia_gpu PRIVATE include)
    set_target_properties(fsoy_nvidia_gpu PROPERTIES
      OUTPUT_NAME fsoy_nvidia_gpu
      CUDA_STANDARD 17
      CUDA_STANDARD_REQUIRED ON)
  else()
    add_library(fsoy_nvidia_gpu SHARED src/plugins/nvidia_gpu.cpp)
    target_include_directories(fsoy_nvidia_gpu PRIVATE include)
    set_target_properties(fsoy_nvidia_gpu PROPERTIES
      OUTPUT_NAME fsoy_nvidia_gpu
      WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_compile_definitions(fsoy_nvidia_gpu PRIVATE FSOY_NVIDIA_STUB=1)
  endif()
endif()

install(TARGETS fsoy RUNTIME DESTINATION bin)
if(TARGET fsoy_nvidia_gpu)
  install(TARGETS fsoy_nvidia_gpu
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
endif()
