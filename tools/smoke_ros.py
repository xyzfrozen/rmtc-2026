#!/usr/bin/env python3
"""Synthetic local ROS/HTTP smoke test, not an official match or semantic test."""
import argparse,datetime,json,os,pathlib,pty,select,signal,subprocess,threading,time
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import rclpy
from rclpy.qos import QoSProfile,ReliabilityPolicy,DurabilityPolicy
from std_msgs.msg import String

ROOT=pathlib.Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser(description='两阶段真实节点的本机通信自检；合成感知、本地模拟HTTP、不调用付费模型。')
ap.add_argument('--domain',type=int,default=85,help='空闲测试ROS域；所有同域进程可能收到合成感知')
args=ap.parse_args()
if not 0<=args.domain<=231:ap.error('domain must be 0..231')
os.environ.update(ROS_DOMAIN_ID=str(args.domain),ROS_LOCALHOST_ONLY='1',RMW_IMPLEMENTATION='rmw_fastrtps_cpp')
for key in ('FASTRTPS_DEFAULT_PROFILES_FILE','FASTDDS_DEFAULT_PROFILES_FILE','ROS_DISCOVERY_SERVER','RMW_FASTRTPS_USE_QOS_FROM_XML'):
    os.environ.pop(key,None)
out=ROOT/'logs'/('migration-smoke-'+datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f'));out.mkdir(parents=True)
os.environ['ROS_LOG_DIR']=str(out/'ros')
token='migration-smoke-only';calls=[];results=[];process=None;master=None;node=None;captured=b''

class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,value):
        data=json.dumps(value).encode();self.send_response(200)
        self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(data)))
        self.end_headers();self.wfile.write(data)
    def do_GET(self):self.reply({'ok':True})
    def do_POST(self):
        payload=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        calls.append({'path':self.path,'token_matches':self.headers.get('Authorization')=='Bearer '+token,'body':payload})
        self.reply({'choices':[{'finish_reason':'stop','message':{'content':'{"events":[]}'}}]})

server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
gateway=f'http://127.0.0.1:{server.server_port}/v1/chat/completions'
config=out/'match.json';config.write_text(json.dumps({'gateway_url':gateway,'network_interface':'lo'}))

def stop():
    global process,master
    if process is not None and process.poll() is None:
        os.killpg(process.pid,signal.SIGINT)
        try:process.wait(timeout=6)
        except subprocess.TimeoutExpired:os.killpg(process.pid,signal.SIGKILL);process.wait()
    process=None
    if master is not None:os.close(master);master=None

def expect(needle):
    global captured
    end=time.monotonic()+10
    while needle not in captured:
        if process.poll() is not None or time.monotonic()>end:
            raise RuntimeError('启动超时或失败：'+captured.decode(errors='replace'))
        if select.select([master],[],[],.05)[0]:captured+=os.read(master,65536)

try:
    print('合成通信自检，不运行官方比赛、不使用真实密钥。测试域='+str(args.domain),flush=True)
    for stage,team in [('stage1','A'),('stage2','B')]:
        captured=b'';before_calls=len(calls)
        command=['bash',str(ROOT/'start.sh'),stage,'--local','--ros-domain-id',str(args.domain),
            '--team',team,'--team-name','迁移自检','--config',str(config),'--gateway-url',gateway]
        check=subprocess.run(command+['--check'],capture_output=True,text=True,timeout=10)
        assert check.returncode==0,check.stdout+check.stderr
        master,slave=pty.openpty()
        process=subprocess.Popen(command,stdin=slave,stdout=slave,stderr=slave,start_new_session=True)
        os.close(slave)
        if stage=='stage2':expect('不存盘'.encode());os.write(master,(token+'\n').encode())
        expect(b'Ready:')
        rclpy.init();node=rclpy.create_node('rmtc_migration_probe')
        qos=QoSProfile(depth=20,reliability=ReliabilityPolicy.RELIABLE,durability=DurabilityPolicy.VOLATILE)
        regqos=QoSProfile(depth=1,reliability=ReliabilityPolicy.RELIABLE,durability=DurabilityPolicy.TRANSIENT_LOCAL)
        actions=[];registrations=[]
        pub=node.create_publisher(String,f'/team_{team}/perception',qos)
        node.create_subscription(String,f'/team_{team}/action',lambda m:actions.append(json.loads(m.data)),qos)
        node.create_subscription(String,f'/team_{team}/registration',lambda m:registrations.append(json.loads(m.data)),regqos)
        def spin_until(predicate,seconds):
            end=time.monotonic()+seconds
            while not predicate() and time.monotonic()<end:
                assert process.poll() is None,'选手进程意外退出'
                rclpy.spin_once(node,timeout_sec=.02)
            assert predicate(),'ROS发现或动作接收超时'
        spin_until(lambda:bool(registrations) and pub.get_subscription_count()>0,10)
        assert registrations[-1]=={'team':team,'name':'迁移自检'}
        session='synthetic-migration-'+stage
        for tick in range(20):
            frame={'tick':tick,'session_id':session,'vision_radius':4,'map':{'width':9,'height':9},
                'self':{'team':team,'position':[4,4],'hp':100,'alive':True,'protected':False,'last_action':None},
                'visible_entities':[],'score':{'resource_value':0,'effective_damage':0,'mission_completed_ids':[],'invalid_actions':0},
                'missions':[],'event_text':''}
            pub.publish(String(data=json.dumps(frame)))
            spin_until(lambda:any(a.get('tick')==tick and a.get('session_id')==session for a in actions),2)
        valid=('move','wait') if stage=='stage1' else ('move','attack','defend')
        assert len(actions)==20 and all(a['action'] in valid and a['session_id']==session for a in actions)
        if stage=='stage2':
            expect(b'AI interpretation accepted')
            assert len(calls)>before_calls
        else:assert len(calls)==before_calls
        assert token.encode() not in captured,'测试令牌回显'
        (out/(stage+'.log')).write_bytes(captured)
        results.append({'stage':stage,'team':team,'actions':len(actions),'passed':True})
        node.destroy_node();node=None;rclpy.shutdown();stop()
        print(stage+': 注册、感知、20个动作通过',flush=True)
    assert calls and all(c['path']=='/v1/chat/completions' and c['token_matches'] and
        c['body']['model']=='deepseek-flash' and c['body']['max_tokens']==512 and
        c['body']['thinking']=={'type':'disabled'} for c in calls)
    (out/'result.json').write_text(json.dumps({'synthetic':True,'official_referee':False,'domain':args.domain,
        'results':results,'http_calls':len(calls),'paid_calls':0},ensure_ascii=False,indent=2)+'\n')
    print('通过；结果：'+str(out/'result.json'),flush=True)
finally:
    stop()
    if node is not None:node.destroy_node()
    if rclpy.ok():rclpy.shutdown()
    server.shutdown();server.server_close()
