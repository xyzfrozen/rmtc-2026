#include "battle.hpp"
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

namespace
{
void require(bool ok,const char *message)
{
	if(!ok) throw std::runtime_error(message);
}

battle::Frame frame(int tick=0)
{
	battle::Frame f;
	f.session="fusion";
	f.team="A";
	f.tick=tick;
	f.width=f.height=9;
	f.position={4,4};
	f.radius=4;
	f.missions=f.completed=Json::Value(Json::arrayValue);
	return f;
}
}

int main()
{
	try
	{
		battle::Bot bot;
		auto f=frame();
		f.entities={{"resource_high","left",{2,4},20},{"resource_high","right",{6,4},20}};
		auto first=bot.update(f);
		require(first && !first->resource_goal.empty(),"resource objective not recorded");
		f.tick=1;
		const std::string gone=first->resource_goal;
		f.entities.erase(f.entities.begin()+(gone=="left"?0:1));
		auto second=bot.update(f);
		require(second && second->resource_goal!=gone,"resource commitment survived visible disappearance");
		require(bot.update(f)->resource_goal==second->resource_goal,"duplicate frame advanced objective state");
		f.tick=2;
		f.alive=false;
		require(bot.update(f)->resource_goal.empty(),"death retained a stale resource objective");

		// 不对称障碍围住一个高价值资源；不能借另一侧布局推断可达。
		battle::Bot asymmetric;
		f=frame();
		f.entities={{"resource_high","sealed",{1,1},20},{"resource_normal","open",{5,4},10},
			{"obstacle","",{0,1},0},{"obstacle","",{2,1},0},
			{"obstacle","",{1,0},0},{"obstacle","",{1,2},0}};
		auto d=asymmetric.update(f);
		require(d && d->resource_goal=="open" && d->target==battle::Point{5,4},"unreachable lookahead distorted resource route");

		std::mt19937 random(29605);
		const std::string valid=R"({"tick":0,"session_id":"fuzz","vision_radius":4,"map":{"width":9,"height":9},"self":{"team":"A","position":[4,4],"hp":100,"alive":true,"protected":false,"last_action":null},"visible_entities":[],"score":{"resource_value":0,"effective_damage":0,"mission_completed_ids":[],"invalid_actions":0},"missions":[],"event_text":""})";
		for(int i=0;i<2000;++i)
		{
			std::string input=valid;
			for(int j=0,n=1+random()%8;j<n;++j) input[random()%input.size()]=char(random()%128);
			battle::Frame parsed;
			std::string error;
			if(battle::parse_frame(input,"A",parsed,error))
			{
				battle::Bot guarded;
				const auto action=guarded.update(parsed);
				if(action) require(std::isfinite(action->utility),"nonfinite decision from accepted perception");
			}
		}
		for(int i=0;i<250;++i)
		{
			battle::Bot varied;
			f=frame();
			f.width=9+random()%24;
			f.height=9+random()%24;
			f.position={int(random()%f.width),int(random()%f.height)};
			f.hp=1+random()%100;
			f.damage=random()%1200;
			f.resource_value=random()%201;
			for(int y=std::max(0,f.position.y-4);y<std::min(f.height,f.position.y+5);++y)
				for(int x=std::max(0,f.position.x-4);x<std::min(f.width,f.position.x+5);++x)
					if(battle::Point{x,y}!=f.position && random()%9==0)
						f.entities.push_back({"obstacle","",{x,y},0});
			d=varied.update(f);
			require(d && d->action["action"]!="wait","alive fallback did not defend");
			if(d->action["action"]=="move")
			{
				require(d->target.x>=0 && d->target.x<f.width && d->target.y>=0 && d->target.y<f.height,"move outside non-square map");
				for(const auto &e:f.entities) require(e.position!=d->target,"move into observed obstacle");
			}
		}
		std::cout<<"PASS: objective invalidation, asymmetric reachability, 2000 malformed perceptions, 250 varied maps\n";
	}
	catch(const std::exception &e)
	{
		std::cerr<<e.what()<<'\n';
		return 1;
	}
}
