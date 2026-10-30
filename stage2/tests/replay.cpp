#include "battle.hpp"
#include <iostream>
#include <string>

int main(int argc,char **argv)
{
	if(argc!=2) return 2;
	battle::Bot bot;
	std::string line,error;
	while(std::getline(std::cin,line))
	{
		battle::Frame frame;
		if(!battle::parse_frame(line,argv[1],frame,error)) return 3;
		if(auto d=bot.update(frame)) std::cout<<rmtc::compact_json(d->action)<<'\n';
	}
	return 0;
}
