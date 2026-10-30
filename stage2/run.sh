#!/usr/bin/env bash
set -eo pipefail
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source /opt/ros/humble/setup.bash
export ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-27}"
export ROS_LOCALHOST_ONLY="${ROS_LOCALHOST_ONLY:-0}"
export RMW_IMPLEMENTATION="${RMW_IMPLEMENTATION:-rmw_fastrtps_cpp}"
export ROS_LOG_DIR="${ROS_LOG_DIR:-$HERE/logs/ros}"
mkdir -p "$ROS_LOG_DIR"
if [[ ! -x "$HERE/build/battle_node" ]];then
 echo '请先执行 build.sh。' >&2
 exit 1
fi
exec "$HERE/build/battle_node" "$@"
