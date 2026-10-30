#include "rmtc_stage1/collector.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

class CollectorNode : public rclcpp::Node
{
public:
	CollectorNode() : Node("rmtc_stage1_collector")
	{
		team_=declare_parameter<std::string>("team","A");
		const auto name=declare_parameter<std::string>("team_name","采集程序");
		const auto record=declare_parameter<std::string>("record_path","");
		const int ticks=declare_parameter<int>("total_ticks",100);
		total_ticks_=ticks;
		if(team_.size()!=1 || std::string("ABCD").find(team_)==std::string::npos)
			throw std::invalid_argument("team must be A/B/C/D");
		if(name.empty() || name.size()>96 || name.find_first_of("\r\n")!=std::string::npos)
			throw std::invalid_argument("invalid team_name");
		collector_=std::make_unique<rmtc::Collector>(ticks);
		if(!record.empty())
		{
			const auto parent=std::filesystem::path(record).parent_path();
			if(!parent.empty()) std::filesystem::create_directories(parent);
			record_.open(record,std::ios::app);
			if(!record_) throw std::runtime_error("cannot open record_path");
		}
		const auto qos=rclcpp::QoS(rclcpp::KeepLast(1)).reliable().durability_volatile();
		const auto prefix="/team_"+team_;
		action_=create_publisher<std_msgs::msg::String>(prefix+"/action",qos);
		registration_=create_publisher<std_msgs::msg::String>(prefix+"/registration",rclcpp::QoS(
			1).reliable().transient_local());
		perception_=
		    create_subscription<std_msgs::msg::String>(prefix+"/perception",qos,
			[this](std_msgs::msg::String::ConstSharedPtr msg)
		{
			receive(msg->data);
		});
		Json::Value registration;
		registration["team"]=team_;
		registration["name"]=name;
		std_msgs::msg::String message;
		message.data=rmtc::compact_json(registration);
		registration_->publish(message);
		RCLCPP_INFO(get_logger(),"Ready: team=%s total_ticks=%d; stage one, no AI/network client",team_.c_str(),ticks);
	}

private:
	void receive(const std::string &text)
	{
		const auto start=std::chrono::steady_clock::now();
		rmtc::Frame f;
		std::string error;
		if(!rmtc::parse_frame(text,team_,f,error))
		{
			RCLCPP_WARN_THROTTLE(get_logger(),*get_clock(),2000,"Discard perception: %s",error.c_str());
			return;
		}
		auto d=collector_->update(f);
		if(!d)
		{
			if(f.tick==total_ticks_ && f.session==last_session_ && f.session!=finished_session_)
			{
				finished_session_=f.session;
				RCLCPP_INFO(get_logger(),"Finished: session=%s resource=%d invalid_actions=%d",
					f.session.c_str(),f.resource_value,f.invalid_actions);
				if(record_)
				{
					Json::Value row;
					row["session_id"]=f.session;
					row["tick"]=Json::Int64(f.tick);
					row["finished"]=true;
					row["resource_value"]=f.resource_value;
					row["invalid_actions"]=f.invalid_actions;
					row["last_action"]=f.last_action;
					record_<<rmtc::compact_json(row)<<'\n';
					record_.flush();
				}
			}
			return;
		}
		if(d->replayed && start-last_publish_<std::chrono::milliseconds(20)) return;
		std_msgs::msg::String action;
		action.data=rmtc::compact_json(d->action);
		action_->publish(action);
		last_publish_=std::chrono::steady_clock::now();
		const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(last_publish_-start).count();
		if(record_)
		{
			Json::Value row;
			row["session_id"]=f.session;
			row["tick"]=Json::Int64(f.tick);
			row["position"].append(f.position.x);
			row["position"].append(f.position.y);
			row["target"].append(d->target.x);
			row["target"].append(d->target.y);
			row["resource_value"]=f.resource_value;
			row["invalid_actions"]=f.invalid_actions;
			row["last_action"]=f.last_action;
			row["action"]=d->action;
			row["reason"]=d->reason;
			row["observed_cells"]=d->observed_cells;
			row["known_resources"]=d->known_resources;
			row["decision_us"]=Json::Int64(elapsed);
			row["replayed"]=d->replayed;
			record_<<rmtc::compact_json(row)<<'\n';
			record_.flush();
			if(!record_)
				RCLCPP_ERROR_THROTTLE(get_logger(),*get_clock(),5000,
					"Decision log write failed; control remains active");
		}
		if(!d->replayed && (f.tick%10==0 || f.resource_value!=last_score_ || f.session!=last_session_))
			RCLCPP_INFO(
				get_logger(),"session=%s tick=%ld pos=[%d,%d] resource=%d action=%s direction=%s decision_us=%ld",
				f.session.c_str(),static_cast<long>(f.tick),f.position.x,f.position.y,f.resource_value,
				d->action["action"].asCString(),d->action.get("direction","-").asCString(),static_cast<long>(elapsed));
		last_score_=f.resource_value;
		last_session_=f.session;
	}

	std::string team_,last_session_,finished_session_;
	int last_score_=-1,total_ticks_=100;
	std::unique_ptr<rmtc::Collector> collector_;
	std::ofstream record_;
	std::chrono::steady_clock::time_point last_publish_{};
	rclcpp::Publisher<std_msgs::msg::String>::SharedPtr action_,registration_;
	rclcpp::Subscription<std_msgs::msg::String>::SharedPtr perception_;
};

int main(int argc,char * *argv)
{
	rclcpp::init(argc,argv);
	try
	{
		rclcpp::spin(std::make_shared<CollectorNode>());
	}
	catch(const std::exception &e)
	{
		RCLCPP_FATAL(rclcpp::get_logger("rmtc_stage1"),"%s",e.what());
		rclcpp::shutdown();
		return 1;
	}
	rclcpp::shutdown();
	return 0;
}
