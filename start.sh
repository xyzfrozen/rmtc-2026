#!/usr/bin/env bash
set -eo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec /usr/bin/python3 "$HERE/tools/launch.py" "$@"
