cmake_minimum_required(VERSION 3.20)
project(shader_dump_native_tests LANGUAGES CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 20)
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(UTIL "${REPO}/source_third_party/khronos/vulkan-utilities")
add_executable(shader_dump_native_tests
    shader_dump_native_tests.cpp
    "${REPO}/layer_gpu_support/source/shader_dump.cpp"
    "${REPO}/layer_gpu_support/source/layer_device_functions_shader_dump.cpp"
    "${UTIL}/src/vulkan/vk_safe_struct_core.cpp"
    "${UTIL}/src/vulkan/vk_safe_struct_ext.cpp"
    "${UTIL}/src/vulkan/vk_safe_struct_khr.cpp"
    "${UTIL}/src/vulkan/vk_safe_struct_manual.cpp"
    "${UTIL}/src/vulkan/vk_safe_struct_utils.cpp"
    "${UTIL}/src/vulkan/vk_safe_struct_vendor.cpp")
target_include_directories(shader_dump_native_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${REPO}/source_common"
    "${REPO}/source_third_party"
    "${REPO}/layer_gpu_support/source"
    "${REPO}/source_third_party/khronos/vulkan/include"
    "${UTIL}/include")
target_compile_definitions(shader_dump_native_tests PRIVATE CONFIG_LOG=0)
if(MSVC)
    target_compile_options(shader_dump_native_tests PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/compat/compiler.hpp")
endif()
add_test(NAME shader_dump_native_tests COMMAND shader_dump_native_tests)
