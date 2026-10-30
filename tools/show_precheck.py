#!/usr/bin/env python3
"""Read the contestant's saved log; display one matching action/receipt sample."""
import argparse,json,pathlib

ap=argparse.ArgumentParser(description='从本队真实日志提取一组感知—动作—回执，仅阅读，不发动作。')
ap.add_argument('log',type=pathlib.Path,help='本次运行生成的 decisions.jsonl')
args=ap.parse_args()
try:
    with args.log.open() as f:rows=[json.loads(line) for line in f if line.strip()]
except (OSError,ValueError) as e:raise SystemExit('日志无法读取：'+str(e))
previous={};sample=None
for row in rows:
    session=row.get('session_id');tick=row.get('tick');receipt=row.get('last_action')
    if isinstance(session,str) and type(tick)==int:
        if isinstance(receipt,dict) and type(receipt.get('tick'))==int:
            prior=previous.get((session,receipt['tick']))
            if prior and tick==prior['tick']+1 and isinstance(row.get('position'),list):
                action=prior['action']
                if receipt.get('action')=='move' and action.get('action')=='move' and receipt.get('moved') is True and not receipt.get('error'):
                    sample=(prior,row);break
        action=row.get('action')
        if isinstance(action,dict) and action.get('tick')==tick and action.get('session_id')==session and isinstance(row.get('position'),list):
            previous[(session,tick)]=row
if sample is None:raise SystemExit('尚未找到同场相邻Tick的成功移动及回执；请确认本队实际运行并检查原日志，不能把此状态算作通过。')
before,after=sample
def show(value):print(json.dumps(value,ensure_ascii=False,separators=(',',':')))
print('本队程序日志节选（不是独立话题抓包；未生成或修改比赛数据）')
print('来源：'+str(args.log.resolve()))
print('\n1. 收到的感知字段（position 对应 self.position）')
show({k:before[k] for k in ('session_id','tick','position')})
print('\n2. 本队程序发布的动作（原 action 字段）')
show(before['action'])
print('\n3. 下一Tick感知与裁判执行回执（last_action 对应 self.last_action）')
show({k:after[k] for k in ('session_id','tick','position','last_action')})
print('\n本组核对通过：同一场次；感知与动作Tick相同；下一帧确认上一Tick移动成功。')
print('此节选只核对一组记录；完整局、编译启动及正式联调另留证。')
