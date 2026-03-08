cmake_minimum_required(VERSION 3.22)
project(pump)

set(CMAKE_CXX_STANDARD 26)
add_compile_options(-O0 -Wno-interference-size -march=native)

# 添加src目录到包含路径
include_directories(src)

# 查找所需的包
find_package(PkgConfig REQUIRED)

include(src/env/scheduler/nvme/nvme.cmake)

include(src/env/scheduler/tcp/tcp.cmake)

include(src/env/scheduler/udp/udp.cmake)

# DPDK support
pkg_check_modules(DPDK REQUIRED libdpdk)
include_directories(${DPDK_INCLUDE_DIRS})
link_directories(${DPDK_LIBRARY_DIRS})

add_subdirectory(apps)