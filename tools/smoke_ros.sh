#!/usr/bin/env bash
set -eo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source /opt/ros/humble/setup.bash
exec /usr/bin/python3 "$HERE/smoke_ros.py" "$@"
