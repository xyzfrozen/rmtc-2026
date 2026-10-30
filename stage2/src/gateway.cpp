#include "gateway.hpp"
#include "event_prompt.hpp"
#include <curl/curl.h>
#include <chrono>
#include <stdexcept>

namespace battle
{
Gateway::Gateway(AiOptions options) : options_(std::move(options))
{
	if(options_.timeout_ms<100 || options_.timeout_ms>30000 || options_.max_tokens<128 || options_.max_tokens>8192)
		throw std::invalid_argument("invalid AI time/output limit");
	if(!options_.url.empty() && options_.url.rfind("http://",0)!=0 && options_.url.rfind("https://",0)!=0)
		throw std::invalid_argument("AI URL must use HTTP(S)");
	if(options_.token.find_first_of("\r\n")!=std::string::npos) throw std::invalid_argument("invalid token");
	curl_global_init(CURL_GLOBAL_DEFAULT);
	thread_=std::thread([this]
	{
		run();
	});
}

Gateway::~Gateway()
{
	stopping_=true;
	ready_.notify_one();
	thread_.join();
	curl_global_cleanup();
}

void Gateway::reset()
{
	++generation_;
	std::lock_guard<std::mutex> lock(mutex_);
	job_.reset();
	result_.reset();
}

bool Gateway::submit(AiJob job)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if(!enabled() || busy_ || job_ || result_) return false;
	job_=std::move(job);
	ready_.notify_one();
	return true;
}

std::optional<AiResult> Gateway::poll()
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto result=std::move(result_);
	result_.reset();
	return result;
}

void Gateway::run()
{
	while(!stopping_)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		ready_.wait(lock,[&]
		{
			return stopping_ || job_.has_value();
		});
		if(stopping_) break;
		auto job=std::move(*job_);
		job_.reset();
		busy_=true;
		const auto generation=generation_.load();
		lock.unlock();
		auto result=request(job,generation);
		lock.lock();
		busy_=false;
		if(generation==generation_) result_=std::move(result);
	}
}

AiResult Gateway::request(const AiJob &job,unsigned generation)
{
	AiResult result;
	result.job=job;
	const auto start=std::chrono::steady_clock::now();
	std::string input;

	for(const auto &line:job.lines)
		input+=line+'\n';
	if(input.empty()) input="No announcements.";
	input="Context: team="+job.team+", tick="+std::to_string(job.tick)+", map="+
	    std::to_string(job.width)+"x"+std::to_string(job.height)+".\nNotices:\n"+input;
	Json::Value known(Json::arrayValue);

	for(const auto &e:job.memo)
		if(e["end"].asInt()+20>job.tick || e["start"].asInt()+e.get("effect_ticks",0).asInt()>job.tick)
			known.append(e);
	if(!known.empty()) input+="\nPreviously interpreted events: "+rmtc::compact_json(known);
	Json::Value body,system,user;
	system["role"]="system";
	system["content"]=EVENT_PROMPT;
	user["role"]="user";
	user["content"]=input;
	body["messages"].append(system);
	body["messages"].append(user);
	body["stream"]=false;
	body["thinking"]["type"]=options_.thinking?"enabled":"disabled";
	body["max_tokens"]=options_.max_tokens;
	if(options_.json_mode) body["response_format"]["type"]="json_object";
	body["user"]="rmtc-stage2-"+job.team;
	if(!options_.model.empty()) body["model"]=options_.model;
	if(options_.thinking && !options_.effort.empty()) body["reasoning_effort"]=options_.effort;
	Json::StreamWriterBuilder writer;
	writer["indentation"]="";
	writer["emitUTF8"]=true;
	const auto payload=Json::writeString(writer,body);
	std::string response;
	CURL *curl=curl_easy_init();
	if(!curl)
	{
		result.error_code="client_init";
		return result;
	}
	curl_slist *headers=nullptr;
	headers=curl_slist_append(headers,"Content-Type: application/json");
	headers=curl_slist_append(headers,("Authorization: Bearer "+options_.token).c_str());
	curl_easy_setopt(curl,CURLOPT_URL,options_.url.c_str());
	curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
	// The match gateway is reached directly, including on the venue LAN.
	curl_easy_setopt(curl,CURLOPT_PROXY,"");
	curl_easy_setopt(curl,CURLOPT_POSTFIELDS,payload.data());
	curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,long(payload.size()));
	curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
	curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
	curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT_MS,2000L);
	curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,long(options_.timeout_ms));
	curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,+[] (char *data,size_t size,size_t count,void *context)->size_t
		{
			auto &s=*static_cast<std::string *>(context);
			const auto n=size*count;
			if(s.size()+n>1024*1024) return 0;
			s.append(data,n);
			return n;
		});
	curl_easy_setopt(curl,CURLOPT_WRITEDATA,&response);

	struct Cancellation
	{
		Gateway *self;
		unsigned generation;
	} cancellation{this,generation};
	curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
	curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,+[] (void *context,curl_off_t,curl_off_t,curl_off_t,curl_off_t)->int
		{
			auto &c=*static_cast<Cancellation *>(context);
			return c.self->stopping_ || c.self->generation_!=c.generation;
		});
	curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&cancellation);
	const auto code=curl_easy_perform(curl);
	curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&result.http);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	result.elapsed_ms=
	    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
	if(code!=CURLE_OK)
	{
		result.error_code=code==CURLE_OPERATION_TIMEDOUT?"timeout":"network_or_cancelled";
		return result;
	}
	try
	{
		Json::Value v;
		if(!read_json(response,v) || !v.isObject())
		{
			result.error_code="invalid_response";
			return result;
		}
		result.usage=v["usage"];
		if(result.http<200 || result.http>=300)
		{
			result.error_code=v["error"].isObject() &&
			    v["error"]["code"].isString()?v["error"]["code"].asString():"http_error";
			return result;
		}
		if(!v["choices"].isArray() || v["choices"].empty() || !v["choices"][0]["message"]["content"].isString())
		{
			result.error_code="missing_content";
			return result;
		}
		if(v["choices"][0]["finish_reason"]=="length")
		{
			result.error_code="truncated_output";
			return result;
		}
		result.content=v["choices"][0]["message"]["content"].asString();
		result.success=true;
	}
	catch(const std::exception &)
	{
		result.error_code="response_type_error";
	}
	return result;
}
}
