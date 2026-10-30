#include "rmtc_stage1/collector.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool ok,const char *message)
{
	if(!ok) throw std::runtime_error(message);
}

std::string grid(int dx,int dy)
{
	std::string s;

	for(int y=0;y<9;++y)
	{
		for(int x=0;x<9;++x)
			s+=(x==4 && y==4)?'1':((x==4+dx && y==4+dy)?'2':'0');
		if(y<8) s+='\n';
	}
	return s;
}

Json::Value frame(int tick=0,const std::string &session="test-1")
{
	Json::Value v;
	v["tick"]=tick;
	v["session_id"]=session;
	v["vision_radius"]=4;
	v["map"]["width"]=9;
	v["map"]["height"]=9;
	v["self"]["team"]="A";
	v["self"]["alive"]=true;
	v["self"]["position"].append(4);
	v["self"]["position"].append(4);
	v["self"]["last_action"]=Json::nullValue;
	v["visible_entities"]=Json::Value(Json::arrayValue);
	v["event_text"]="";
	return v;
}

void add(Json::Value &v,const char *type,int dx,int dy,const char *id="r",int value=10)
{
	Json::Value e;
	e["type"]=type;
	e["position"]=grid(dx,dy);
	if(std::string(type).find("resource_")==0)
	{
		e["id"]=id;
		e["value"]=value;
	}
	v["visible_entities"].append(e);
}

rmtc::Frame parsed(const Json::Value &v)
{
	rmtc::Frame f;
	std::string error;
	require(rmtc::parse_frame(rmtc::compact_json(v),"A",f,error),error.c_str());
	return f;
}

void rejected(const Json::Value &v)
{
	rmtc::Frame f;
	std::string error;
	require(!rmtc::parse_frame(rmtc::compact_json(v),"A",f,error),"malformed input accepted");
}
}

int main()
{
	try
	{
		auto v=frame();
		add(v,"resource_high",1,-2,"high",20);
		auto f=parsed(v);
		require(f.entities[0].position==rmtc::Point{5,2},"grid axis/sign incorrect");

		v=frame();
		add(v,"resource_normal",0,0);

		rmtc::Collector same;
		require(same.update(parsed(v))->action["action"]=="wait","must collect resource at current cell");

		for(int kind=0;kind<9;++kind)
		{
			v=frame();
			if(kind==0) v["tick"]=1.0;
			if(kind==1) v["tick"]=true;
			if(kind==2) v["session_id"]=123;
			if(kind==3) v["vision_radius"]=5;
			if(kind==4) v["self"]["position"][0]=-1;
			if(kind==5) v["self"]["team"]="B";
			if(kind==6) v["event_text"]="非空公告";
			if(kind==7) v["map"]["width"]=0;
			if(kind==8) v["visible_entities"]=Json::nullValue;
			rejected(v);
		}

		v=frame();
		add(v,"obstacle",1,0);
		v["visible_entities"][0]["position"]="000";
		rejected(v);

		v=frame();
		add(v,"obstacle",1,0);
		auto s=grid(1,0);
		s[0]='2';
		v["visible_entities"][0]["position"]=s;
		rejected(v);

		v=frame();
		add(v,"resource_normal",1,0);
		add(v,"resource_high",0,1,"r",20);
		rejected(v);
		rmtc::Frame unused;
		std::string error;
		require(!rmtc::parse_frame("{\"tick\":0,\"tick\":1}","A",unused,error),"duplicate JSON key accepted");
		require(!rmtc::parse_frame("{\"self\":3}","A",unused,error),"bad self accepted");

		v=frame();
		add(v,"obstacle",1,0);
		add(v,"resource_high",2,0,"high",20);

		rmtc::Collector detour;
		auto d=detour.update(parsed(v));
		require(d && d->action["action"]=="move" && d->action["direction"]!="right","walked into visible obstacle");
		require(d->observed_cells==81,"complete visible square not recorded");
		auto duplicate=detour.update(parsed(v));
		require(duplicate && duplicate->replayed && duplicate->action==d->action,"duplicate tick advanced planner");
		v["tick"]=3;
		detour.update(parsed(v));
		v["tick"]=2;
		require(!detour.update(parsed(v)),"old tick accepted");
		v["tick"]=100;
		require(!detour.update(parsed(v)),"action published for final state");

		v=frame(0,"test-2");
		auto fresh=detour.update(parsed(v));
		require(fresh && fresh->action["action"]=="wait" && fresh->known_resources==0,"new game retained resources");
		require(!detour.update(parsed(frame(4,"test-1"))),"retired game revived");

		rmtc::Collector late;

		v=frame(99);
		add(v,"resource_normal",1,0,"near",10);
		add(v,"resource_high",0,2,"far",20);
		d=late.update(parsed(v));
		require(d->action["direction"]=="right","late decision chose unreachable reward");

		v=frame();
		v["self"]["alive"]=false;

		rmtc::Collector dead;
		require(dead.update(parsed(v))->action["action"]=="wait","dead robot moved");

		rmtc::Collector memory;

		v=frame();
		v["map"]["width"]=20;
		v["map"]["height"]=20;
		add(v,"resource_normal",4,0);
		memory.update(parsed(v));
		v["tick"]=1;
		v["self"]["position"][0]=1;
		v["visible_entities"]=Json::Value(Json::arrayValue);
		d=memory.update(parsed(v));
		require(d->known_resources==1,"forgot out-of-view resource");
		v["tick"]=2;
		v["self"]["position"][0]=4;
		require(memory.update(parsed(v))->known_resources==0,"retained missing visible resource");

		// Both routes to [5,5] take two moves; the right side was already observed.

		rmtc::Collector sight;

		v=frame();
		v["vision_radius"]=1;
		v["self"]["position"][0]=6;
		sight.update(parsed(v));

		v=frame(1);
		v["vision_radius"]=1;
		add(v,"resource_high",1,1,"diagonal",20);
		d=sight.update(parsed(v));
		require(d->target==rmtc::Point{5,5} && d->action["direction"]=="down",
			"equal-length route missed exploration opportunity");

		v=frame(2);
		v["vision_radius"]=1;
		v["self"]["position"][1]=5;
		add(v,"resource_high",1,0,"diagonal",20);
		require(sight.update(parsed(v))->action["direction"]=="right","exploration lengthened a shortest route");

		rmtc::Collector blind;

		v=frame();
		v["vision_radius"]=0;
		auto before=blind.update(parsed(v));
		require(before->action["action"]=="move","radius zero cannot explore");
		v["tick"]=1;
		v["self"]["last_action"]["tick"]=0;
		v["self"]["last_action"]["error"]="E_BLOCKED";
		auto after=blind.update(parsed(v));
		require(after->action["direction"]!=before->action["direction"],"repeated known blocked move");
		std::cout<<
		    "PASS: strict schema, coordinates, walls, overlap, sessions, duplicate/final ticks, memory, deadlines, radius zero\n";
	}
	catch(const std::exception &e)
	{
		std::cerr<<"FAIL: "<<e.what()<<'\n';
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
