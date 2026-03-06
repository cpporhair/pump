cmake_minimum_required(VERSION 3.22)
project(pump)

set(CMAKE_CXX_STANDARD 26)
add_compile_options(-O0 -Wno-interference-size)

# 添加src目录到包含路径
include_directories(src)

# 查找所需的包
find_package(PkgConfig REQUIRED)

include(src/env/scheduler/nvme/nvme.cmake)

include(src/env/scheduler/tcp/tcp.cmake)

add_subdirectory(apps)