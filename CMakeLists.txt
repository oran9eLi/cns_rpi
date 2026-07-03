cmake_minimum_required(VERSION 3.16)
project(cns_rpi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

find_package(nlohmann_json 3.2.0 REQUIRED)

enable_testing()

# cns_rpi_core：uart/、config/ 等和 main() 无关的业务代码单独成库，
# 这样 tests/ 下的单测能直接链接这些代码，不用把 main() 也一起拖进测试二进制。
add_library(cns_rpi_core
    src/uart/serial_port.cpp
)
target_include_directories(cns_rpi_core PUBLIC src)
# src/mavlink 是从固件仓库 Third_Party/mavlink/ 原样同步的官方生成头文件（common/standard/minimal），
# 只读、不改；SYSTEM 让 -Wall -Wextra 不对这批第三方代码报警，只管我们自己写的代码。
target_include_directories(cns_rpi_core SYSTEM PUBLIC src/mavlink)
target_link_libraries(cns_rpi_core PUBLIC nlohmann_json::nlohmann_json)

add_executable(cns_rpi src/main.cpp)
target_link_libraries(cns_rpi PRIVATE cns_rpi_core)
