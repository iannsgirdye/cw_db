#!/usr/bin/env bash
# Install build dependencies on Debian/Ubuntu/WSL.
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
  echo "This script targets apt-based systems (WSL/Ubuntu/Debian)."
  exit 1
fi

echo "Installing system packages..."
sudo DEBIAN_FRONTEND=noninteractive apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  build-essential \
  cmake \
  git \
  flex \
  bison \
  nlohmann-json3-dev \
  libasio-dev

echo "Done. Crow is fetched by CMake FetchContent into build/_deps/ on first configure."
echo "Next: mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && cmake --build . -j"
