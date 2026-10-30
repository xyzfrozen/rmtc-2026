#pragma once
#include <json/json.h>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace rmtc
{
struct Point
{
	int x=0,y=0;
	bool operator==(Point p) const
	{
		return x==p.x && y==p.y;
	}

	bool operator!=(Point p) const
	{
		return !(*this==p);
	}
};

struct Entity
{
	std::string type,id;
	Point position;
	int value=0;
};

struct Frame
{
	std::int64_t tick=0;
	std::string session;
	Point position;
	int width=0,height=0,radius=4,resource_value=0,invalid_actions=0;
	bool alive=true;
	std::vector<Entity> entities;
	Json::Value last_action;
};

bool parse_frame(const std::string &text,const std::string &team,Frame &out,std::string &error);
std::string compact_json(const Json::Value &value);

struct Decision
{
	Json::Value action;
	Point target;
	std::string reason;
	int observed_cells=0,known_resources=0;
	bool replayed=false;
};

class Collector
{
public:
	explicit Collector(int total_ticks=100);
	std::optional<Decision> update(const Frame &frame);

private:
	struct Resource
	{
		Point position;
		int value;
	};

	struct Search
	{
		std::vector<int> distance,first;
	};

	int total_ticks_,width_=0,height_=0,radius_=4,observed_=0;
	std::int64_t last_tick_=-1;
	std::string session_;
	std::vector<unsigned char> cells_;
	std::vector<bool> occupied_;
	std::map<std::string,Resource> resources_;
	std::unordered_set<std::string> retired_;
	std::deque<std::string> retired_order_;
	Point previous_position_,previous_target_;
	Decision cached_;
	bool have_cached_=false;

	int index(Point p) const
	{
		return p.y*width_+p.x;
	}

	Point point(int i) const
	{
		return {i%width_,i/width_};
	}

	bool inside(Point p) const
	{
		return p.x>=0 && p.y>=0 && p.x<width_ && p.y<height_;
	}

	void reset(const Frame &frame);
	void observe(const Frame &frame);
	int unseen(Point p) const;
	Search search(Point start,bool explore=false) const;
	Decision plan(const Frame &frame) const;
};
}
