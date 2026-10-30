#include "rmtc_stage1/collector.hpp"
#include <iostream>

int main(int argc,char * *argv)
{
	try
	{
		rmtc::Collector collector(argc>1?std::stoi(argv[1]):100);
		std::string line;

		while(std::getline(std::cin,line))
		{
			rmtc::Frame frame;
			std::string error;
			if(!rmtc::parse_frame(line,"A",frame,error))
			{
				std::cerr<<error<<'\n';
				std::cout<<"null"<<std::endl;
				continue;
			}
			auto d=collector.update(frame);
			std::cout<<(d?rmtc::compact_json(d->action):"null")<<std::endl;
		}
	}
	catch(const std::exception &e)
	{
		std::cerr<<e.what()<<'\n';
		return 1;
	}
}
