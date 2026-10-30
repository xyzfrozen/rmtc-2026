#pragma once
#include "rmtc_stage1/collector.hpp"
#include <array>
#include <set>

namespace battle
{
using rmtc::Point;
inline constexpr std::array<Point,4> DELTA={Point{0,-1},Point{1,0},Point{0,1},Point{-1,0}};
inline constexpr const char *DIR[]={"up","right","down","left"};
bool integer(const Json::Value &v,int low,int high);
bool read_json(const std::string &s,Json::Value &v);

struct Robot
{
	std::string team;
	Point position;
	int hp=100;
	bool defending=false,protected_=false;
};

struct Frame : rmtc::Frame
{
	std::string team,event_text;
	int hp=100;
	bool protected_=false,respawned=false;
	double damage=0;
	std::vector<Robot> enemies;
	Json::Value missions,completed;
};

bool parse_frame(const std::string &text,const std::string &team,Frame &out,std::string &error);

struct Event
{
	std::string kind;
	int start=0,end=0;
	Point lo{-1,-1},hi{-1,-1};
	std::vector<Point> points;
	std::set<std::string> targets;
	Json::Value data;

	bool active(int t) const
	{
		return start<=t && t<end;
	}

	bool contains(Point p) const
	{
		return p.x>=lo.x && p.y>=lo.y && p.x<=hi.x && p.y<=hi.y;
	}

	double number(const char *key,double fallback=0) const
	{
		return data.isMember(key)?data[key].asDouble():fallback;
	}
};

struct Effects
{
	bool inverted=false;
	int radius=0;
	double attack=20,defended=10,heal=0,multiplier=1;
};

inline constexpr double DAMAGE_CAP=500,DEATH_PENALTY=60,KILL_BONUS=60;

// 单个致死批次的预测输入；反伤、辣椒只分击杀奖，不计普通攻击贡献。
struct Hit
{
	std::string team;
	double damage;
	bool attack=true;
};

struct Reward
{
	double attack=0,kill=0;
};

Reward damage_reward(double hp,const std::vector<Hit> &hits,const std::string &team);
double projected_damage(double current,double gain,int deaths);
double combat_points(double damage);

class Events
{
public:
	bool merge(const std::string &text,int width,int height,std::string &error);
	Json::Value memo() const;
	Effects effects(const std::string &team,int tick,const std::map<int,Frame> &history,bool infer_self) const;
	bool wall(Point p,int tick) const;
	bool temporary_seen(Point p,int seen,int now) const;
	double hazard(Point p,int tick,bool protected_) const;
	double reflection(int tick) const;
	std::vector<Event> items;
};

struct Decision : rmtc::Decision
{
	double incoming=0,utility=0;
	int event_count=0;
	std::string resource_goal;
};

class Bot
{
public:
	explicit Bot(int ticks=500) : total_(ticks)
	{
	}

	std::optional<Decision> update(const Frame &f);
	bool accept_events(const std::string &session,const std::string &text,std::string &error);

	Json::Value event_memo() const
	{
		return events_.memo();
	}

	const std::string &session() const
	{
		return session_;
	}

private:
	struct Cell
	{
		int kind=0,seen=-1;
	};

	struct Memory
	{
		Robot robot;
		int tick;
	};

	int total_,width_=0,height_=0,last_tick_=-1;
	Point spawn_{-1,-1};
	std::string session_;
	std::deque<std::string> retired_;
	std::vector<Cell> cells_;
	std::vector<int> blocked_until_;
	std::map<std::string,rmtc::Entity> resources_;
	std::map<std::string,Memory> enemies_;
	std::map<int,Frame> history_;
	Events events_;
	Decision cached_;
	std::string resource_goal_;
	int goal_since_=-1;

	int index(Point p) const
	{
		return p.y*width_+p.x;
	}

	bool inside(Point p) const
	{
		return p.x>=0 && p.y>=0 && p.x<width_ && p.y<height_;
	}

	void observe(const Frame &f);
	Decision plan(const Frame &f) const;
};
}
