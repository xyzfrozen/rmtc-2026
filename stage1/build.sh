#!/usr/bin/env bash
set -eo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source /opt/ros/humble/setup.bash
cmake -S "$HERE" -B "$HERE/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$HERE/build" --parallel 2
ctest --test-dir "$HERE/build" --output-on-failure
