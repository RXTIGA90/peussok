cmake_minimum_required(VERSION 3.16)
project(PEUSSOKEngine LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../third_party")
set(RIFE "${THIRD_PARTY}/rife-ncnn-vulkan/src")
set(Vulkan_INCLUDE_DIR "${THIRD_PARTY}/Vulkan-Headers/include" CACHE PATH "" FORCE)

# Import library for vulkan-1.dll (the Vulkan loader that ships with the GPU driver), made from vulkan-1.def.
set(VULKAN_IMPLIB "${CMAKE_CURRENT_BINARY_DIR}/vulkan-1.lib")
add_custom_command(OUTPUT "${VULKAN_IMPLIB}"
  COMMAND "${CMAKE_C_COMPILER}" dlltool -m i386:x86-64 -d "${CMAKE_CURRENT_SOURCE_DIR}/vulkan-1.def" -l "${VULKAN_IMPLIB}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vulkan-1.def" VERBATIM)
add_custom_target(vulkan_implib DEPENDS "${VULKAN_IMPLIB}")
set(Vulkan_LIBRARY "${VULKAN_IMPLIB}" CACHE FILEPATH "" FORCE)

set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
foreach(option OPENMP INSTALL_SDK BUILD_TOOLS BUILD_BENCHMARK BUILD_EXAMPLES BUILD_TESTS ENABLE_LTO RUNTIME_CPU AVX AVX2 AVX512 INT8 PIXEL_ROTATE PIXEL_AFFINE PIXEL_DRAWING PYTHON)
  set(NCNN_${option} OFF CACHE BOOL "" FORCE)
endforeach()
# Mirror the upstream RIFE layer selection; keep its MIT notice in third_party.
file(STRINGS "${RIFE}/CMakeLists.txt" RIFE_OPTIONS REGEX "^[ ]*option\\(WITH_LAYER_")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/rife_layers.cmake" "")
foreach(line IN LISTS RIFE_OPTIONS)
  file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/rife_layers.cmake" "${line}\n")
endforeach()
include("${CMAKE_CURRENT_BINARY_DIR}/rife_layers.cmake")
set(WITH_LAYER_memorydata ON CACHE BOOL "" FORCE)
foreach(layer prelu eltwise innerproduct pooling reorg pixelshuffle padding)
 set(WITH_LAYER_${layer} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${RIFE}/ncnn" ncnn)
add_dependencies(ncnn vulkan_implib)
file(GLOB SHADERS "${RIFE}/*.comp")
foreach(shader IN LISTS SHADERS)
  get_filename_component(name "${shader}" NAME_WE)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.comp.hex.h")
  add_custom_command(OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" "-DSHADER_SRC=${shader}" "-DSHADER_COMP_HEADER=${output}" -P "${RIFE}/generate_shader_comp_header.cmake"
    DEPENDS "${shader}" VERBATIM)
  list(APPEND HEADERS "${output}")
endforeach()

# AMD FidelityFX SDK 1.1.4 (MIT): optical flow + frame interpolation, Vulkan backend only.
# Shader headers in shaders-generated were compiled from the SDK GLSL with glslang 11.9 (no optimizer).
set(FFX "${THIRD_PARTY}/fidelityfx-sdk-1.1.4")
set(FFX_SOURCES "${FFX}/src/backends/vk/ffx_vk.cpp" "${FFX}/src/backends/shared/ffx_shader_blobs.cpp"
  "${FFX}/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp" "${FFX}/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp"
  "${FFX}/src/shared/ffx_assert.cpp" "${FFX}/src/shared/ffx_message.cpp" "${FFX}/src/shared/ffx_object_management.cpp" "${FFX}/src/shared/ffx_breadcrumbs_list.cpp"
  "${FFX}/src/components/opticalflow/ffx_opticalflow.cpp" "${FFX}/src/components/frameinterpolation/ffx_frameinterpolation.cpp")
set_source_files_properties(${FFX_SOURCES} PROPERTIES COMPILE_OPTIONS "-fms-extensions;-Wno-everything;-include;mutex" COMPILE_DEFINITIONS "UNICODE;_UNICODE")

# The engine DLL that the installer ships.
add_library(peussok_engine SHARED engine.cpp gpu_keeper.cpp "${RIFE}/rife.cpp" "${RIFE}/warp.cpp" fsr3_engine.cpp ffx_swapchain_stub.cpp ${FFX_SOURCES} ${HEADERS})
target_compile_definitions(peussok_engine PRIVATE FFX_OF FFX_FI PK_GPU_KEEPER=1)
target_include_directories(peussok_engine PRIVATE "${RIFE}" "${CMAKE_CURRENT_BINARY_DIR}" "${FFX}/include" "${FFX}/src/shared" "${FFX}/src/backends/shared" "${FFX}/src/components" "${FFX}/src/backends/vk" "${FFX}/shaders-generated" "${Vulkan_INCLUDE_DIR}")
target_link_libraries(peussok_engine PRIVATE ncnn bcrypt wintrust "${VULKAN_IMPLIB}")
add_dependencies(peussok_engine vulkan_implib)
# NVIDIA VFG (runtime downloaded by the user), AMD FSR FG (AMD signed DLLs, binary license) and the GPU path are in
# every build, and DLSS FG.
target_sources(peussok_engine PRIVATE vfg_engine.cpp amdfg_engine.cpp gpu_interop.cpp seq_gpu.cpp dlssg_engine.cpp) # 2.3.4: DLSS FG public
target_link_libraries(peussok_engine PRIVATE d3d11 dxgi)
target_compile_definitions(peussok_engine PRIVATE PK_INTERNAL_VFG=1 PK_INTERNAL_DLSSG=1)

set_target_properties(peussok_engine PROPERTIES PREFIX "")

