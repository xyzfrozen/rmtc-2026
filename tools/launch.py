#!/usr/bin/env python3
"""Match access configuration only; strategies and prompts stay in the frozen binary."""
import argparse,datetime,fcntl,getpass,ipaddress,json,os,pathlib,subprocess,sys,urllib.parse,urllib.request
from zoneinfo import ZoneInfo

ROOT=pathlib.Path(__file__).resolve().parents[1]

def fail(message):raise SystemExit(message)

def dds_xml(address):
    address=str(ipaddress.IPv4Address(address))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
        '<profiles xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">'
        '<transport_descriptors><transport_descriptor><transport_id>rmtc_udp</transport_id>'
        '<type>UDPv4</type><interfaceWhiteList><address>'+address+'</address></interfaceWhiteList>'
        '</transport_descriptor></transport_descriptors><participant profile_name="rmtc_wired" is_default_profile="true">'
        '<rtps><userTransports><transport_id>rmtc_udp</transport_id></userTransports>'
        '<useBuiltinTransports>false</useBuiltinTransports></rtps></participant></profiles>\n')

def main():
    ap=argparse.ArgumentParser(description='本机参赛程序：先检查配置，每场重新输入令牌。')
    ap.add_argument('stage',choices=['stage1','stage2'])
    ap.add_argument('--ros-domain-id',type=int,required=True,metavar='N',help='本场裁判分配的 ROS_DOMAIN_ID')
    ap.add_argument('--team',choices=['A','B','C','D'],required=True,help='本场席位；第一阶段必须为 A')
    ap.add_argument('--team-name',required=True,help='报名队名，用引号包住；换席位无需改队名')
    ap.add_argument('--gateway-url',help='覆盖配置中的本场 AI 网关完整请求地址')
    ap.add_argument('--config',type=pathlib.Path,default=ROOT/'config/match.json')
    ap.add_argument('--check',action='store_true',help='只检查配置和网关健康，不输入令牌或启动参赛车')
    ap.add_argument('--local',action='store_true',help='仅用于同机练习；正式网线比赛不能使用')
    args=ap.parse_args()
    if not args.config.is_file():fail('先复制 config/match.example.json 为 config/match.json，填写网卡名和裁判网关地址。')
    try:cfg=json.loads(args.config.read_text())
    except (ValueError,OSError):fail('配置不是有效的 JSON 文件。')
    if not isinstance(cfg,dict):fail('配置必须是 JSON 对象。')
    domain=args.ros_domain_id;name=args.team_name;team=args.team
    if not 0<=domain<=232:fail('--ros-domain-id 须填写裁判给出的 0–232 整数，不能照抄练习域。')
    if args.stage=='stage1' and team!='A':fail('第一阶段固定使用 A 席位，请填写 --team A。')
    if not isinstance(name,str) or not 1<=len(name.encode())<=96 or any(c in name for c in '\r\n\x00') or '填写' in name:fail('请填写实际队名：1–96 个 UTF-8 字节，不能含换行。')
    if not pathlib.Path('/opt/ros/humble/setup.bash').exists():fail('尚未安装 ROS 2 Humble。')
    binary=ROOT/args.stage/'build'/('collector_node' if args.stage=='stage1' else 'battle_node')
    if not os.access(binary,os.X_OK):fail('找不到已构建的程序；请先运行 bash build.sh。')
    gateway=args.gateway_url if args.gateway_url is not None else cfg.get('gateway_url','')
    if args.stage=='stage2':
        if not isinstance(gateway,str):fail('gateway_url 必须是字符串。')
        try:
            url=urllib.parse.urlsplit(gateway);port=url.port
            if url.scheme not in ('http','https') or not url.hostname or url.username or url.password or url.path!='/v1/chat/completions' or url.query or url.fragment or '填写' in gateway:raise ValueError()
        except ValueError:fail('gateway_url 须填写完整 http(s)://裁判机地址:端口/v1/chat/completions，不能包含令牌。')
        try:loopback=ipaddress.ip_address(url.hostname).is_loopback
        except ValueError:loopback=url.hostname.rstrip('.').lower()=='localhost'
        if not args.local and loopback:fail('正式比赛不能把网关指向本机；同机练习请显式加 --local。')
    interface=cfg.get('network_interface','')
    local_ip=''
    if not args.local:
        if not isinstance(interface,str) or not interface or '/' in interface or not (pathlib.Path('/sys/class/net')/interface).is_dir():fail('network_interface 不存在；用 ip -br address 查看有线网卡名并填写。')
        data=json.loads(subprocess.check_output(['ip','-j','-4','address','show','dev',interface],text=True))
        addresses=[a['local'] for d in data for a in d.get('addr_info',[]) if a.get('scope')=='global']
        if not addresses:fail('该网卡还没有 IPv4 地址。先按裁判分配填写静态 IPv4 和掩码，再启动。')
        local_ip=str(ipaddress.IPv4Address(addresses[0]))
    print(f'阶段={args.stage}；席位={team}；队名={name}；ROS_DOMAIN_ID={domain}；仅本机={int(args.local)}',flush=True)
    if local_ip:print(f'有线网卡={interface}，本机地址={local_ip}；DDS 使用该接口。',flush=True)
    if args.check:
        if args.stage=='stage2':
            health=urllib.parse.urlunsplit((url.scheme,url.netloc,'/healthz','',''))
            try:
                http=urllib.request.build_opener(urllib.request.ProxyHandler({}))
                with http.open(health,timeout=3) as r:
                    if not 200<=r.status<300:raise OSError()
            except Exception:fail('网关 /healthz 未连通；核对网线、IP、端口和裁判服务。未发送模型请求、未消耗调用配额。')
            print('网关健康检查通过；这不验证令牌、ROS 双向通信或本场 AI 资格。')
        print('配置检查完成，未启动参赛车。');return
    token=''
    if args.stage=='stage2':
        if not sys.stdin.isatty():fail('请在交互终端启动并隐藏输入本场令牌。')
        token=getpass.getpass('粘贴本场席位令牌后按回车（不显示、不存盘）：').strip()
        if not token or any(c.isspace() or ord(c)<32 for c in token):fail('令牌为空或含空白/控制字符。')
    logs=ROOT/'logs';logs.mkdir(exist_ok=True)
    lock=(logs/'contestant.lock').open('a')
    try:fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    except BlockingIOError:fail('本目录已有参赛程序运行。先 Ctrl+C 停止旧程序，不能重复启动。')
    os.set_inheritable(lock.fileno(),True)
    stamp=datetime.datetime.now(ZoneInfo('Asia/Shanghai')).strftime('%Y%m%d-%H%M%S')
    run=logs/f'{stamp}-{args.stage}-{team}';run.mkdir()
    env=dict(os.environ,ROS_DOMAIN_ID=str(domain),ROS_LOCALHOST_ONLY=str(int(args.local)),
        RMW_IMPLEMENTATION='rmw_fastrtps_cpp',ROS_LOG_DIR=str(run/'ros'))
    for key in ('RMTC_TEAM_TOKEN','RMTC_AI_GATEWAY','RMTC_AI_MODEL','RMTC_AI_EFFORT','FASTRTPS_DEFAULT_PROFILES_FILE','FASTDDS_DEFAULT_PROFILES_FILE','ROS_DISCOVERY_SERVER','RMW_FASTRTPS_USE_QOS_FROM_XML'):
        env.pop(key,None)
    if local_ip:
        profile=run/'dds.xml'
        profile.write_text(dds_xml(local_ip))
        env['FASTRTPS_DEFAULT_PROFILES_FILE']=str(profile)
    command=['bash',str(ROOT/args.stage/'run.sh'),'--ros-args','-p','team:='+team,'-p','team_name:='+name,'-p','record_path:='+str(run/'decisions.jsonl')]
    if args.stage=='stage2':
        env['RMTC_TEAM_TOKEN']=token
        command+=['-p','ai_gateway:='+gateway,'-p','ai_model:=deepseek-flash','-p','ai_thinking:=false']
    print(f'启动程序，日志位于 {run}；退出按 Ctrl+C。',flush=True)
    os.execvpe('bash',command,env)

if __name__=='__main__':main()
