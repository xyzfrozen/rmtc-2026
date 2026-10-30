#include "gateway.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace
{
std::string env(const char *name)
{
	const char *s=std::getenv(name);
	return s?s:"";
}
}

class BattleNode : public rclcpp::Node
{
public:
	BattleNode() : Node("rmtc_stage2_battle")
	{
		team_=declare_parameter<std::string>("team","A");
		const auto name=declare_parameter<std::string>("team_name","对抗程序");
		total_=declare_parameter<int>("total_ticks",500);
		max_calls_=declare_parameter<int>("ai_max_calls",80);
		if(team_.size()!=1 || std::string("ABCD").find(team_)==std::string::npos || total_<1 || total_>5000 ||
		    max_calls_<0 || max_calls_>1000)
			throw std::invalid_argument("invalid team/ticks/call limit");
		if(name.empty() || name.size()>96 || name.find_first_of("\r\n")!=std::string::npos)
			throw std::invalid_argument("invalid team_name");
		bot_=std::make_unique<battle::Bot>(total_);
		battle::AiOptions options;
		options.url=declare_parameter<std::string>("ai_gateway",env("RMTC_AI_GATEWAY"));
		options.token=env("RMTC_TEAM_TOKEN");
		options.model=declare_parameter<std::string>("ai_model","deepseek-flash");
		options.effort=declare_parameter<std::string>("ai_effort",env("RMTC_AI_EFFORT"));
		options.thinking=declare_parameter<bool>("ai_thinking",false);
		options.timeout_ms=declare_parameter<int>("ai_timeout_ms",10000);
		options.max_tokens=declare_parameter<int>("ai_max_tokens",512);
		options.json_mode=declare_parameter<bool>("ai_json_mode",true);
		gateway_=std::make_unique<battle::Gateway>(options);
		const auto path=declare_parameter<std::string>("record_path","");
		if(!path.empty())
		{
			const auto parent=std::filesystem::path(path).parent_path();
			if(!parent.empty())
				std::filesystem::create_directories(parent);
			log_.open(path,std::ios::app);
			if(!log_) throw std::runtime_error("cannot open record_path");
		}
		const auto qos=rclcpp::QoS(1).reliable().durability_volatile();
		const auto prefix="/team_"+team_;
		action_=create_publisher<std_msgs::msg::String>(prefix+"/action",qos);
		registration_=create_publisher<std_msgs::msg::String>(prefix+"/registration",rclcpp::QoS(
			1).reliable().transient_local());
		perception_=
		    create_subscription<std_msgs::msg::String>(prefix+"/perception",qos,
			[this](std_msgs::msg::String::ConstSharedPtr message)
		{
			receive(message->data);
		});
		timer_=create_wall_timer(std::chrono::milliseconds(50),[this]
		{
			poll_ai();
		});
		Json::Value reg;
		reg["team"]=team_;
		reg["name"]=name;
		std_msgs::msg::String message;
		message.data=rmtc::compact_json(reg);
		registration_->publish(message);
		RCLCPP_INFO(get_logger(),"Ready: team=%s ticks=%d AI=%s",team_.c_str(),total_,
			gateway_->enabled()?"configured":"disabled");
	}

private:
	void record(const Json::Value &v)
	{
		if(log_)
		{
			log_<<rmtc::compact_json(v)<<'\n';
			log_.flush();
		}
	}

	void poll_ai()
	{
		auto result=gateway_->poll();
		if(!result || result->job.session!=session_) return;
		std::string error;
		const bool accepted=result->success && bot_->accept_events(session_,result->content,error);
		http_succeeded_|=result->http>=200 && result->http<300;
		Json::Value row;
		row["kind"]="ai";
		row["session_id"]=session_;
		row["request_tick"]=result->job.tick;
		row["received_tick"]=tick_;
		row["http"]=Json::Int64(result->http);
		row["elapsed_ms"]=result->elapsed_ms;
		row["usage"]=result->usage;
		row["accepted"]=accepted;
		row["http_success"]=result->http>=200 && result->http<300;
		row["error_code"]=result->error_code;
		row["validation_error"]=error;
		row["content"]=result->content;
		row["announcements"]=Json::Value(Json::arrayValue);

		for(const auto &s:result->job.lines)
			row["announcements"].append(s);
		record(row);
		if(accepted)
		{
			succeeded_=true;
			failures_=0;

			for(std::size_t i=0;i<result->job.lines.size() && !pending_.empty();++i)
				pending_.pop_front();
			RCLCPP_INFO(
				get_logger(),"AI interpretation accepted: tick=%d events=%u latency_ms=%d",tick_,
				bot_->event_memo().size(),result->elapsed_ms);
		}
		else
		{
			++failures_;
			const int delay=result->error_code=="timeout"?6:std::min(8,1<<std::min(failures_,3));
			// Invalid event JSON can be retried before a short announcement window closes.
			const int retry_ms=result->success?std::min(1000,250*failures_):delay*1000;
			retry_after_=std::chrono::steady_clock::now()+std::chrono::milliseconds(retry_ms);
			if(result->http==401 || result->error_code=="quota_exceeded" || result->error_code=="audit_unavailable")
				ai_stopped_=true;
			if(failures_>=3)
			{
				for(std::size_t i=0;i<result->job.lines.size() && !pending_.empty();++i)
					pending_.pop_front();
				failures_=0;
			}
			RCLCPP_WARN(
				get_logger(),"AI unavailable/invalid: http=%ld code=%s; local control continues",result->http,
				result->error_code.c_str());
		}
	}

	void receive(const std::string &text)
	{
		const auto start=std::chrono::steady_clock::now();
		battle::Frame f;
		std::string error;
		if(!battle::parse_frame(text,team_,f,error))
		{
			RCLCPP_WARN_THROTTLE(get_logger(),*get_clock(),2000,"Discard malformed perception: %s",error.c_str());
			return;
		}
		auto decision=bot_->update(f);
		if(!decision)
		{
			if(f.tick==total_ && f.session==session_ && !finished_)
			{
				finished_=true;
				Json::Value row;
				row["kind"]="finished";
				row["session_id"]=session_;
				row["tick"]=int(f.tick);
				row["resource_value"]=f.resource_value;
				row["effective_damage"]=f.damage;
				row["completed"]=f.completed;
				row["invalid_actions"]=f.invalid_actions;
				row["last_action"]=f.last_action;
				row["ai_accepted"]=succeeded_;
				row["ai_http_success"]=http_succeeded_;
				record(row);
				RCLCPP_INFO(
					get_logger(),"Finished: resource=%d damage=%.2f missions=%u invalid=%d",f.resource_value,f.damage,
					f.completed.size(),f.invalid_actions);
			}
			return;
		}
		if(f.session!=session_)
		{
			session_=f.session;
			gateway_->reset();
			seen_.clear();
			pending_.clear();
			calls_=failures_=0;
			retry_after_={};
			succeeded_=http_succeeded_=ai_stopped_=finished_=false;
		}
		tick_=f.tick;
		if(decision->replayed && start-last_publish_<std::chrono::milliseconds(20)) return;
		std_msgs::msg::String message;
		message.data=rmtc::compact_json(decision->action);
		action_->publish(message);
		last_publish_=std::chrono::steady_clock::now();
		Json::Value row;
		row["kind"]="decision";
		row["session_id"]=session_;
		row["tick"]=tick_;
		row["action"]=decision->action;
		row["position"].append(f.position.x);
		row["position"].append(f.position.y);
		row["hp"]=f.hp;
		row["alive"]=f.alive;
		row["protected"]=f.protected_;
		row["last_action"]=f.last_action;
		row["resource_value"]=f.resource_value;
		row["effective_damage"]=f.damage;
		row["completed"]=f.completed;
		row["invalid_actions"]=f.invalid_actions;
		row["reason"]=decision->reason;
		row["incoming"]=decision->incoming;
		row["events"]=decision->event_count;
		row["replayed"]=decision->replayed;
		row["decision_us"]=Json::Int64(std::chrono::duration_cast<std::chrono::microseconds>(
			last_publish_-start).count());
		record(row);
		std::istringstream lines(f.event_text);
		std::string line;

		// Transport-level exact deduplication only; all event meaning is supplied by the LLM.
		while(std::getline(lines,line))
			if(!line.empty() && seen_.size()<4096 && seen_.insert(line).second)
			{
				if(line.size()<=12000) pending_.push_back("[seen at tick "+std::to_string(tick_)+"] "+line);
				else
					RCLCPP_WARN(get_logger(),"Oversized announcement skipped: %zu bytes",line.size());
			}
		if(!ai_stopped_ && calls_<max_calls_ && std::chrono::steady_clock::now()>=retry_after_ &&
		    (!succeeded_ || !pending_.empty()))
		{
			battle::AiJob job;
			job.session=session_;
			job.team=team_;
			job.tick=tick_;
			job.width=f.width;
			job.height=f.height;
			job.memo=bot_->event_memo();
			job.missions=f.missions;
			std::size_t bytes=0;

			for(const auto &s:pending_)
			{
				if(job.lines.size()>=4 || (!job.lines.empty() && bytes+s.size()>3000)) break;
				job.lines.push_back(s);
				bytes+=s.size();
			}
			if(gateway_->submit(std::move(job))) ++calls_;
		}
		if(!decision->replayed && tick_%50==0)
			RCLCPP_INFO(
				get_logger(),"tick=%d hp=%d resource=%d damage=%.1f action=%s events=%d",tick_,f.hp,f.resource_value,f.damage,
				decision->action["action"].asCString(),decision->event_count);
	}

	std::string team_,session_;
	int total_=500,tick_=0,max_calls_=80,calls_=0,failures_=0;
	bool succeeded_=false,http_succeeded_=false,ai_stopped_=false,finished_=false;
	std::unique_ptr<battle::Bot> bot_;
	std::unique_ptr<battle::Gateway> gateway_;
	std::set<std::string> seen_;
	std::deque<std::string> pending_;
	std::ofstream log_;
	std::chrono::steady_clock::time_point last_publish_{},retry_after_{};
	rclcpp::Publisher<std_msgs::msg::String>::SharedPtr action_,registration_;
	rclcpp::Subscription<std_msgs::msg::String>::SharedPtr perception_;
	rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc,char * *argv)
{
	rclcpp::init(argc,argv);
	try
	{
		rclcpp::spin(std::make_shared<BattleNode>());
	}
	catch(const std::exception &e)
	{
		RCLCPP_FATAL(rclcpp::get_logger("rmtc_stage2"),"%s",e.what());
		rclcpp::shutdown();
		return 1;
	}
	rclcpp::shutdown();
	return 0;
}
