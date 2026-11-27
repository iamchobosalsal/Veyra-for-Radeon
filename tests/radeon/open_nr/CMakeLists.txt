# SPDX-License-Identifier: GPL-3.0-only
cmake_minimum_required(VERSION 3.24)
project(veyra_open_nr_probe LANGUAGES CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(VEYRA_ROOT "../../.." ABSOLUTE)
if(NOT WIN32 OR NOT MINGW)
  message(FATAL_ERROR "This isolated probe currently uses native Windows llvm-mingw.")
endif()
set(OPEN_NR_SOURCE "" CACHE PATH "Pinned DLSSNR-AMD checkout, commit 191b97b54175d50c7ec5992b54322dd3eb0ef8ac")
set(VULKAN_HEADERS "" CACHE PATH "Vulkan-Headers 1.4.357 checkout")
set(OPEN_NR_GENERATED "" CACHE PATH "Private generated Vulkan dispatch directory")
foreach(file "${OPEN_NR_SOURCE}/windows/src/core/nr_runtime.cpp" "${VULKAN_HEADERS}/include/vulkan/vulkan.h" "${OPEN_NR_GENERATED}/vulkan_dispatch.cpp")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "Missing prerequisite: ${file}")
  endif()
endforeach()
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_compile_options(-O2)
add_link_options(-static)
add_executable(VeyraOpenNR WIN32 OpenNrLauncher.cpp)
target_link_options(VeyraOpenNR PRIVATE -municode)
file(READ "${OPEN_NR_SOURCE}/windows/build/arch/rdna4.sh" arch)
string(REGEX MATCHALL "-DNR_[A-Z0-9_]+=[0-9]+" defines "${arch}")
add_library(open_nr_core STATIC
  "${OPEN_NR_SOURCE}/windows/src/core/nr_runtime.cpp"
  "${OPEN_NR_SOURCE}/windows/src/core/nr_native_plan.cpp"
  "${OPEN_NR_SOURCE}/windows/src/pe/nr_pe_bridge.cpp"
  "${OPEN_NR_SOURCE}/windows/src/pe/nr_pe_interop.cpp"
  "${OPEN_NR_SOURCE}/windows/src/pe/nr_pe_log.cpp"
  "${OPEN_NR_GENERATED}/vulkan_dispatch.cpp"
  "${OPEN_NR_GENERATED}/vulkan_dispatch.s")
foreach(define IN LISTS defines)
  target_compile_options(open_nr_core PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${define}>")
endforeach()
target_include_directories(open_nr_core PUBLIC
  "${OPEN_NR_SOURCE}/windows/src/core" "${OPEN_NR_SOURCE}/windows/src/pe" "${VULKAN_HEADERS}/include")
add_executable(veyra_open_nr_probe "../RadeonNrProbe.cpp"
  "${VEYRA_ROOT}/src/pipeline/NrParityPass.cpp"
  "${VEYRA_ROOT}/src/pipeline/OpenNrBackend.cpp"
  "${VEYRA_ROOT}/src/pipeline/AmdHipPreflight.cpp"
  "${VEYRA_ROOT}/src/gfx/GpuSchedulingPriority.cpp"
  "${VEYRA_ROOT}/src/base/Log.cpp" "${VEYRA_ROOT}/src/base/RuntimePaths.cpp")
target_include_directories(veyra_open_nr_probe PRIVATE "${VEYRA_ROOT}/include")
target_compile_definitions(veyra_open_nr_probe PRIVATE VEYRA_OPEN_NR_PROBE=1)
target_link_options(veyra_open_nr_probe PRIVATE -municode)
target_link_libraries(veyra_open_nr_probe PRIVATE open_nr_core d3d12 d3dcompiler dxgi dxguid ole32 gdi32 shell32 user32)
add_executable(veyra_open_nr_stream_probe OpenNrStreamProbe.cpp
  "${VEYRA_ROOT}/src/pipeline/NrParityPass.cpp"
  "${VEYRA_ROOT}/src/pipeline/OpenNrBackend.cpp" "${VEYRA_ROOT}/src/base/Log.cpp")
target_include_directories(veyra_open_nr_stream_probe PRIVATE "${VEYRA_ROOT}/include")
target_link_options(veyra_open_nr_stream_probe PRIVATE -municode)
target_link_libraries(veyra_open_nr_stream_probe PRIVATE open_nr_core d3d12 d3dcompiler dxgi dxguid ole32 gdi32)
foreach(mode small original)
  add_library(veyra_open_nr_${mode} SHARED
    "${VEYRA_ROOT}/src/pipeline/OpenNrRuntime.cpp"
    "${VEYRA_ROOT}/src/pipeline/OpenNrBackend.cpp"
    "${VEYRA_ROOT}/src/pipeline/NrParityPass.cpp"
    "${VEYRA_ROOT}/src/base/Log.cpp")
  target_include_directories(veyra_open_nr_${mode} PRIVATE "${VEYRA_ROOT}/include")
  target_link_libraries(veyra_open_nr_${mode} PRIVATE open_nr_core d3d12 d3dcompiler dxgi dxguid ole32 gdi32)
  set_target_properties(veyra_open_nr_${mode} PROPERTIES PREFIX "")
endforeach()
target_compile_definitions(veyra_open_nr_original PRIVATE VEYRA_OPEN_NR_ORIGINAL=1)
add_executable(nr_parity_gpu_tests NrParityGpuTests.cpp
  "${VEYRA_ROOT}/src/pipeline/NrParityPass.cpp"
  "${VEYRA_ROOT}/src/parity/RenoDxParityCodec.cpp" "${VEYRA_ROOT}/src/base/Log.cpp")
target_include_directories(nr_parity_gpu_tests PRIVATE "${VEYRA_ROOT}/include")
target_link_options(nr_parity_gpu_tests PRIVATE -municode)
target_link_libraries(nr_parity_gpu_tests PRIVATE d3d12 d3dcompiler dxgi dxguid)
add_executable(open_nr_abi_tests OpenNrAbiTests.cpp)
target_link_options(open_nr_abi_tests PRIVATE -municode)
target_link_libraries(open_nr_abi_tests PRIVATE d3d12 dxgi dxguid)
add_executable(open_nr_model_import "${OPEN_NR_SOURCE}/windows/package/model-tools/dlssnr_extract_model.cpp")
set_target_properties(open_nr_model_import PROPERTIES CXX_STANDARD 17)
target_link_options(open_nr_model_import PRIVATE -municode)
