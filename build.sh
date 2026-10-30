#!/usr/bin/env bash
set -eo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
bash "$HERE/stage1/build.sh"
bash "$HERE/stage2/build.sh"
