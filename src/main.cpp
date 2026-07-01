/**
 * @file main.cpp
 * @brief 程序入口，组合根。
 *
 * @details
 * M1 阶段只验证构建链路（CMake 能在 RPi 上原生编译出可执行文件），
 * 不接入 UART/MAVLink/MQTT 等业务逻辑，这些从 M2 开始逐步接入本文件。
 */

#include <iostream>

int main() {
  std::cout << "cns_rpi starting (M1 skeleton)" << std::endl;
  return 0;
}
