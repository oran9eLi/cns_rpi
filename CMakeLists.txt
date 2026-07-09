cmake_minimum_required(VERSION 3.16)
project(cns_rpi LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

find_package(nlohmann_json 3.2.0 REQUIRED)

enable_testing()

find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)

# cns_rpi_core：uart/、config/ 等和 main() 无关的业务代码单独成库，
# 这样 tests/ 下的单测能直接链接这些代码，不用把 main() 也一起拖进测试二进制。
add_library(cns_rpi_core
    src/uart/serial_port.cpp
    src/uart/mavlink_link.cpp
    src/config/app_config.cpp
    src/state/state_store.cpp
    src/protocol/telemetry_decoder.cpp
    src/protocol/extension_decoder.cpp
    src/protocol/identity.cpp
    src/payload/json_serializer.cpp
    src/mqtt/topic.cpp
)
target_include_directories(cns_rpi_core PUBLIC src)
# src/mavlink 是从固件仓库 Third_Party/mavlink/ 原样同步的官方生成头文件（common/standard/minimal），
# 只读、不改；SYSTEM 让 -Wall -Wextra 不对这批第三方代码报警，只管我们自己写的代码。
target_include_directories(cns_rpi_core SYSTEM PUBLIC src/mavlink)
target_link_libraries(cns_rpi_core PUBLIC nlohmann_json::nlohmann_json)

# cns_rpi_mqtt 独立成库，不并入 cns_rpi_core：只有它需要链接真实的
# libmosquitto，这样 cns_rpi_core 的其他消费者（各 test_* target）
# 不会被迫在链接时也依赖 libmosquitto。
add_library(cns_rpi_mqtt src/mqtt/mqtt_client.cpp)
target_include_directories(cns_rpi_mqtt PUBLIC src)
target_link_libraries(cns_rpi_mqtt PUBLIC PkgConfig::MOSQUITTO)

add_executable(cns_rpi src/main.cpp)
target_link_libraries(cns_rpi PRIVATE cns_rpi_core)

add_executable(test_mavlink_link tests/test_mavlink_link.cpp)
target_link_libraries(test_mavlink_link PRIVATE cns_rpi_core)
add_test(NAME mavlink_link COMMAND test_mavlink_link)

add_executable(test_app_config tests/test_app_config.cpp)
target_link_libraries(test_app_config PRIVATE cns_rpi_core)
add_test(NAME app_config COMMAND test_app_config)

add_executable(test_state_store tests/test_state_store.cpp)
target_link_libraries(test_state_store PRIVATE cns_rpi_core)
add_test(NAME state_store COMMAND test_state_store)

add_executable(test_telemetry_decoder tests/test_telemetry_decoder.cpp)
target_link_libraries(test_telemetry_decoder PRIVATE cns_rpi_core)
add_test(NAME telemetry_decoder COMMAND test_telemetry_decoder)

add_executable(test_extension_decoder tests/test_extension_decoder.cpp)
target_link_libraries(test_extension_decoder PRIVATE cns_rpi_core)
add_test(NAME extension_decoder COMMAND test_extension_decoder)

add_executable(test_identity tests/test_identity.cpp)
target_link_libraries(test_identity PRIVATE cns_rpi_core)
add_test(NAME identity COMMAND test_identity)
# ctest 默认工作目录固定为构建目录（build/），与调用 ctest 时的 CWD 无关；
# test_identity.cpp 里的 fixture 用的是相对路径 tests/fixtures/...，必须显式
# 指回仓库根目录，否则不管从哪里调用 ctest 都会因为找不到 fixture 文件而失败。
set_tests_properties(identity PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_json_serializer tests/test_json_serializer.cpp)
target_link_libraries(test_json_serializer PRIVATE cns_rpi_core)
add_test(NAME json_serializer COMMAND test_json_serializer)

add_executable(test_mqtt_topic tests/test_mqtt_topic.cpp)
target_link_libraries(test_mqtt_topic PRIVATE cns_rpi_core)
add_test(NAME mqtt_topic COMMAND test_mqtt_topic)
