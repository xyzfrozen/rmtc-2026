#include "gateway.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <thread>

int main(int argc,char **argv)
{
	if(argc!=3) return 2;
	battle::AiOptions options;
	options.url=argv[1];
	options.timeout_ms=std::stoi(argv[2]);
	const char *token=std::getenv("RMTC_TEAM_TOKEN");
	options.token=token?token:"";
	Json::Value input;
	const std::string text{std::istreambuf_iterator<char>(std::cin),std::istreambuf_iterator<char>()};
	if(!battle::read_json(text,input)) return 2;
	battle::AiJob job;
	job.session=input.get("session","probe").asString();
	job.team=input.get("team","A").asString();
	job.tick=input.get("tick",0).asInt();
	job.width=input.get("width",24).asInt();
	job.height=input.get("height",24).asInt();
	job.memo=input.get("memo",Json::Value(Json::arrayValue));
	for(const auto &line:input["lines"]) job.lines.push_back(line.asString());
	battle::Gateway gateway(options);
	if(!gateway.submit(job)) return 3;
	if(input.get("cancel",false).asBool())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		gateway.reset();
		job.session="new-session";
		for(int i=0;!gateway.submit(job);++i)
		{
			if(i>options.timeout_ms/10+100) return 4;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
	for(int i=0;i<options.timeout_ms/10+200;++i)
	{
		if(auto r=gateway.poll())
		{
			Json::Value out;
			battle::Events events;
			std::string error;
			out["session"]=r->job.session;
			out["http"]=Json::Int64(r->http);
			out["http_success"]=r->http>=200 && r->http<300;
			out["response_valid"]=r->success;
			out["accepted"]=r->success && events.merge(r->content,job.width,job.height,error);
			out["elapsed_ms"]=r->elapsed_ms;
			out["content"]=r->content;
			out["error"]=r->error_code;
			out["validation_error"]=error;
			out["usage"]=r->usage;
			std::cout<<rmtc::compact_json(out)<<'\n';
			return 0;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return 5;
}
