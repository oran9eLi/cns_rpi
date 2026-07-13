#include <fstream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::string program = argv[0];
  if (program.find("corrupt") != std::string::npos) {
    std::ofstream(argv[2], std::ios::trunc) << "{";
    return 1;
  }
  if (program.find("wrong") != std::string::npos) {
    std::ofstream(argv[2], std::ios::trunc) << R"({"wrong":true})";
    return 0;
  }
  return 1;
}
