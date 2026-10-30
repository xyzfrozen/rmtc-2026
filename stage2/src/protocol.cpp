#include "battle.hpp"
#include <memory>
#include <cmath>

namespace battle
{
bool integer(const Json::Value &v,int low,int high)
{
	return (v.type()==Json::intValue || v.type()==Json::uintValue) && v.isInt64() && v.asInt64()>=low &&
	       v.asInt64()<=high;
}

bool read_json(const std::string &s,Json::Value &v)
{
	if(s.size()>1024*1024) return false;
	Json::CharReaderBuilder b;
	b["allowComments"]=false;
	b["allowTrailingCommas"]=false;
	b["rejectDupKeys"]=true;
	b["failIfExtra"]=true;
	b["stackLimit"]=32;
	std::unique_ptr<Json::CharReader> r(b.newCharReader());
	std::string error;
	return r->parse(s.data(),s.data()+s.size(),&v,&error);
}

bool parse_frame(const std::string &text,const std::string &team,Frame &out,std::string &error)
{
	error="invalid stage-two perception";
	try
	{
		Json::Value v;
		if(!read_json(text,v) || !v.isObject() || !v["event_text"].isString()) return false;
		Frame f;
		f.team=team;
		f.event_text=v["event_text"].asString();
		// Reuse the stage-one geometry/schema decoder, while handling events separately.
		v["event_text"]="";
		if(!rmtc::parse_frame(rmtc::compact_json(v),team,f,error)) return false;
		const auto &self=v["self"];
		if(!integer(self["hp"],0,1000000) || !self["protected"].isBool()) return false;
		f.hp=self["hp"].asInt();
		f.protected_=self["protected"].asBool();
		f.respawned=self["position_reason"]=="respawn";
		const auto &score=v["score"];
		if(!score.isObject() || !score["effective_damage"].isNumeric() ||
		    !std::isfinite(score["effective_damage"].asDouble()) || score["effective_damage"].asDouble()<0)
			return false;
		// v1.1 已含奖励和死亡扣除；允许下降及超过500，不在本地再次结算。
		f.damage=score["effective_damage"].asDouble();
		f.completed=score["mission_completed_ids"];
		f.missions=v["missions"];
		if(!f.completed.isArray() || !f.missions.isArray() || f.missions.size()>32) return false;

		for(const auto &id:f.completed)
			if(!id.isString()) return false;

		for(const auto &m:f.missions)
		{
			if(!m.isObject() || !m["id"].isString() || !m["type"].isString()) return false;
			const auto type=m["type"].asString();
			if(type=="reach")
			{
				const auto &p=m["position"];
				if(!p.isArray() || p.size()!=2 ||
				    !integer(p[0],0,f.width-1) || !integer(p[1],0,f.height-1)) return false;
			}
			else if(type!="normal_resources" && type!="high_resources" && type!="resource_value" &&
			    type!="effective_damage")
				return false;
			else if(!integer(m["target"],1,1000000))
				return false;
		}
		std::size_t i=0;
		std::set<std::string> seen{team};

		for(const auto &e:v["visible_entities"])
		{
			const auto type=e["type"].asString();
			if(type!="robot" && type!="obstacle" && type!="resource_normal" && type!="resource_high") continue;
			if(type=="robot")
			{
				if(!e["team"].isString() || e["team"].asString().size()!=1 ||
				    std::string("ABCD").find(e["team"].asString())==std::string::npos ||
				    !integer(e["hp"],1,1000000) || !e["defending"].isBool() || !e["protected"].isBool() ||
				    !seen.insert(e["team"].asString()).second)
					return false;
				f.enemies.push_back({e["team"].asString(),f.entities.at(i).position,
					                 e["hp"].asInt(),e["defending"].asBool(),e["protected"].asBool()});
			}
			++i;
		}
		out=std::move(f);
		error.clear();
		return true;
	}
	catch(const std::exception &)
	{
		return false;
	}
}
}
