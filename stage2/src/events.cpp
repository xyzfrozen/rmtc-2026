#include "battle.hpp"
#include <algorithm>
#include <cmath>

namespace battle
{
bool Events::merge(const std::string &text,int width,int height,std::string &error)
{
	error="invalid event interpretation";
	try
	{
		Json::Value root;
		if(!read_json(text,root) || !root.isObject() || !root["events"].isArray() || root["events"].size()>32)
			return false;
		std::vector<Event> parsed;

		for(const auto &v:root["events"])
		{
			if(!v.isObject() || !v["kind"].isString() || !v["scope"].isString() ||
			    !integer(v["start"],0,1000000) || !integer(v["end"],1,1000000))
				return false;
			Event e;
			e.data=v;
			e.kind=v["kind"].asString();
			e.start=v["start"].asInt();
			e.end=v["end"].asInt();
			const auto scope=v["scope"].asString();
			if(e.end<=e.start || (scope!="zone" && scope!="effect")) return false;
			if(e.kind!="explosion" && e.kind!="illusion" && e.kind!="angel" && e.kind!="unrivaled" &&
			    e.kind!="world_split" && e.kind!="pepper" && e.kind!="purity" && e.kind!="pan")
				return false;

			auto point=[&](const Json::Value &p,Point &out)
			{
				if(!p.isArray() || p.size()!=2 || !integer(p[0],0,width-1) || !integer(p[1],0,height-1)) return false;
				out={p[0].asInt(),p[1].asInt()};
				return true;
			};

			if(scope=="effect")
			{
				if(e.kind!="illusion" && e.kind!="angel" && e.kind!="unrivaled" && e.kind!="pan") return false;
				if(!v["targets"].isArray() || v["targets"].empty() || v["targets"].size()>4) return false;

				for(const auto &t:v["targets"])
				{
					if(!t.isString() || t.asString().size()!=1 ||
					    std::string("ABCD").find(t.asString())==std::string::npos)
						return false;
					e.targets.insert(t.asString());
				}
			}
			if(scope=="zone" && (e.kind=="explosion" || e.kind=="illusion" || e.kind=="unrivaled"))
			{
				const auto &r=v["rect"];
				if(!r.isArray() || r.size()!=4 ||
				    !integer(r[0],0,
					width-1) ||
				    !integer(r[1],0,
					height-1) || !integer(r[2],r[0].asInt(),width-1) || !integer(r[3],r[1].asInt(),height-1))
					return false;
				e.lo={r[0].asInt(),r[1].asInt()};
				e.hi={r[2].asInt(),r[3].asInt()};
			}
			if(scope=="zone" && (e.kind=="angel" || e.kind=="pepper" || e.kind=="world_split"))
			{
				if(!v["points"].isArray() || v["points"].empty() || v["points"].size()>64) return false;

				for(const auto &p:v["points"])
				{
					Point q;
					if(!point(p,q)) return false;
					e.points.push_back(q);
				}
				if(e.kind=="world_split" && e.points.size()!=1) return false;
			}

			auto number=[&](const char *key,double low,double high)
			{
				return v[key].isNumeric() && std::isfinite(v[key].asDouble()) && v[key].asDouble()>=low &&
				       v[key].asDouble()<=high;
			};

			if((e.kind=="explosion" || e.kind=="pepper") &&
			    (!number("damage",0,10000) || !v["respects_protection"].isBool()))
				return false;
			if(e.kind=="illusion" && scope=="zone" && !integer(v["effect_ticks"],1,5000)) return false;
			if(e.kind=="angel" &&
			    (!number("heal",0,
					10000) || !number("hp_cap",1,10000) || (scope=="zone" && !integer(v["effect_ticks"],1,5000))))
				return false;
			if(e.kind=="unrivaled" &&
			    (!integer(v["radius"],1,
					16) ||
			    !number("attack_damage",0,
				10000) || !number("defended_damage",0,10000) || (scope=="zone" && !integer(v["stay"],1,5000))))
				return false;
			if(e.kind=="purity" && !number("reflection",0,10000)) return false;
			if(e.kind=="pan" &&
			    (!number("multiplier",1,100) || (scope=="zone" && !integer(v["window_end"],e.start+1,e.end))))
				return false;
			parsed.push_back(e);
		}
		auto next=items;

		for(const auto &e:parsed)
		{
			auto same=[&](const Event &a)
			{
				return a.kind==e.kind && a.start==e.start && a.targets==e.targets && a.lo==e.lo && a.hi==e.hi &&
				       a.points==e.points;
			};

			auto it=std::find_if(next.begin(),next.end(),same);
			if(it==next.end()) next.push_back(e);
			else
				*it=e;
		}
		if(next.size()>128) return false;
		items=std::move(next);
		error.clear();
		return true;
	}
	catch(const std::exception &)
	{
		return false;
	}
}

Json::Value Events::memo() const
{
	Json::Value result(Json::arrayValue);

	for(const auto &e:items)
		result.append(e.data);
	return result;
}

Effects Events::effects(const std::string &team,int tick,const std::map<int,Frame> &history,bool infer_self) const
{
	Effects result;

	for(const auto &e:items)
	{
		bool applies=e.targets.count(team) && e.active(tick);
		if(e.targets.empty() && infer_self && tick>=e.start)
		{
			if(e.kind=="illusion" && tick<e.start+e.number("effect_ticks"))
			{
				auto it=history.find(e.start);
				applies=it!=history.end() && it->second.alive && e.contains(it->second.position);
			}
			if(e.kind=="angel")
			{
				for(auto it=history.lower_bound(e.start+1);it!=history.end() && it->first<=tick;++it)
				{
					const auto &f=it->second;
					const int moved_tick=it->first-1;
					if(moved_tick>=e.end || tick>=moved_tick+e.number("effect_ticks")) continue;
					if(f.last_action["moved"].isBool() && f.last_action["moved"].asBool() &&
					    integer(f.last_action["tick"],moved_tick,moved_tick) &&
					    std::find(e.points.begin(),e.points.end(),f.position)!=e.points.end())
						applies=true;
				}
			}
			if(e.kind=="unrivaled" && e.active(tick))
			{
				int stayed=0,previous=e.start;

				for(auto it=history.lower_bound(e.start+1);it!=history.end() && it->first<=tick;++it)
				{
					const auto &f=it->second;
					const auto before=history.find(it->first-1);
					if(f.respawned) stayed=0;
					// 普通复活在驻留结算之后；分裂立即复活清空进度，但可累计本Tick。
					if(it->first!=previous+1 || !f.alive || !e.contains(f.position) ||
					    (before!=history.end() && !before->second.alive) ||
					    (f.respawned && before==history.end())) stayed=0;
					else
						++stayed;
					if(stayed>=e.number("stay")) applies=true;
					previous=it->first;
				}
			}
			if(e.kind=="pan" && e.active(tick))
			{
				for(auto it=history.lower_bound(e.start+1);
				    it!=history.end() && it->first<=tick && it->first<=e.number("window_end");++it)
					if(it->second.last_action["moved"].isBool() && it->second.last_action["moved"].asBool() &&
					    integer(it->second.last_action["tick"],it->first-1,it->first-1))
						applies=true;
			}
		}
		if(!applies) continue;
		if(e.kind=="illusion") result.inverted=true;
		if(e.kind=="angel") result.heal=std::max(result.heal,e.number("heal"));
		if(e.kind=="pan") result.multiplier=std::max(result.multiplier,e.number("multiplier"));
		if(e.kind=="unrivaled")
		{
			result.radius=e.number("radius");
			result.attack=e.number("attack_damage");
			result.defended=e.number("defended_damage");
		}
	}
	return result;
}

bool Events::wall(Point p,int tick) const
{
	for(const auto &e:items)
		if(e.kind=="world_split" && e.active(tick) && (p.x==e.points[0].x || p.y==e.points[0].y))
			return true;
	return false;
}

bool Events::temporary_seen(Point p,int seen,int now) const
{
	for(const auto &e:items)
		if(e.kind=="world_split" && e.active(seen) && now>=e.end && (p.x==e.points[0].x || p.y==e.points[0].y))
			return true;
	return false;
}

double Events::hazard(Point p,int tick,bool protected_) const
{
	double damage=0;

	for(const auto &e:items)
		if(e.kind=="explosion" && e.active(tick) && e.contains(p) &&
		    (!protected_ || !e.data["respects_protection"].asBool()))
			damage+=e.number("damage");
	return damage;
}

double Events::reflection(int tick) const
{
	double value=0;

	for(const auto &e:items)
		if(e.kind=="purity" && e.active(tick))
			value=std::max(value,e.number("reflection"));
	return value;
}
}
