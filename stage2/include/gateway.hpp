#pragma once
#include "battle.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace battle
{
struct AiJob
{
	std::string session,team;
	int tick=0,width=0,height=0;
	std::vector<std::string> lines;
	Json::Value memo,missions;
};

struct AiResult
{
	AiJob job;
	std::string content,error_code;
	long http=0;
	bool success=false;
	int elapsed_ms=0;
	Json::Value usage;
};

struct AiOptions
{
	std::string url,token,model="deepseek-flash",effort;
	int timeout_ms=10000,max_tokens=512;
	bool json_mode=true,thinking=false;
};

class Gateway
{
public:
	explicit Gateway(AiOptions options);
	~Gateway();
	bool submit(AiJob job);
	std::optional<AiResult> poll();
	void reset();

	bool enabled() const
	{
		return !options_.url.empty() && !options_.token.empty();
	}

private:
	AiOptions options_;
	std::mutex mutex_;
	std::condition_variable ready_;
	std::thread thread_;
	std::optional<AiJob> job_;
	std::optional<AiResult> result_;
	std::atomic<bool> stopping_{false};
	std::atomic<unsigned> generation_{0};
	bool busy_=false;
	void run();
	AiResult request(const AiJob &job,unsigned generation);
};
}
