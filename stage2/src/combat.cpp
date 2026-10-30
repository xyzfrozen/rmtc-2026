#include "battle.hpp"
#include <algorithm>

namespace battle
{
Reward damage_reward(double hp,const std::vector<Hit> &hits,const std::string &team)
{
	Reward out;
	double total=0,attack=0;
	std::set<std::string> contributors;

	if(hp<=0) return out;
	for(const auto &hit:hits)
		if(hit.damage>0)
		{
			total+=hit.damage;
			contributors.insert(hit.team);
			if(hit.team==team && hit.attack) attack+=hit.damage;
		}
	if(total<=0) return out;
	out.attack=attack*std::min(1.0,hp/total);
	if(total>=hp && contributors.count(team)) out.kill=KILL_BONUS/contributors.size();
	return out;
}

double projected_damage(double current,double gain,int deaths)
{
	return std::max(0.0,current+gain-DEATH_PENALTY*deaths);
}

double combat_points(double damage)
{
	return 15*std::clamp(damage/DAMAGE_CAP,0.0,1.0);
}
}
