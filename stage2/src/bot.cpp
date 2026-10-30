#include "battle.hpp"
#include <algorithm>
#include <cmath>
#include <queue>
#include <stdexcept>

namespace battle
{
namespace
{

int distance(Point a,Point b)
{
	return std::abs(a.x-b.x)+std::abs(a.y-b.y);
}

bool hits(Point a,Point b,const Effects &fx)
{
	if(fx.radius) return std::max(std::abs(a.x-b.x),std::abs(a.y-b.y))<=fx.radius;
	return (a.x==b.x || a.y==b.y) && distance(a,b)>0 && distance(a,b)<=2;
}

// 敌人动作未知，分别估计其他贡献者出手/不出手，避免把潜在合击当成必杀。
Reward expected_reward(double hp,const Hit &own,const std::vector<Hit> &possible)
{
	Reward out;
	const int cases=1<<possible.size();

	for(int mask=0;mask<cases;++mask)
	{
		std::vector<Hit> batch{own};
		for(std::size_t i=0;i<possible.size();++i)
			if(mask&(1<<i)) batch.push_back(possible[i]);
		const auto reward=damage_reward(hp,batch,own.team);
		out.attack+=reward.attack/cases;
		out.kill+=reward.kill/cases;
	}
	return out;
}
}

void Bot::observe(const Frame &f)
{
	auto visible=[&](Point p)
	{
		return std::max(std::abs(p.x-f.position.x),std::abs(p.y-f.position.y))<=f.radius;
	};

	if(f.alive)
	{
		for(auto it=resources_.begin();it!=resources_.end();)
		{
			if(visible(it->second.position)) it=resources_.erase(it);
			else
				++it;
		}

		for(auto it=enemies_.begin();it!=enemies_.end();)
		{
			if(visible(it->second.robot.position) || f.tick-it->second.tick>40) it=enemies_.erase(it);
			else
				++it;
		}

		for(int y=std::max(0,f.position.y-f.radius);y<=std::min(height_-1,f.position.y+f.radius);++y)
			for(int x=std::max(0,f.position.x-f.radius);x<=std::min(width_-1,f.position.x+f.radius);++x)
				cells_[index({x,y})]={1,int(f.tick)};

		for(const auto &e:f.entities)
		{
			if(e.type=="obstacle") cells_[index(e.position)]={2,int(f.tick)};
			if(e.type=="resource_normal" || e.type=="resource_high") resources_[e.id]=e;
		}

		for(const auto &e:f.enemies)
			enemies_[e.team]={e,int(f.tick)};
	}
	if(last_tick_>=0 && cached_.action["action"]=="move" && f.last_action["error"]=="E_BLOCKED" &&
	    integer(f.last_action["tick"],last_tick_,last_tick_))
	{
		// 分别记住近期受阻格，并错开重试时间，避免两个方向交替争格。
		blocked_until_[index(cached_.target)]=f.tick+3+(f.team[0]-'A'+f.tick)%4;
	}
	history_[f.tick]=f;
}

bool Bot::accept_events(const std::string &session,const std::string &text,std::string &error)
{
	if(session!=session_)
	{
		error="stale session";
		return false;
	}
	return events_.merge(text,width_,height_,error);
}

std::optional<Decision> Bot::update(const Frame &f)
{
	if(f.session!=session_)
	{
		if(std::find(retired_.begin(),retired_.end(),f.session)!=retired_.end()) return std::nullopt;
		if(!session_.empty()) retired_.push_back(session_);
		if(retired_.size()>64) retired_.pop_front();
		session_=f.session;
		width_=f.width;
		height_=f.height;
		last_tick_=-1;
		spawn_=f.tick==0?f.position:Point{-1,-1};
		cells_.assign(width_*height_,{});
		blocked_until_.assign(width_*height_,0);
		resources_.clear();
		enemies_.clear();
		history_.clear();
		events_.items.clear();
		resource_goal_.clear();
		goal_since_=-1;
	}
	if(f.width!=width_ || f.height!=height_ || f.tick<last_tick_ || f.tick>=total_) return std::nullopt;
	if(f.tick==last_tick_)
	{
		auto result=cached_;
		result.replayed=true;
		return result;
	}
	observe(f);
	cached_=plan(f);
	if(cached_.resource_goal!=resource_goal_ || f.tick-goal_since_>=12)
	{
		resource_goal_=cached_.resource_goal;
		goal_since_=f.tick;
	}
	last_tick_=f.tick;
	return cached_;
}

Decision Bot::plan(const Frame &f) const
{
	Decision out;
	out.action["tick"]=Json::Int64(f.tick);
	out.action["session_id"]=f.session;
	out.action["action"]="wait";
	out.target=f.position;
	out.reason="wait";
	out.event_count=events_.items.size();
	out.known_resources=resources_.size();
	out.observed_cells=std::count_if(cells_.begin(),cells_.end(),[](auto c)
	{
		return c.kind!=0;
	});
	if(!f.alive)
	{
		out.reason="respawning";
		return out;
	}
	const int tick=f.tick,remaining=total_-tick;
	// 资源争夺告一段落后，同分排名仍需要伤害积累；最后一Tick不再保留未来血量价值。
	const double rank_weight=tick>=total_/2 && resources_.empty()?.03:.004;
	const double lost_time=2.2*std::min(1.0,(remaining-1)/3.0);
	auto damage_value=[&](double damage)
	{
		return combat_points(damage)+rank_weight*damage;
	};
	const auto own=events_.effects(f.team,tick,history_,true);
	const double hp=f.hp-events_.hazard(f.position,tick,f.protected_);
	if(hp<=0)
	{
		out.reason="lethal activation before action";
		return out;
	}

	auto split_wall=[&](Point p,int at)
	{
		return p!=spawn_ && events_.wall(p,at);
	};

	for(const auto &e:events_.items)
		if(e.kind=="world_split" && e.start==tick && split_wall(f.position,tick))
		{
			out.reason="forced respawn before action";
			return out;
		}
	// 能正常行动时，原地停留统一防御；WAIT只用于阵亡或事件强制跳过动作。
	out.action["action"]="defend";
	out.reason="defend";

	auto occupied=[&](Point p)
	{
		for(const auto &e:f.enemies)
			if(e.position==p) return true;
		return false;
	};

	auto blocked=[&](Point p,bool immediate=true)
	{
		if(!inside(p)) return true;
		const auto &c=cells_[index(p)];
		return occupied(p) || split_wall(p,tick) || (immediate && tick<blocked_until_[index(p)]) ||
			   (c.kind==2 && !events_.temporary_seen(p,c.seen,tick));
	};

	auto contention=[&](Point p)
	{
		double chance=0;
		for(const auto &e:f.enemies)
			if(distance(e.position,p)==1) chance=std::max(chance,e.defending?.15:.6);
		return chance;
	};

	auto can_hit=[&](const Robot &e,Point p,bool other_target=false)
	{
		const auto fx=events_.effects(e.team,tick,history_,false);
		if(e.protected_ || e.hp<=events_.hazard(e.position,tick,false) || !hits(e.position,p,fx)) return false;
		auto between=[&](Point q)
		{
			return q!=p && q!=e.position && distance(e.position,q)+distance(q,p)==distance(e.position,p);
		};
		if(!fx.radius)
		{
			if(other_target && between(f.position)) return false;
			for(const auto &other:f.enemies)
				if(between(other.position)) return false;
		}
		return true;
	};

	const double reflection=events_.reflection(tick);
	auto enemy_damage=[&](Point p,bool defend)
	{
		double damage=0;
		if(defend && reflection>0) return damage;

		for(const auto &e:f.enemies)
			if(can_hit(e,p))
			{
				const auto fx=events_.effects(e.team,tick,history_,false);
				damage+=(defend?fx.defended:fx.attack)*own.multiplier;
			}
		return damage;
	};

	const double incoming=f.protected_?0:enemy_damage(f.position,false);
	const double defended=f.protected_?0:enemy_damage(f.position,true);
	const double death_cost=lost_time+damage_value(f.damage)-damage_value(projected_damage(f.damage,0,1));
	const double caution=1+std::max(0.0,60-hp)/20;
	out.incoming=incoming;

	auto combat_value=[&](double gain,double damage)
	{
		const bool dead=damage>=hp;
		const double after=projected_damage(f.damage,gain,dead);
		return damage_value(after)-damage_value(f.damage)-
			   (remaining>1?.012*caution*std::max(0.0,std::min(hp,damage)-own.heal):0)-(dead?lost_time:0);
	};

	auto risk=[&](double damage)
	{
		return -combat_value(0,damage);
	};

	auto attack_reward=[&](const Robot &target,double damage,bool normal,bool guarding,int guards=0)
	{
		std::vector<Hit> others;
		const auto fx=events_.effects(target.team,tick,history_,false);
		for(const auto &e:f.enemies)
			if(e.team!=target.team && !(guards&(1<<(e.team[0]-'A'))) && can_hit(e,target.position,true))
			{
				const auto attacker=events_.effects(e.team,tick,history_,false);
				others.push_back({e.team,(guarding?attacker.defended:attacker.attack)*fx.multiplier,true});
			}
		const double health=target.hp-events_.hazard(target.position,tick,target.protected_);
		return expected_reward(health,{f.team,damage,normal},others);
	};

	// 一帧只计算一次视野增量，路径扩展与资源评估共享结果。
	std::vector<int> vision_gain(width_*height_);
	for(int y=0;y<height_;++y)
		for(int x=0;x<width_;++x)
			if(!cells_[index({x,y})].kind)
				for(int yy=std::max(0,y-f.radius);yy<=std::min(height_-1,y+f.radius);++yy)
					for(int xx=std::max(0,x-f.radius);xx<=std::min(width_-1,x+f.radius);++xx)
						++vision_gain[index({xx,yy})];
	auto unseen=[&](Point p)
	{
		return vision_gain[index(p)];
	};

	auto pepper_risk=[&](Point p)
	{
		double damage=0;
		if(p==f.position && f.protected_) return damage;
		for(const auto &enemy:f.enemies)
		{
			double worst=0;
			for(const auto &e:events_.items)
				if(e.kind=="pepper" && e.active(tick))
					for(const auto &point:e.points)
						if(p!=point && point!=f.position && !blocked(point) && distance(enemy.position,point)==1 &&
						    (p.x==point.x || p.y==point.y)) worst=std::max(worst,e.number("damage"));
			damage+=worst;
		}
		return damage;
	};

	auto pepper_reward=[&](Point p)
	{
		double gain=0,damage=0;
		if(p==f.position) return gain;
		for(const auto &e:events_.items)
			if(e.kind=="pepper" && e.active(tick))
				for(const auto &point:e.points)
					if(p==point) damage+=e.number("damage");
		if(damage<=0) return gain;
		for(const auto &target:f.enemies)
			if(!target.protected_ && (p.x==target.position.x || p.y==target.position.y))
			{
				const double health=target.hp-events_.hazard(target.position,tick,false);
				double combat=0;
				std::vector<Hit> others;
				for(const auto &e:f.enemies)
					if(e.team!=target.team)
					{
						if(can_hit(e,target.position,true)) combat+=events_.effects(e.team,tick,history_,false).attack*
							    events_.effects(target.team,tick,history_,false).multiplier;
						double extra=0;
						for(const auto &ev:events_.items)
							if(ev.kind=="pepper" && ev.active(tick))
								for(const auto &q:ev.points)
									if(q!=p && q!=f.position && !blocked(q) && distance(e.position,q)==1 &&
									    (q.x==target.position.x || q.y==target.position.y))
										extra=std::max(extra,ev.number("damage"));
						if(extra>0) others.push_back({e.team,extra,false});
					}
				// 移动发生在战斗后；可能已在战斗中阵亡的目标不再预支辣椒奖励。
				if(health>combat) gain+=.5*expected_reward(health,{f.team,damage,false},others).kill;
			}
		return gain;
	};

	const int size=width_*height_,root=index(f.position);
	std::vector<double> cost(size,1e9);
	std::vector<int> steps(size,1000000),first(size,-1);
	using Entry=std::pair<double,int>;
	std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> queue;
	cost[root]=0;
	steps[root]=0;
	queue.push({0,root});

	while(!queue.empty())
	{
		const auto [value,v]=queue.top();
		queue.pop();
		if(value>cost[v]+1e-9) continue;
		const Point p{v%width_,v/width_};

		for(int d=0;d<4;++d)
		{
			const Point q{p.x+DELTA[d].x,p.y+DELTA[d].y};
			if(blocked(q,v==root)) continue;
			const int u=index(q);
			// 争格冷却只阻止当前步；后续路线可等待恢复，不能把资源当成永久不可达。
			const int delay=std::max(0,blocked_until_[u]-tick-steps[v]-1);
			const int arrival=tick+steps[v]+1+delay;
			if(split_wall(q,arrival)) continue;
			const double danger=events_.hazard(q,arrival,false);
			const double next=value+1+delay+.025*caution*enemy_damage(q,false)+.15*danger+
			    2*contention(q)-.001*unseen(q);
			if(next>=cost[u]-1e-9) continue;
			cost[u]=next;
			steps[u]=steps[v]+1+delay;
			first[u]=(v==root?d:first[v]);
			queue.push({next,u});
		}
	}

	auto completed=[&](const Json::Value &id)
	{
		for(const auto &done:f.completed)
			if(done==id) return true;
		return false;
	};

	auto pending=[&](const char *type)
	{
		for(const auto &m:f.missions)
			if(m["type"]==type && !completed(m["id"])) return true;
		return false;
	};

	auto gain_value=[&](double gain,double damage)
	{
		double value=combat_value(gain,damage);
		if(damage<hp)
			for(const auto &m:f.missions)
				if(m["type"]=="effective_damage" && !completed(m["id"]))
					value+=.02*std::min(gain,std::max(0.0,m["target"].asDouble()-f.damage));
		return value;
	};

	double priority=-1,goal_value=0;
	Point goal=f.position;
	std::string reason;

	auto goal_candidate=[&](Point p,double value,const char *why)
	{
		const int i=index(p);
		if(steps[i]>remaining || cost[i]>1e8) return;
		const double score=value/(cost[i]+3);
		if(score>priority)
		{
			priority=score;
			goal_value=value;
			goal=p;
			reason=why;
		}
	};

	auto resource_value=[&](const rmtc::Entity &e)
	{
		double value=.25*std::min(e.value,std::max(0,120-f.resource_value))+.025*e.value+.01*unseen(e.position);
		if(pending(e.value>=20?"high_resources":"normal_resources"))
			value+=e.value>=20?5:2.5;
		if(pending("resource_value")) value+=2;
		for(const auto &enemy:f.enemies)
			if(distance(enemy.position,e.position)+1<steps[index(e.position)])
				value*=.75;
		return value;
	};
	for(const auto &[id,e]:resources_)
	{
		const int start=index(e.position);
		if(cost[start]>1e8 || steps[start]>remaining) continue;
		double value=resource_value(e);
		// DS的短期目标连续性改为软偏好；资源消失、路线阻断或死亡时自然解除。
		if(id==resource_goal_ && tick-goal_since_<12 && first[start]>=0) value*=1.12;
		goal_candidate(e.position,value,"collect resource");
	}

	for(const auto &m:f.missions)
		if(m["type"]=="reach" && !completed(m["id"]))
			goal_candidate({m["position"][0].asInt(),m["position"][1].asInt()},resources_.empty()?6:2,"reach mission");

	for(const auto &e:events_.items)
		if(e.targets.empty() && e.active(tick))
		{
			if(e.kind=="angel" && hp<80)
				for(const auto &p:e.points)
					if(steps[index(p)]<e.end-tick && p!=f.position)
						goal_candidate(p,
							std::min(100-hp,e.number("heal")*e.number("effect_ticks"))*.05,
							"enter healing point");
			if(e.kind=="unrivaled" && !own.radius && e.end-tick>e.number("stay"))
				for(int y=e.lo.y;y<=e.hi.y;++y)
					for(int x=e.lo.x;x<=e.hi.x;++x)
						if(steps[index({x,y})]+e.number("stay")<e.end-tick) goal_candidate({x,y},3.5,"occupy fortress");


			if(e.kind=="pepper")
				for(const auto &p:e.points)
					if(steps[index(p)]<e.end-tick && pepper_reward(p)>0)
						goal_candidate(p,damage_value(f.damage+pepper_reward(p))-damage_value(f.damage),
							"trigger pepper");
		}

	const bool collecting=reason=="collect resource";
	for(int i=0;i<size;++i)
		if(cells_[i].kind!=2 || events_.temporary_seen({i%width_,i/width_},cells_[i].seen,tick))
		{
			const Point p{i%width_,i/width_};
			const int gain=unseen(p);
			if(gain) goal_candidate(p,(collecting?.02:.055)*gain,"explore resources");
		}

	// 资源尚未达标且仍有可达的新视野时，先完成探索，不因暂时看不到资源就主动追敌。
	const bool exploring=reason=="reach mission" || (reason=="explore resources" && f.resource_value<120);
	for(const auto &[team,m]:enemies_)
	{
		(void)team;

		for(int y=std::max(0,m.robot.position.y-2);y<=std::min(height_-1,m.robot.position.y+2);++y)
			for(int x=std::max(0,m.robot.position.x-2);x<=std::min(width_-1,m.robot.position.x+2);++x)
				if(hits({x,y},m.robot.position,own))
					goal_candidate({x,y},
						(resources_.empty() && !exploring?(f.damage<DAMAGE_CAP?5.0:.25):.5)*
						(own.radius || distance({x,y},m.robot.position)==1?1:.5)*
						std::min(1.0,hp/60)*std::exp(-(tick-m.tick)/20.0),
						"approach observed enemy");
	}
	if(priority<.015)
		for(int i=0;i<size;++i)
			if(cells_[i].kind!=2 && tick-cells_[i].seen>15)
				goal_candidate({i%width_,i/width_},std::min(100,tick-cells_[i].seen)*.02,"patrol last observed area");
	const double fire=pepper_risk(f.position);
	double best=gain_value(0,defended+fire);

	auto choose=[&](double value,const char *action,int direction,Point target,const std::string &why)
	{
		if(value<=best+1e-9) return;
		best=value;
		out.action["action"]=action;
		out.action.removeMember("direction");
		if(direction>=0) out.action["direction"]=DIR[direction];
		out.target=target;
		out.reason=why;
	};

	if(incoming>0)
	{
		double gain=0;
		if(reflection>0)
			for(const auto &e:f.enemies)
				if(can_hit(e,f.position)) gain+=.5*attack_reward(e,reflection,false,false).kill;
		choose(gain_value(gain,defended+fire),"defend",-1,f.position,
			reflection>0?"reflect incoming attack":"reduce incoming combat damage");
	}
	if(!f.protected_ && (own.attack>0 || own.defended>0))
		for(int d=0;d<4;++d)
		{
			std::vector<Robot> targets;
			if(own.radius)
			{
				for(const auto &e:f.enemies)
					if(hits(f.position,e.position,own)) targets.push_back(e);
			}
			else
			{
				int nearest=3;

				for(const auto &e:f.enemies)
					for(int n=1;n<=2;++n)
						if(e.position==Point{f.position.x+DELTA[d].x*n,f.position.y+DELTA[d].y*n} && n<nearest)
						{
							nearest=n;
							targets={e};
						}
			}
			targets.erase(std::remove_if(targets.begin(),targets.end(),[](const auto &e)
			{
				return e.protected_;
			}),targets.end());
			if(targets.empty()) continue;
			double value=0;
			// 防御和攻击互斥；最多三名目标，分别结算每个分支的击杀、死亡与收益。
			for(int mask=0;mask<(1<<targets.size());++mask)
			{
				double chance=1,gain=0,damage=incoming+fire;
				int guards=0;
				for(int i=0;i<int(targets.size());++i)
					if(mask>>i&1) guards|=1<<(targets[i].team[0]-'A');
				for(int i=0;i<int(targets.size());++i)
				{
					const auto &e=targets[i];
					const bool guard=mask>>i&1;
					const double probability=e.defending?.8:.15;
					const auto fx=events_.effects(e.team,tick,history_,false);
					chance*=guard?probability:1-probability;
					if(guard && can_hit(e,f.position)) damage-=fx.attack*own.multiplier;
					if(guard && reflection>0) damage+=reflection;
					else
					{
						const auto reward=attack_reward(e,(guard?own.defended:own.attack)*fx.multiplier,true,guard,
							guards);
						gain+=reward.attack+reward.kill;
					}
				}
				double branch=gain_value(gain,damage);
				if(reason=="collect resource") branch-=.6*(branch-gain_value(0,damage));
				value+=chance*branch;
			}
			choose(value,"attack",d,f.position,"attack before movement");
		}
	const int navigation=first[index(goal)];
	if(reason=="collect resource")
		for(const auto &[id,e]:resources_)
			if(e.position==goal) out.resource_goal=id;

	for(int d=0;d<4;++d)
	{
		const Point p{f.position.x+DELTA[d].x,f.position.y+DELTA[d].y};
		// 战斗先结算；无法撑到移动时，不预支到达、逃离与资源收益。
		if(blocked(p) || hp<=incoming) continue;
		const double next_hazard=events_.hazard(p,tick+1,false)+(split_wall(p,tick+1)?100:0);
		const double pepper=pepper_risk(p);
		double value=(d==navigation?std::min(1.4,goal_value/(cost[index(goal)]+2)):-.12)+
		    gain_value(pepper_reward(p),incoming+pepper);
		double future_multiplier=1;

		for(const auto &e:events_.items)
			if(e.kind=="pan" && e.targets.empty() && e.start<=tick && tick<e.number("window_end"))
				future_multiplier=std::max(future_multiplier,e.number("multiplier")/own.multiplier);
		const double next_damage=enemy_damage(p,false)*future_multiplier;
		value+=.018*caution*(incoming-next_damage)-.2*next_hazard-contention(p);
		if(hp>incoming+pepper &&
		    next_damage+next_hazard>=std::min(100.0,hp-incoming-pepper+own.heal)) value-=death_cost;
		value+=.2*(events_.hazard(f.position,tick+1,f.protected_)+(split_wall(f.position,tick+1)?100:0));

		for(const auto &[id,e]:resources_)
			if(e.position==p)
			{
				(void)id;
				value+=.25*std::min(e.value,std::max(0,120-f.resource_value));
			}
		if(own.heal>0 && hp<100) value+=.01*own.heal;
		choose(value,"move",own.inverted?(d+2)%4:d,p,d==navigation?reason:"leave danger");
	}
	if(goal==f.position && reason=="occupy fortress" && best<.15)
		choose(.15-risk(defended+fire),"defend",-1,f.position,reason);
	out.utility=best;
	return out;
}
}
