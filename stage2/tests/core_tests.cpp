#include "battle.hpp"
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool ok,const char *why)
{
	if(!ok) throw std::runtime_error(why);
}

battle::Frame frame(int tick=0,const std::string &session="match")
{
	battle::Frame f;
	f.session=session;
	f.team="A";
	f.tick=tick;
	f.width=f.height=9;
	f.radius=4;
	f.position={4,4};
	f.hp=100;
	f.missions=Json::Value(Json::arrayValue);
	f.completed=Json::Value(Json::arrayValue);
	return f;
}

void event(battle::Events &e,const std::string &text)
{
	std::string error;
	require(e.merge(text,9,9,error),error.c_str());
}
}

int main()
{
	try
	{
		// v1.1：同批次分摊、来源去重、先奖励后扣死亡；这里只检验决策预测。
		auto reward=battle::damage_reward(10,{{"A",20},{"C",20}},"A");
		require(reward.attack==5 && reward.kill==30,"overkill/shared reward wrong");
		reward=battle::damage_reward(30,{{"A",20},{"C",20,false}},"A");
		require(reward.attack==15 && reward.kill==30,"reflection must share lethal batch with attacks");
		reward=battle::damage_reward(30,{{"A",20},{"C",20,false}},"C");
		require(reward.attack==0 && reward.kill==30,"reflection incorrectly granted attack contribution");
		reward=battle::damage_reward(40,{{"A",20,false},{"A",30,false},{"C",10,false},{"D",0}},"A");
		require(reward.attack==0 && reward.kill==30,"pepper teams not deduplicated or zero hit rewarded");
		reward=battle::damage_reward(100,{{"A",50,false}},"A");
		require(reward.attack==0 && reward.kill==0,"nonlethal pepper granted damage points");
		reward=battle::damage_reward(0,{{"A",50,false}},"A");
		require(reward.attack==0 && reward.kill==0,"combat victim rewarded again by pepper");
		reward=battle::damage_reward(10,{{"C",20}},"A");
		require(reward.attack==0 && reward.kill==0,"previous-tick contributor received kill reward");
		require(battle::projected_damage(0,80,1)==20,"mutual kill must add rewards before death penalty");
		require(battle::projected_damage(35,0,1)==0 && battle::projected_damage(100,80,2)==60,
			"death floor or multiple deaths wrong");
		require(battle::projected_damage(495,80,0)==575 && battle::projected_damage(575,0,1)==515,
			"damage accumulation was capped at 500");
		require(battle::combat_points(300)==9 && battle::combat_points(500)==15 && battle::combat_points(575)==15,
			"v1.1 combat points threshold wrong");

		const std::string valid=
		    R"({"tick":0,"session_id":"schema","vision_radius":4,"map":{"width":9,"height":9},"self":{"team":"A","position":[4,4],"hp":100,"alive":true,"protected":false,"last_action":null},"visible_entities":[],"score":{"resource_value":0,"effective_damage":0,"mission_completed_ids":[],"invalid_actions":0},"missions":[],"event_text":""})";
		battle::Frame parsed;
		std::string parse_error;
		Json::Value malformed;
		require(battle::parse_frame(valid,"A",parsed,parse_error),"valid stage-two schema rejected");
		battle::read_json(valid,malformed);
		malformed["self"]["position_reason"]="respawn";
		malformed["self"]["position"][0]=2;
		require(battle::parse_frame(rmtc::compact_json(malformed),"A",parsed,parse_error) && parsed.respawned &&
			parsed.position==battle::Point{2,4},"actual fallback respawn position ignored");
		battle::read_json(valid,malformed);
		malformed["self"]["hp"]="100";
		require(!battle::parse_frame(rmtc::compact_json(malformed),"A",parsed,parse_error),"string hp accepted");
		battle::read_json(valid,malformed);
		malformed["event_text"]=Json::arrayValue;
		require(!battle::parse_frame(rmtc::compact_json(
			malformed),"A",parsed,parse_error),"structured event input accepted");

		battle::Bot bot;
		auto f=frame();
		f.enemies.push_back({"B",{4,2},100,false,false});
		f.entities.push_back({"obstacle","",{4,3},0});
		auto d=bot.update(f);
		require(d && d->action["action"]=="attack" && d->action["direction"]=="up","attack should cross wall");
		require(bot.update(f)->replayed,"duplicate must replay");
		f.tick=2;
		bot.update(f);
		f.tick=1;
		require(!bot.update(f),"old tick accepted");
		f=frame(0,"new");
		require(bot.update(f).has_value(),"new session rejected");
		require(!bot.update(frame(3)),"old session revived");
		std::string error;
		require(!bot.accept_events("match","{\"events\":[]}",error),"stale AI accepted");

		battle::Bot dead;
		f=frame();
		f.alive=false;
		require(dead.update(f)->action["action"]=="wait","dead must wait");

		// 无目标、末Tick和堡垒驻留都应保留防御，不能退化成WAIT。
		for(int tick:{0,499})
		{
			battle::Bot idle;
			require(idle.update(frame(tick))->action["action"]=="defend","idle robot must defend");
		}
		battle::Bot hold;
		hold.update(frame());
		require(hold.accept_events("match",
			R"({"events":[{"kind":"unrivaled","scope":"zone","start":1,"end":20,"rect":[4,4,4,4],"stay":3,"radius":2,"attack_damage":35,"defended_damage":7}]})",
			error),"fortress event rejected");
		d=hold.update(frame(1));
		require(d->action["action"]=="defend" && d->reason=="occupy fortress","fortress residence must defend");

		// 公开自对弈复现：双方20HP时，不应把敌人的防御和反击同时计入。
		battle::Bot stalemate;
		f=frame();
		f.hp=20;
		f.damage=240;
		f.enemies={{"B",{4,3},20,true,false}};
		d=stalemate.update(f);
		require(d->action["action"]=="attack" && d->action["direction"]=="up",
			"mutually exclusive defense and attack caused a permanent low-health stalemate");
		battle::Bot healthy_guard;
		f.enemies[0].hp=100;
		require(healthy_guard.update(f)->action["action"]=="defend",
			"defense prediction must retain the risk of a lethal counterattack");

		battle::Bot powerless;
		powerless.update(frame());
		require(powerless.accept_events("match",
			R"({"events":[{"kind":"unrivaled","scope":"effect","start":1,"end":20,"targets":["A"],"radius":2,"attack_damage":0,"defended_damage":0}]})",
			error),"zero-damage effect rejected");
		f=frame(1);
		f.enemies={{"B",{4,2},100,true,false}};
		f.entities={{"obstacle","u",{4,3},0},{"obstacle","r",{5,4},0},
			{"obstacle","d",{4,5},0},{"obstacle","l",{3,4},0}};
		require(powerless.update(f)->action["action"]=="defend","zero attack power must not replace defense");

		// 连续两个方向争格失败，第二次回执不能抹掉第一个受阻格。
		battle::Bot congested;
		f=frame();
		f.entities={{"resource_high","left",{3,4},20},{"resource_high","up",{4,3},20}};
		d=congested.update(f);
		require(d->action["action"]=="move","congestion fixture did not start moving");
		const auto first_blocked=d->target;
		f.tick=1;
		f.last_action["tick"]=0;
		f.last_action["error"]="E_BLOCKED";
		d=congested.update(f);
		require(d->action["action"]=="move" && !(d->target==first_blocked),"blocked target immediately retried");
		const auto second_blocked=d->target;
		f.tick=2;
		f.last_action["tick"]=1;
		d=congested.update(f);
		require(d->action["action"]!="move" || (!(d->target==first_blocked) && !(d->target==second_blocked)),
			"second collision erased the first blocked cell");
		f.tick=10;
		f.last_action=Json::nullValue;
		require(congested.update(f)->action["action"]=="move","temporary contention ban did not expire");

		// 争格冷却只影响近期入格；绕行时仍需保留稍后可达的高价值资源目标。
		for(int mirrored=0;mirrored<2;++mirrored)
		{
			battle::Bot detour;
			auto transform=[&](battle::Point p)
			{
				return mirrored?battle::Point{8-p.x,8-p.y}:p;
			};
			f=frame();
			const auto valuable=transform({4,3});
			f.entities={{"resource_high","near",valuable,20},
				{"resource_normal","far",transform({8,4}),10}};
			d=detour.update(f);
			require(d->action["action"]=="move" && d->target==valuable,"contention fixture missed near resource");
			f.tick=1;
			f.last_action["tick"]=0;
			f.last_action["error"]="E_BLOCKED";
			d=detour.update(f);
			require(d->target!=valuable,"contention cooldown allowed immediate repeated collision");
			f.tick=2;
			f.position=transform({5,4});
			f.last_action=Json::nullValue;
			d=detour.update(f);
			require(d->action["action"]=="move" &&
				std::abs(d->target.x-valuable.x)+std::abs(d->target.y-valuable.y)<2,
				"temporary contention removed a future resource goal from navigation");
		}

		for(int sign:{-1,1})
		{
			battle::Bot approach;
			f=frame();
			f.enemies={{"B",{4+sign,4+sign},100,false,false}};
			d=approach.update(f);
			require(d->action["action"]=="move" &&
				std::abs(d->target.x-4-sign)+std::abs(d->target.y-4-sign)==1,
				"healthy robot stalled beside a diagonal opponent below the damage target");
			battle::Bot fragile;
			f.hp=20;
			require(fragile.update(f)->action["action"]=="defend","pursuit ignored lethal next-tick exposure");
		}

		battle::Bot protected_bot;
		f=frame();
		f.protected_=true;
		f.enemies.push_back({"B",{4,3},100,false,false});
		require(protected_bot.update(f)->action["action"]!="attack","protected attacker attacked");

		battle::Bot shield;
		f=frame();
		f.enemies={{"B",{4,3},100,false,true},{"C",{4,2},100,false,false}};
		require(shield.update(f)->action["action"]!="attack","protected nearest target did not block shot");

		battle::Bot survive;
		f=frame();
		f.hp=15;
		f.enemies.push_back({"B",{4,3},100,false,false});
		require(survive.update(f)->action["action"]=="defend","movement cannot evade already in-range lethal attack");

		// 低血量日志中的逃跑在战斗结算前就阵亡，不能预支移动后的安全与采集收益。
		for(int mirrored=0;mirrored<2;++mirrored)
		{
			auto transform=[&](battle::Point p)
			{
				return mirrored?battle::Point{8-p.x,8-p.y}:p;
			};
			battle::Bot doomed_escape;
			f=frame();
			f.hp=10;
			f.damage=40;
			f.enemies={{"B",transform({4,3}),100,false,false}};
			d=doomed_escape.update(f);
			require(d->action["action"]=="attack" && d->action["direction"]==(mirrored?"down":"up"),
				"escape credited movement after predicted lethal combat");

			for(int hp:{15,20,21})
			{
				battle::Bot endangered_resource;
				f=frame();
				f.hp=hp;
				f.enemies={{"B",transform({4,3}),100,false,false}};
				const auto resource=transform({5,4});
				f.entities={{"resource_high","near",resource,20}};
				d=endangered_resource.update(f);
				if(hp<=20)
					require(d->action["action"]=="defend","resource reward paid before surviving combat");
				else
					require(d->action["action"]=="move" && d->target==resource,
						"survivable resource escape was incorrectly disabled");
			}
		}

		battle::Bot task;
		f=frame();
		Json::Value m;
		m["id"]="go";
		m["type"]="reach";
		m["position"].append(5);
		m["position"].append(4);
		f.missions.append(m);
		require(task.update(f)->action["direction"]=="right","reach mission not pursued");
		f.tick=500;
		require(!task.update(f),"final tick action emitted");

		// 两个等距资源之间，优先兼顾采集和新增视野；镜像后仍成立。
		for(int mirrored=0;mirrored<2;++mirrored)
		{
			battle::Bot gather;
			const std::vector<battle::Point> path={{16,1},{15,1},{15,2},{14,2},{13,2},{13,3},{13,4},{12,4},{12,5}};
			auto transform=[&](battle::Point p)
			{
				return mirrored?battle::Point{17-p.x,17-p.y}:p;
			};
			for(int t=0;t<int(path.size());++t)
			{
				f=frame(t);
				f.width=f.height=18;
				f.position=transform(path[t]);
				f.resource_value=40;
				if(t==8) f.entities={{"resource_normal","a",transform({8,4}),10},
						{"resource_normal","b",transform({13,9}),10}};
				d=gather.update(f);
			}
			require(d->action["action"]=="move" && d->reason=="collect resource" &&
				d->target==transform({12,6}),"speculative exploration displaced a known resource route");
		}

		// 附近没有资源不代表已经采完：未完成到达任务时，不要掉头追逐尚未交战的敌车。
		for(int mirrored=0;mirrored<2;++mirrored)
		{
			battle::Bot productive;
			f=frame();
			f.width=f.height=24;
			auto transform=[&](battle::Point p)
			{
				return mirrored?battle::Point{23-p.x,23-p.y}:p;
			};
			f.position=transform({10,18});
			f.resource_value=30;
			f.enemies={{"B",transform({8,21}),100,false,false}};
			Json::Value mission;
			mission["id"]="unfinished";
			mission["type"]="reach";
			const auto goal=transform({12,12});
			mission["position"].append(goal.x);
			mission["position"].append(goal.y);
			f.missions.append(mission);
			d=productive.update(f);
			require(d->action["action"]=="move" && d->reason!="approach observed enemy" &&
				std::abs(d->target.x-goal.x)+std::abs(d->target.y-goal.y)<8,
				"unfinished productive goal displaced by unnecessary enemy pursuit");
		}

		battle::Bot close_distance;
		f=frame();
		f.position={6,4};
		f.enemies={{"B",{4,3},100,false,false}};
		d=close_distance.update(f);
		require(d->action["action"]=="move" && d->target.x<6,
			"pursuit must close distance instead of oscillating along the outer firing range");

		battle::Bot threshold;
		f=frame();
		f.damage=350;
		f.enemies={{"B",{4,2},100,false,false}};
		require(threshold.update(f)->action["action"]=="attack",
			"damage reward incorrectly stopped at old 300 threshold");
		f.tick=1;
		f.damage=500;
		d=threshold.update(f);
		require(d->action["action"]=="move" && d->target.x!=4,"capped damage should favor leaving needless exchange");
		f.tick=2;
		f.damage=440;
		require(threshold.update(
			f)->action["action"]=="attack","official death deduction should reopen damage reward opportunity");

		battle::Bot retreat;
		f=frame();
		f.hp=30;
		f.damage=100;
		f.enemies={{"B",{4,2},100,false,false}};
		d=retreat.update(f);
		require(d->action["action"]=="move" && d->target.x!=4,
			"low-health robot should escape while it can survive this tick");

		battle::Bot trade;
		f=frame();
		f.hp=15;
		f.damage=100;
		f.enemies={{"B",{4,3},10,false,false}};
		require(trade.update(f)->action["action"]=="defend",
			"kill reward must not erase simultaneous lethal return fire");
		f.tick=499;
		require(trade.update(f)->action["action"]=="attack",
			"last-tick positive trade must not be penalized for nonexistent future downtime");

		battle::Bot late_combat;
		f=frame(300);
		f.damage=700;
		f.enemies={{"B",{4,2},100,false,false}};
		require(late_combat.update(f)->action["action"]=="attack",
			"late-game tie-break damage should remain valuable above the points cap");

		battle::Bot finish;
		f=frame();
		f.damage=100;
		f.enemies={{"B",{4,2},100,false,false},{"C",{6,4},10,false,false}};
		d=finish.update(f);
		require(d->action["action"]=="attack" && d->action["direction"]=="right",
			"safe finishing shot should beat farming a healthy target");

		battle::Bot share;
		f.enemies[0].hp=10;
		f.enemies.push_back({"D",{5,2},100,false,false});
		d=share.update(f);
		require(d->action["action"]=="attack" && d->action["direction"]=="right",
			"contested kill should not receive the full exclusive bonus");
		battle::read_json(valid,malformed);
		malformed["tick"]=1;
		malformed["score"]["effective_damage"]=35;
		malformed["self"]["alive"]=false;
		malformed["self"]["hp"]=0;
		require(battle::parse_frame(rmtc::compact_json(
			malformed),"A",parsed,parse_error) && parsed.damage==35,
			"official adjusted damage must not be deducted again");

		battle::Bot authoritative;
		f=frame(0,"schema");
		f.damage=95;
		authoritative.update(f);
		require(authoritative.update(parsed)->action["action"]=="wait","reduced official score rejected after death");
		malformed["tick"]=2;
		malformed["score"]["effective_damage"]=0;
		require(battle::parse_frame(rmtc::compact_json(
			malformed),"A",parsed,parse_error) && parsed.damage==0,"zero damage floor rejected");
		malformed["score"]["effective_damage"]=575.125;
		malformed["score"]["mission_completed_ids"].append("damage_done");
		require(battle::parse_frame(rmtc::compact_json(malformed),"A",parsed,parse_error) && parsed.damage==575.125,
			"fractional damage above 500 rejected");
		malformed["score"]["effective_damage"]=20;
		require(battle::parse_frame(rmtc::compact_json(malformed),"A",parsed,parse_error) &&
			parsed.completed[0]=="damage_done","damage decrease revoked completed mission");

		battle::Bot assist;
		f=frame();
		f.enemies={{"B",{4,2},100,false,false},{"C",{6,4},30,false,false},{"D",{6,3},100,false,false}};
		d=assist.update(f);
		require(d->action["action"]=="attack" && d->action["direction"]=="right",
			"joint lethal damage should earn shared kill reward without a solo lethal shot");

		battle::Events events;
		event(events,
			R"({"events":[{"kind":"explosion","scope":"zone","start":10,"end":11,"rect":[3,3,5,5],"damage":80,"respects_protection":true}]})");
		require(events.hazard({4,4},9,false)==0 && events.hazard({4,4},10,false)==80 && events.hazard({4,4},11,
			false)==0,
			"half-open activation failed");
		require(events.hazard({4,4},10,true)==0,"explosion protection ignored");
		require(!events.merge(
			R"({"events":[{"kind":"world_split","scope":"zone","start":4,"end":9,"points":[[99,1]]}]})",9,9,
			error),"out-of-bounds event accepted");
		require(events.items.size()==1,"invalid event changed state");
		require(!events.merge(R"({"events":[{"kind":"illusion","scope":"zone","start":4,"end":9,"rect":[0,0,3,3]}]})",9,
			9,error),"missing event parameter guessed");
		require(!events.merge(R"({"events":[],"events":[]})",9,9,error),"duplicate model key accepted");
		event(events,
			R"({"events":[{"kind":"illusion","scope":"zone","start":10,"end":11,"rect":[3,3,5,5],"effect_ticks":12}]})");
		std::map<int,battle::Frame> history;
		history[10]=frame(10);
		history[11]=frame(11);
		history[11].alive=false;
		require(events.effects("A",11,history,true).inverted,"illusion not retained across death");
		require(!events.effects("A",22,history,true).inverted,"illusion did not expire");
		event(events,R"({"events":[{"kind":"world_split","scope":"zone","start":20,"end":30,"points":[[2,2]]}]})");
		require(events.wall({2,8},20) && !events.wall({2,8},30) && events.temporary_seen({2,8},25,
			30),
			"temporary wall lifetime wrong");
		event(events,
			R"({"events":[{"kind":"angel","scope":"zone","start":10,"end":20,"points":[[4,4]],"effect_ticks":5,"heal":5,"hp_cap":100}]})");
		require(events.effects("A",11,history,true).heal==0,"standing on angel point incorrectly granted heal");
		history[11].last_action["moved"]=true;
		history[11].last_action["tick"]=10;
		require(events.effects("A",11,history,true).heal==5 && events.effects("A",15,history,
			true).heal==0,"angel entry/expiry wrong");
		event(events,
			R"({"events":[{"kind":"pan","scope":"zone","start":10,"end":20,"window_end":15,"multiplier":2},{"kind":"purity","scope":"zone","start":10,"end":20,"reflection":12},{"kind":"pepper","scope":"zone","start":10,"end":20,"points":[[2,3]],"damage":30,"respects_protection":true}]})");
		require(events.effects("A",10,history,true).multiplier==1 && events.effects("A",11,history,
			true).multiplier==2,
			"pan applied before next combat");
		require(events.reflection(19)==12 && events.reflection(20)==0,"reflection lifetime wrong");
		battle::Events fortress;
		event(fortress,
			R"({"events":[{"kind":"unrivaled","scope":"zone","start":10,"end":20,"rect":[3,3,5,5],"stay":3,"radius":2,"attack_damage":35,"defended_damage":7}]})");
		history.clear();
		history[11]=frame(11);
		history[12]=frame(12);
		require(fortress.effects("A",12,history,true).radius==0,"fortress granted too early");
		history[13]=frame(13);
		require(fortress.effects("A",13,history,true).radius==2,"fortress did not count completed ticks");
		history[14]=frame(14);
		history[14].alive=false;
		require(fortress.effects("A",14,history,true).radius==2 && fortress.effects("A",20,history,
			true).radius==0,
			"fortress death/expiry wrong");
		history.erase(12);
		require(fortress.effects("A",13,history,true).radius==0,"missing tick invented uninterrupted residence");

		// 普通复活在本Tick驻留结算之后，复活首帧不能虚增一次驻留。
		history.clear();
		for(int t=10;t<=14;++t)
			history[t]=frame(t);
		history[11].alive=false;
		require(fortress.effects("A",14,history,true).radius==0,"ordinary respawn counted as fortress residence");
		history[15]=frame(15);
		require(fortress.effects("A",15,history,true).radius==2,"post-respawn fortress residence lost");
		history[16]=frame(16);
		history[16].respawned=true;
		require(fortress.effects("A",16,history,true).radius==2,"earned fortress buff lost across respawn");
		history.clear();
		for(int t=10;t<=14;++t)
			history[t]=frame(t);
		history[12].respawned=true;
		require(fortress.effects("A",13,history,true).radius==0 && fortress.effects("A",14,history,true).radius==2,
			"instant respawn did not reset unfinished residence");
		battle::Events offset;
		event(offset,
			R"({"events":[{"kind":"unrivaled","scope":"zone","start":10,"end":20,"rect":[1,2,3,4],"stay":3,"radius":2,"attack_damage":50,"defended_damage":10}]})");
		history.clear();
		for(int t=10;t<=13;++t)
			history[t]=frame(t);
		require(offset.effects("A",13,history,true).radius==0,"fortress defaulted to map centre");
		for(auto &[t,h]:history)
			h.position={1,2};
		require(offset.effects("A",13,history,true).attack==50 && offset.effects("A",20,history,true).radius==0,
			"announced fortress boundary or expiry ignored");

		battle::Bot aoe;
		f=frame();
		aoe.update(f);
		require(aoe.accept_events(f.session,
			R"({"events":[{"kind":"unrivaled","scope":"effect","start":1,"end":20,"targets":["A"],"radius":2,"attack_damage":35,"defended_damage":7}]})",
			error),"valid AoE rejected");
		f.tick=1;
		f.enemies.push_back({"B",{5,5},100,false,false});
		require(aoe.update(f)->action["action"]=="attack","AoE diagonal target ignored");
		f.tick=2;
		f.damage=700;
		f.enemies[0].hp=10;
		require(aoe.update(f)->action["action"]=="attack","safe kill above 500 lost tie-break value");

		battle::Bot reflect;
		f=frame();
		reflect.update(f);
		require(reflect.accept_events(f.session,
			R"({"events":[{"kind":"purity","scope":"zone","start":1,"end":10,"reflection":20}]})",
			error),"valid purity rejected");
		f.tick=1;
		f.hp=15;
		f.enemies={{"B",{4,3},20,false,false}};
		d=reflect.update(f);
		require(d->action["action"]=="defend" && d->utility>0,
			"purity defense should negate attack damage and value reflected kill");

		const std::string pepper_text=
		    R"({"events":[{"kind":"pepper","scope":"zone","start":1,"end":10,"points":[[5,4],[5,2]],"damage":50,"respects_protection":true}]})";
		battle::Bot pepper;
		f=frame();
		pepper.update(f);
		require(pepper.accept_events(f.session,pepper_text,error),"valid pepper rejected");
		f.tick=1;
		f.enemies={{"B",{5,8},50,false,false}};
		d=pepper.update(f);
		require(d->action["action"]=="move" && d->target==battle::Point{5,4},"safe pepper kill not valued");
		f.tick=2;
		f.hp=40;
		f.damage=100;
		f.enemies.push_back({"C",{6,2},100,false,false});
		d=pepper.update(f);
		require(d->action["action"]!="move" || d->target!=battle::Point{5,4},
			"own pepper center incorrectly immune to another line's lethal fire");

		battle::Bot reverse;
		f=frame(10);
		reverse.update(f);
		require(reverse.accept_events(f.session,
			R"({"events":[{"kind":"illusion","scope":"zone","start":10,"end":11,"rect":[3,3,5,5],"effect_ticks":12}]})",
			error),"valid illusion rejected");
		f.tick=11;
		f.entities.push_back({"resource_high","r",{5,4},20});
		require(reverse.update(f)->action["direction"]=="left","illusion movement not compensated");

		battle::Bot escape;
		f=frame(10);
		escape.update(f);
		require(escape.accept_events(f.session,
			R"({"events":[{"kind":"explosion","scope":"zone","start":12,"end":13,"rect":[4,4,5,5],"damage":80,"respects_protection":true}]})",
			error),"valid explosion rejected");
		f.tick=11;
		const auto flee=escape.update(f);
		require(flee->action["action"]=="move" && (flee->target.x<4 || flee->target.y<4),
			"announced explosion did not change route");
		std::cout<<
		    "PASS: v1.1 rewards/deaths, combat before movement, reflection, pepper, missions, sessions and event boundaries\n";
	}
	catch(const std::exception &e)
	{
		std::cerr<<"FAIL: "<<e.what()<<'\n';
		return 1;
	}
}
