## 技术报告

[RMTC](https://www.yuque.com/g/xyzfrozen/mcnuvk/zi8pklv11iulo6er/collaborator/join?token=7Lj8quChuY1Plt1U&source=doc_collaborator# 《RMTC》)

## 获取与运行环境

Ubuntu 22.04、x86_64、ROS 2 Humble、C++17、Python 3.10。主办方公开练习裁判是x86_64程序。新电脑详细步骤见[迁移与验收](docs/迁移与验收.md)，可交给另一台电脑上Codex执行的任务卡见[迁移提示词](docs/迁移提示词.md)。

已登录GitHub且有访问权限时：

```bash
gh repo clone xyzfrozen/rmtc-2026 "$HOME/Documents/rmtc-2026"
cd "$HOME/Documents/rmtc-2026"
git checkout --detach v1.2-20261030
python3 tools/verify_source.py
```

构建依赖：ROS的`ament_cmake`、`rclcpp`、`std_msgs`和`rmw_fastrtps_cpp`，以及`build-essential`、`cmake`、`pkg-config`、`libjsoncpp-dev`、`libcurl4-openssl-dev`、`python3`、`iproute2`、`tzdata`。自检另需`ros-humble-rclpy`。所有脚本根据自身位置定位程序，不依赖原开发电脑用户名或目录。

```bash
bash build.sh
```

构建会分别编译两个阶段，并执行3项C++测试。无需`colcon build`或本仓库的`install/setup.bash`；脚本自行加载`/opt/ros/humble/setup.bash`。依赖缺失时按[安装步骤](docs/迁移与验收.md)补齐。构建前不要复制另一台电脑的`build/`目录。

## 本机基础自检

在没有正式比赛或其他节点使用测试域85时执行：

```bash
bash tools/smoke_ros.sh --domain 85
```

此工具启动实际两阶段程序，每阶段发送20帧**合成感知**，检查队名注册、Tick/场次回填、动作与本地模拟HTTP请求。日志保存在`logs/`，不使用真实密钥、不消费模型额度。它不是官方裁判、完整对局、执行回执或事件语义认证；完整练习仍使用主办方测试包，见[本机练习与截图](docs/本机练习与截图.md)。

## 正式比赛启动

先在仓库根目录打开终端，设置本次终端中的路径变量，后续命令不再受当前目录影响：

```bash
RMTC_REPO="$(pwd)"
test -f "$RMTC_REPO/config/match.json" || cp "$RMTC_REPO/config/match.example.json" "$RMTC_REPO/config/match.json"
```

填写`config/match.json`中的有线网卡名和官方网关完整地址。静态IPv4与掩码在Ubuntu网络设置中按裁判分配填写。每场必须在命令中显式指定域、席位和报名队名；**下文27、29、B及队名都是示例，必须使用实际分配值**。

第一阶段固定A席位，无需令牌：

```bash
bash "$RMTC_REPO/start.sh" stage1 --ros-domain-id 27 --team A --team-name '你的报名队名'
```

结束后按裁判安排停止旧程序。第二阶段使用本场席位：

```bash
bash "$RMTC_REPO/start.sh" stage2 --ros-domain-id 29 --team B --team-name '你的报名队名'
```

第二阶段启动后在终端隐藏输入本场席位令牌；令牌不放进命令、配置文件或仓库。网关改变时，更新配置或在命令中追加`--gateway-url 'http://实际裁判IP:实际端口/v1/chat/completions'`。本队通常不随换场修改报名队名。

程序固定默认使用`deepseek-flash`、`max_tokens=512`、关闭思考、JSON输出和非流式请求；每场最多尝试80次、并发1，本地动作不等待HTTP。正式有线比赛不加`--local`；同机公开测试才加。存活时第二阶段以`defend`作为原地动作，阵亡时可`wait`。

完整网络、令牌、启动及排错说明见[现场启动说明](docs/现场启动说明.md)。`--check`只检查接入配置和网关`/healthz`，不验证正式令牌或双向ROS；公开练习包v1.1没有`/healthz`，本机练习按练习文档直接启动。

## 目录与主要逻辑

| 路径 | 用途 |
|---|---|
| `stage1/src/node.cpp` | 第一阶段感知订阅、动作发布、队名注册和日志 |
| `stage1/src/collector.cpp` | 地图记忆、探索、采集目标选择与寻路 |
| `stage2/src/node.cpp`、`protocol.cpp` | 第二阶段消息校验、场次切换、异步请求与动作发布 |
| `stage2/src/bot.cpp`、`combat.cpp` | 加权最短路、目标选择、伤害/死亡风险与战斗决策 |
| `stage2/src/events.cpp`、`gateway.cpp` | 事件结构校验与效果应用、后台HTTP调用 |
| `stage2/include/event_prompt.hpp` | 当前事件解析Prompt |
| `stage2/deps/stage1/` | 第二阶段依赖的冻结采集核心副本，仓库可独立构建 |
| `config/match.example.json` | 无凭据接入模板 |
| `tools/launch.py` | 参数校验、隐藏令牌输入、单实例锁、有线DDS配置 |
| `tools/verify_source.py`、`MANIFEST.json` | 源码SHA-256核对；二进制哈希仅供原构建记录 |
| `tools/smoke_ros.sh`、`tools/show_precheck.py` | 合成通信自检；真实运行日志中的动作/回执展示 |
| `docs/` | 环境、迁移、本机练习、正式启动、验证范围 |
| `logs/`、`local/`、`third_party/` | 运行日志、本机报告与官方练习包；生成后留本地，不提交 |

程序按“本队感知 → JSON校验及绝对位置恢复 → 地图和任务状态更新 → 本地选路/战斗决策 → 原样回填Tick/session并发布动作”运行。公开事件文本进入后台网关，结果经验证后影响后续决策。新场次清空旧状态，重复帧不重复推进规划；后续感知的`self.last_action`用于核对执行结果。
