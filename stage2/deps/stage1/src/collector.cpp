#include "rmtc_stage1/collector.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <queue>
#include <stdexcept>

namespace rmtc
{
namespace
{
constexpr int INF=1000000000;
constexpr std::array<Point,4> DELTA={Point{0,-1},Point{1,0},Point{0,1},Point{-1,0}};
constexpr const char *DIRECTION[]={"up","right","down","left"};

bool integer(const Json::Value &v,std::int64_t low,std::int64_t high)
{
	if(v.type()!=Json::intValue && v.type()!=Json::uintValue) return false;
	if(!v.isInt64()) return false;
	return v.asInt64()>=low && v.asInt64()<=high;
}

bool coordinate(const Json::Value &v,int width,int height,Point &p)
{
	if(!v.isArray() || v.size()!=2 || !integer(v[0],0,width-1) || !integer(v[1],0,height-1)) return false;
	p={v[0].asInt(),v[1].asInt()};
	return true;
}

bool relative(const Json::Value &v,Point &p)
{
	if(!v.isString()) return false;
	const auto s=v.asString();
	if(s.size()!=89) return false;
	int objects=0,self=0;
	p={0,0};

	for(int y=0;y<9;++y)
	{
		for(int x=0;x<9;++x)
		{
			const char c=s[y*10+x];
			if(c=='1')
			{
				if(x!=4 || y!=4) return false;
				++self;
			}
			else if(c=='2')
			{
				if(++objects>1) return false;
				p={x-4,y-4};
			}
			else if(c!='0')
				return false;
		}
		if(y<8 && s[y*10+9]!='\n') return false;
	}
	return self==1;
}
}

std::string compact_json(const Json::Value &value)
{
	Json::StreamWriterBuilder writer;
	writer["indentation"]="";
	return Json::writeString(writer,value);
}

bool parse_frame(const std::string &text,const std::string &team,Frame &out,std::string &error)
{
	auto fail=[&](const char *why)
	{
		error=why;
		return false;
	};

	if(text.size()>1024*1024) return fail("perception exceeds 1 MiB");
	Json::CharReaderBuilder builder;
	builder["collectComments"]=false;
	builder["allowComments"]=false;
	builder["allowTrailingCommas"]=false;
	builder["rejectDupKeys"]=true;
	builder["failIfExtra"]=true;
	builder["stackLimit"]=32;
	Json::Value v;
	std::string detail;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	try
	{
		if(!reader->parse(text.data(),text.data()+text.size(),&v,&detail) || !v.isObject())
			return fail("invalid JSON object");
		Frame f;
		if(!integer(v["tick"],0,1000000000) || !v["session_id"].isString() || v["session_id"].asString().empty())
			return fail("invalid tick/session_id");
		f.tick=v["tick"].asInt64();
		f.session=v["session_id"].asString();
		if(f.session.size()>1024) return fail("session_id too long");
		const auto &m=v["map"];
		if(!m.isObject() || !integer(m["width"],1,256) || !integer(m["height"],1,256))
			return fail("invalid map dimensions");
		f.width=m["width"].asInt();
		f.height=m["height"].asInt();
		if(!integer(v["vision_radius"],0,4)) return fail("invalid vision_radius");
		f.radius=v["vision_radius"].asInt();
		const auto &self=v["self"];
		if(!self.isObject() || !self["alive"].isBool() || !coordinate(self["position"],f.width,f.height,f.position))
			return fail("invalid self state");
		if(self.isMember("team") && (!self["team"].isString() || self["team"].asString()!=team))
			return fail("wrong team");
		f.alive=self["alive"].asBool();
		f.last_action=self["last_action"];
		if(!f.last_action.isNull() && !f.last_action.isObject()) return fail("invalid last_action");
		if(!v["visible_entities"].isArray() || v["visible_entities"].size()>8192)
			return fail("invalid visible_entities");
		if(v.isMember("event_text") && (!v["event_text"].isString() || !v["event_text"].asString().empty()))
			return fail("stage-one collector cannot handle event announcements");
		std::unordered_set<std::string> ids;

		for(const auto &item:v["visible_entities"])
		{
			if(!item.isObject() || !item["type"].isString()) return fail("invalid entity type");
			Entity e;
			e.type=item["type"].asString();
			if(e.type!="obstacle" && e.type!="robot" && e.type!="resource_normal" && e.type!="resource_high") continue;
			Point offset;
			if(!relative(item["position"],offset) || std::abs(offset.x)>f.radius || std::abs(offset.y)>f.radius)
				return fail("invalid entity position grid");
			e.position={f.position.x+offset.x,f.position.y+offset.y};
			if(e.position.x<0 || e.position.y<0 || e.position.x>=f.width || e.position.y>=f.height)
				return fail("entity outside map");
			if(e.type=="resource_normal" || e.type=="resource_high")
			{
				if(!item["id"].isString() || item["id"].asString().empty() || !integer(item["value"],1,1000000))
					return fail("invalid resource");
				e.id=item["id"].asString();
				e.value=item["value"].asInt();
				if(!ids.insert(e.id).second) return fail("duplicate resource ID");
			}
			if(e.type=="obstacle" && e.position==f.position) return fail("self overlaps obstacle");
			f.entities.push_back(e);
		}
		if(v["score"].isObject())
		{
			if(integer(v["score"]["resource_value"],0,1000000000))
				f.resource_value=v["score"]["resource_value"].asInt();
			if(integer(v["score"]["invalid_actions"],0,1000000000))
				f.invalid_actions=v["score"]["invalid_actions"].asInt();
		}
		out=std::move(f);
		error.clear();
		return true;
	}
	catch(const std::exception &)
	{
		return fail("perception type/parse exception");
	}
}

Collector::Collector(int total_ticks) : total_ticks_(total_ticks)
{
	if(total_ticks<1 || total_ticks>5000) throw std::invalid_argument("total_ticks must be 1..5000");
}

void Collector::reset(const Frame &f)
{
	if(!session_.empty())
	{
		retired_.insert(session_);
		retired_order_.push_back(session_);
		if(retired_order_.size()>64)
		{
			retired_.erase(retired_order_.front());
			retired_order_.pop_front();
		}
	}
	session_=f.session;
	width_=f.width;
	height_=f.height;
	radius_=f.radius;
	cells_.assign(width_*height_,0);
	occupied_.assign(width_*height_,false);
	resources_.clear();
	observed_=0;
	last_tick_=-1;
	have_cached_=false;
	previous_target_=f.position;
}

void Collector::observe(const Frame &f)
{
	radius_=f.radius;
	std::fill(occupied_.begin(),occupied_.end(),false);
	// Only a matching failed move is evidence about an unobserved adjacent obstacle.
	if(have_cached_ && cached_.action["action"]=="move" && f.position==previous_position_ &&
	    integer(f.last_action["tick"],0,1000000000) && f.last_action["tick"].asInt64()==last_tick_ &&
	    f.last_action["error"].isString() && f.last_action["error"].asString()=="E_BLOCKED")
	{
		for(int d=0;d<4;++d)
			if(cached_.action["direction"]==DIRECTION[d])
			{
				Point p{previous_position_.x+DELTA[d].x,previous_position_.y+DELTA[d].y};
				if(inside(p)) cells_[index(p)]=2;
			}
	}
	if(!f.alive) return;

	auto visible=[&](Point p)
	{
		return std::abs(p.x-f.position.x)<=radius_ && std::abs(p.y-f.position.y)<=radius_;
	};

	for(auto it=resources_.begin();it!=resources_.end();)
	{
		if(visible(it->second.position)) it=resources_.erase(it);
		else
			++it;
	}

	// The complete entity list establishes free cells inside the visible square;
	// a '0' in one entity's marker string alone does not establish free space.
	for(int y=std::max(0,f.position.y-radius_);y<=std::min(height_-1,f.position.y+radius_);++y)
		for(int x=std::max(0,f.position.x-radius_);x<=std::min(width_-1,f.position.x+radius_);++x)
			cells_[index({x,y})]=1;

	for(const auto &e:f.entities)
	{
		if(e.type=="obstacle") cells_[index(e.position)]=2;
		else if(e.type=="robot" && e.position!=f.position)
			occupied_[index(e.position)]=true;
		else if(e.type=="resource_normal" || e.type=="resource_high")
			resources_[e.id]={e.position,e.value};
	}
	observed_=static_cast<int>(std::count_if(cells_.begin(),cells_.end(),[](auto c)
	{
		return c!=0;
	}));
}

int Collector::unseen(Point p) const
{
	int gain=0;

	for(int y=std::max(0,p.y-radius_);y<=std::min(height_-1,p.y+radius_);++y)
		for(int x=std::max(0,p.x-radius_);x<=std::min(width_-1,p.x+radius_);++x)
			gain+=cells_[index({x,y})]==0;
	return gain;
}

Collector::Search Collector::search(Point start,bool explore) const
{
	Search s;
	s.distance.assign(cells_.size(),INF);
	s.first.assign(cells_.size(),-1);
	std::vector<int> information(cells_.size()),benefit(cells_.size());
	if(explore)
		for(int i=0;i<width_*height_;++i)
			information[i]=unseen(point(i));
	std::vector<int> queue(cells_.size());
	int begin=0,end=0;
	const int root=index(start);
	s.distance[root]=0;
	queue[end++]=root;

	while(begin<end)
	{
		const int v=queue[begin++];
		const Point p=point(v);

		for(int d=0;d<4;++d)
		{
			Point q{p.x+DELTA[d].x,p.y+DELTA[d].y};
			if(!inside(q)) continue;
			const int u=index(q);
			if(cells_[u]==2 || occupied_[u] || s.distance[u]<s.distance[v]+1) continue;
			// Only break shortest-path ties; visibility is an estimate, not unique coverage.
			const int gain=benefit[v]+information[u];
			if(s.distance[u]==s.distance[v]+1 && gain<=benefit[u]) continue;
			if(s.distance[u]==INF) queue[end++]=u;
			s.distance[u]=s.distance[v]+1;
			s.first[u]=(v==root?d:s.first[v]);
			benefit[u]=gain;
		}
	}
	return s;
}

Decision Collector::plan(const Frame &f) const
{
	Decision result;
	result.action["tick"]=Json::Int64(f.tick);
	result.action["session_id"]=f.session;
	result.action["action"]="wait";
	result.target=f.position;
	result.reason="no reachable benefit";
	result.observed_cells=observed_;
	result.known_resources=static_cast<int>(resources_.size());
	if(!f.alive)
	{
		result.reason="not alive";
		return result;
	}

	for(const auto &[id,r]:resources_)
		if(r.position==f.position)
		{
			(void)id;
			result.reason="collect resource under robot";
			return result;
		}
	const int remaining=total_ticks_-static_cast<int>(f.tick);
	const auto initial=search(f.position,true);
	std::vector<Resource> goals;

	for(const auto &[id,r]:resources_)
	{
		(void)id;
		if(initial.distance[index(r.position)]<=remaining) goals.push_back(r);
	}
	std::stable_sort(goals.begin(),goals.end(),[&](const auto &a,const auto &b)
	{
		return double(a.value)/(initial.distance[index(a.position)]+1)>
		double(b.value)/(initial.distance[index(b.position)]+1);
	});
	if(goals.size()>20) goals.resize(20);
	std::vector<double> discount(remaining+1);
	const double horizon=std::max(8.0,std::min(35.0,remaining*.55));

	for(int i=0;i<=remaining;++i)
		discount[i]=std::exp(-i/horizon);

	struct Frontier
	{
		Point p;
		int gain;
		double priority;
	};

	std::vector<Frontier> frontier;

	for(int i=0;i<width_*height_;++i)
	{
		if(initial.distance[i]<=0 || initial.distance[i]>remaining) continue;
		const Point p=point(i);
		const int gain=unseen(p);
		if(gain) frontier.push_back({p,gain,gain*discount[initial.distance[i]]});
	}
	std::stable_sort(frontier.begin(),frontier.end(),[](const auto &a,const auto &b)
	{
		return a.priority>b.priority;
	});
	std::vector<Frontier> selected;

	for(const auto &ftr:frontier)
	{
		bool near=false;

		for(const auto &other:selected)
			if(std::abs(other.p.x-ftr.p.x)+std::abs(other.p.y-ftr.p.y)<std::max(1,radius_))
				near=true;
		if(!near) selected.push_back(ftr);
		if(selected.size()==24) break;
	}
	std::vector<Search> searches;
	searches.push_back(initial);

	for(const auto &g:goals)
		searches.push_back(search(g.position));
	double best=-1;
	Point target=f.position;
	bool explore=false;

	auto consider=[&](double score,Point p,bool is_explore)
	{
		if(p==previous_target_) score+=.08;
		if(score>best+1e-9)
		{
			best=score;
			target=p;
			explore=is_explore;
		}
	};

	for(const auto &ftr:selected)
	{
		const int distance=initial.distance[index(ftr.p)];
		// Exploration is only an estimate of future opportunity, never hidden-map input.
		consider(std::min(24.0,.26*ftr.gain)*discount[distance],ftr.p,true);
	}

	struct State
	{
		std::uint32_t mask;
		int end,spent,first;
		double reward;
	};

	std::vector<State> beam{{0,0,0,-1,0}};
	const int count=static_cast<int>(goals.size());

	for(int depth=0;depth<std::min(count,12) && !beam.empty();++depth)
	{
		std::vector<State> next;
		next.reserve(beam.size()*count);

		for(const auto &s:beam)
			for(int j=0;j<count;++j)
			{
				if(s.mask&(1u<<j)) continue;
				const int travel=searches[s.end].distance[index(goals[j].position)];
				if(travel>remaining-s.spent) continue;
				const int spent=s.spent+travel;
				State n{s.mask|(1u<<j),j+1,spent,s.first<0?j:s.first,s.reward+goals[j].value*discount[spent]};
				double bonus=0;

				for(const auto &ftr:selected)
				{
					const int d=searches[n.end].distance[index(ftr.p)];
					if(d<=remaining-spent) bonus=std::max(bonus,std::min(24.0,.26*ftr.gain)*discount[spent+d]);
				}
				consider(n.reward+bonus,goals[n.first].position,false);
				next.push_back(n);
			}
		std::stable_sort(next.begin(),next.end(),[](const auto &a,const auto &b)
		{
			if(std::abs(a.reward-b.reward)>1e-9) return a.reward>b.reward;
			return a.spent<b.spent;
		});
		// Collapse identical visited-set/end states; retain the best discounted route.
		beam.clear();
		std::unordered_set<std::uint64_t> seen;

		for(const auto &s:next)
		{
			const auto key=(std::uint64_t(s.mask)<<5)|std::uint64_t(s.end);
			if(seen.insert(key).second) beam.push_back(s);
			if(beam.size()==128) break;
		}
	}
	if(best<0 || target==f.position) return result;
	const int dir=initial.first[index(target)];
	if(dir<0) return result;
	result.action["action"]="move";
	result.action["direction"]=DIRECTION[dir];
	result.target=target;
	result.reason=explore?"explore unknown area":"collect planned resource route";
	return result;
}

std::optional<Decision> Collector::update(const Frame &f)
{
	if(f.session!=session_)
	{
		if(retired_.count(f.session)) return std::nullopt;
		reset(f);
	}
	if(f.width!=width_ || f.height!=height_ || f.tick<last_tick_ || f.tick>=total_ticks_) return std::nullopt;
	if(have_cached_ && f.tick==last_tick_)
	{
		Decision d=cached_;
		d.replayed=true;
		return d;
	}
	observe(f);
	cached_=plan(f);
	last_tick_=f.tick;
	have_cached_=true;
	previous_position_=f.position;
	previous_target_=cached_.target;
	return cached_;
}
}
