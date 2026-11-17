cmake_minimum_required(VERSION 3.22)
project(vkprobe C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(NODE_MODULE vkprobe)

add_library(${NODE_MODULE} SHARED
  vk_probe_napi.cpp
  vk_probe_core.c
)

find_library(VULKAN_LIB vulkan)
find_library(HLOG_LIB hilog_ndk.z)
find_library(ACE_NAPI ace_napi.z)

target_link_libraries(${NODE_MODULE} PUBLIC ${VULKAN_LIB} ${HLOG_LIB} ${ACE_NAPI})
target_compile_options(${NODE_MODULE} PRIVATE -Wno-format -Wno-unused-variable -Wno-unused-function)
