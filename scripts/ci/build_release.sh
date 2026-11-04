#!/usr/bin/env bash
set -euo pipefail
if [ "$(id -u)" -eq 0 ]; then
  echo 'Build and regression tests require an ordinary user' >&2
  exit 1
fi
commit=$1
cmake -S . -B build-ci -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ci -j2
python3 scripts/ci/run_tests.py --build build-ci --logs test-logs 2>&1 | tee test-logs/tests.log
version=$(python3 -c 'import re; print(re.search(r"project\(cns_rpi VERSION ([0-9.]+)",open("CMakeLists.txt").read()).group(1))')
python3 scripts/build_release_package.py --build build-ci --output dist --version "$version" --git-commit "$commit"
