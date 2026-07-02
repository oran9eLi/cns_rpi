cmake_minimum_required(VERSION 3.16)
project(cns_rpi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

add_executable(cns_rpi src/main.cpp)

# src/mavlink 是从固件仓库 Third_Party/mavlink/ 原样同步的官方生成头文件（common/standard/minimal），
# 只读、不改；SYSTEM 让 -Wall -Wextra 不对这批第三方代码报警，只管我们自己写的代码。
target_include_directories(cns_rpi SYSTEM PRIVATE src/mavlink)
