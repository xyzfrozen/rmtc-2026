#!/usr/bin/env python3
"""Offline test operator. Only this harness reads practice truth/control APIs.

The contestant process receives public ROS perceptions only; no map is passed to it.
"""
import argparse
import copy
import json
import os
from pathlib import Path
import random
import signal
import socket
import subprocess
import time
import urllib.request

ROOT=Path(__file__).resolve().parents[1]

def stop(process):
    if process is None or process.poll() is not None:
        return
    os.killpg(process.pid,signal.SIGINT)
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid,signal.SIGKILL)
        process.wait(timeout=3)

def randomized(template,seed):
    config=copy.deepcopy(template)
    rng=random.Random(seed)
    width=height=20
    spawn=(1,1)
    cells=[(x,y) for y in range(height) for x in range(width) if (x,y)!=spawn]
    rng.shuffle(cells)
    obstacles=set(cells[:30])
    reachable={spawn}
    queue=[spawn]
    for x,y in queue:
        for p in [(x+1,y),(x-1,y),(x,y+1),(x,y-1)]:
            if 0<=p[0]<width and 0<=p[1]<height and p not in obstacles and p not in reachable:
                reachable.add(p)
                queue.append(p)
    sites=sorted(reachable-{spawn})
    rng.shuffle(sites)
    config['map']={'width':width,'height':height,'obstacles':[list(p) for p in sorted(obstacles)],
        'resources':[{'id':f'random-{seed}-{i}','position':list(p),'value':10 if i<12 else 20} for i,p in enumerate(sites[:16])]}
    config['robots']=[{'team':'A','spawn':list(spawn)}]
    return config

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--practice-dir',type=Path,required=True)
    parser.add_argument('--output',type=Path,default=ROOT/'test-results'/'official')
    parser.add_argument('--port',type=int,default=18080)
    parser.add_argument('--domain',type=int,default=87)
    parser.add_argument('--cases',default='default,reset_pause,restart,rotated,random_7,random_19')
    args=parser.parse_args()
    if not 0<=args.domain<=231:
        parser.error('domain must be 0..231 (domain+1 is used internally)')
    practice=args.practice_dir.resolve()
    if not (practice/'start_referee.sh').is_file():
        parser.error('practice-dir must be the unpacked official package')
    if not (ROOT/'build'/'collector_node').is_file():
        parser.error('Run build.sh first')
    # Refuse to control an unrelated service already using this port.
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',args.port))
    output=args.output.resolve()
    output.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,ROS_DOMAIN_ID=str(args.domain),RMTC_INTERNAL_DOMAIN_ID=str(args.domain+1),
        ROS_LOCALHOST_ONLY='1',RMW_IMPLEMENTATION='rmw_fastrtps_cpp',ROS_LOG_DIR=str(output/'ros-logs'))
    for k in ['PRACTICE_LLM_KEY','PRACTICE_LLM_URL','PRACTICE_MODEL','RMTC_TEAM_TOKEN']:
        env.pop(k,None)
    base=f'http://127.0.0.1:{args.port}'
    def api(path,body=None):
        data=None if body is None else json.dumps(body).encode()
        request=urllib.request.Request(base+path,data=data,headers={'Content-Type':'application/json'})
        with urllib.request.urlopen(request,timeout=20) as response:
            return json.load(response)
    def wait_for(predicate,timeout=25):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            if referee.poll() is not None:
                raise RuntimeError('Official referee exited; inspect referee.log')
            try:
                state=api('/api/state')
            except (OSError,ValueError):
                time.sleep(.1)
                continue
            if state.get('fault'):
                raise RuntimeError(state['fault'])
            if predicate(state):
                return state
            time.sleep(.1)
        raise TimeoutError('Practice state deadline exceeded')
    logs=[]
    referee_log=(output/'referee.log').open('w');logs.append(referee_log)
    referee=subprocess.Popen(['bash',str(practice/'start_referee.sh'),'--stage','1','--port',str(args.port),
        '--data-dir',str(output/'referee'),'--ai-mode','mock'],cwd=practice,env=env,
        stdout=referee_log,stderr=subprocess.STDOUT,start_new_session=True)
    bot=None
    def launch_bot():
        stream=(output/'collector.log').open('a');logs.append(stream)
        return subprocess.Popen(['bash',str(ROOT/'run.sh'),'--ros-args','-p','team:=A','-p','team_name:=Astra采集',
            '-p',f'record_path:={output / "decisions.jsonl"}'],cwd=ROOT,env=env,
            stdout=stream,stderr=subprocess.STDOUT,start_new_session=True)
    results=[]
    try:
        wait_for(lambda s:s.get('status',{}).get('ready') and not s.get('starting'))
        template=json.loads((practice/'config'/'stage1.json').read_text())
        bot=launch_bot()
        time.sleep(2)
        for case in args.cases.split(','):
            if case=='default' and not results:
                pass
            elif case=='reset_pause':
                api('/api/reset',{})
            else:
                config=copy.deepcopy(template)
                if case=='rotated':
                    w,h=config['map']['width'],config['map']['height']
                    rotate=lambda p:[w-1-p[0],h-1-p[1]]
                    config['map']['obstacles']=[rotate(p) for p in config['map']['obstacles']]
                    for r in config['map']['resources']:
                        r['position']=rotate(r['position'])
                    config['robots'][0]['spawn']=rotate(config['robots'][0]['spawn'])
                elif case.startswith('random_'):
                    config=randomized(template,int(case.split('_')[1]))
                elif case not in ['restart','default']:
                    raise ValueError('Unknown case: '+case)
                api('/api/start',{'stage':1,'tick_ms':100,'max_ticks':100,'bots':[],'config':config})
            state=wait_for(lambda s:s.get('status',{}).get('ready') and not s.get('starting'))
            session=state['session_id']
            time.sleep(.4)
            api('/api/resume',{})
            pause_tick=None;restart_tick=None
            if case=='reset_pause':
                wait_for(lambda s:s.get('status',{}).get('tick',0)>=25)
                api('/api/pause',{})
                state=wait_for(lambda s:s.get('status',{}).get('paused'))
                pause_tick=state['status']['tick']
                time.sleep(.5)
                assert api('/api/state')['status']['tick']==pause_tick,'Tick advanced while paused'
                api('/api/resume',{})
            if case=='restart':
                state=wait_for(lambda s:s.get('status',{}).get('tick',0)>=35)
                restart_tick=state['status']['tick']
                stop(bot)
                bot=launch_bot()
            state=wait_for(lambda s:s.get('board',{}).get('tick',0)>=100,timeout=30)
            time.sleep(.25)
            state=api('/api/state')
            if bot.poll() is not None:
                raise RuntimeError('Collector exited unexpectedly')
            run=Path(state['run_dir'])
            communication=[json.loads(line) for line in (run/'communication.jsonl').read_text().splitlines()]
            records=[json.loads(line) for line in (output/'decisions.jsonl').read_text().splitlines()]
            records=[r for r in records if r['session_id']==session]
            decisions=[r for r in records if 'action' in r]
            actions=[r['data'] for r in communication if r['kind']=='action' and r['seat']=='A']
            perceptions=[r['data'] for r in communication if r['kind']=='perception' and r['seat']=='A']
            receipts={p['self']['last_action']['tick']:p['self']['last_action'] for p in perceptions if isinstance(p.get('self',{}).get('last_action'),dict)}
            row=next(r for r in state['board']['teams'] if r['team']=='A')
            submitted={a['tick'] for a in actions}
            durations=sorted(r['decision_us'] for r in decisions if not r['replayed'])
            result={'case':case,'session_id':session,'tick_ms':100,'final_tick':state['board']['tick'],
                'resource_value':row['resource_value'],'score':row['score'],'invalid_actions':row['invalid_actions'],
                'submitted_ticks':len(submitted),'missing_action_ticks':sorted(set(range(100))-submitted),
                'successful_moves':sum(r.get('moved') is True for r in receipts.values()),
                'failed_moves':sum(r.get('error') in ['E_OOB','E_BLOCKED'] for r in receipts.values()),
                'last_executed_tick':state['checks']['A'].get('executed_tick'),
                'decision_us_max':max(durations,default=0),'decision_us_p95':durations[int(.95*(len(durations)-1))] if durations else 0,
                'pause_tick':pause_tick,'restart_tick':restart_tick,'ai_calls':state['ai'],
                'run_dir':str(run)}
            assert row['invalid_actions']==0,result
            assert row['resource_value']>0 and result['successful_moves']>0,result
            assert all(a['session_id']==session and 0<=a['tick']<100 and a['action'] in ['move','wait'] for a in actions),result
            assert all(r.get('diagnostic')!='session_mismatch' for r in communication),result
            assert result['last_executed_tick']==99,result
            assert not state['ai'],'Collector unexpectedly called an AI gateway'
            if case!='restart':
                assert len(submitted)==100,result
            else:
                assert any(r['tick']>restart_tick+5 for r in decisions),result
            if case=='reset_pause':
                assert any(r['tick']==pause_tick and r['replayed'] for r in decisions),'No cached resend at resume'
            result['passed']=True
            results.append(result)
            (output/'summary.json').write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n')
            print(json.dumps(result,ensure_ascii=False),flush=True)
    finally:
        stop(bot)
        try:
            api('/api/stop',{})
        except Exception:
            pass
        stop(referee)
        for stream in logs:
            stream.close()

if __name__=='__main__':
    main()
