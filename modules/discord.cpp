#include "discord.h"
#include <stdexcept>
#include <algorithm>
namespace music_tracker {
namespace {
void append_u32(std::string& out,std::uint32_t value) {for(unsigned i=0;i<4;++i)out+=static_cast<char>((value>>(i*8))&255);}
std::uint32_t read_u32(const char* p) {std::uint32_t out=0;for(unsigned i=0;i<4;++i)out|=static_cast<std::uint32_t>(static_cast<unsigned char>(p[i]))<<(8*i);return out;}
}
void DiscordRPC::send(std::uint32_t opcode,const json& value) {
    send_encoded(opcode,value.dump());
}
void DiscordRPC::send_encoded(std::uint32_t opcode,const std::string& body) {
    if(body.size()>1024*1024)throw std::runtime_error("Discord frame too large");
    std::string frame;frame.reserve(body.size()+8);append_u32(frame,opcode);append_u32(frame,static_cast<std::uint32_t>(body.size()));frame+=body;
    connection_->send(frame);
}
void DiscordRPC::send_activity(const std::string& encoded) {
    diagnostics_.detail("Discord","SET_ACTIVITY nonce="+std::to_string(nonce_+1)+(encoded=="null"?" clear":" update"));
    send_encoded(1,"{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(process_id())+",\"activity\":"+encoded+"},\"nonce\":\""+std::to_string(++nonce_)+"\"}");
}
void DiscordRPC::receive() {
    input_+=connection_->receive_available();
    if(input_.size()>2*1024*1024)throw std::runtime_error("Discord receive buffer too large");
    for(unsigned count=0;count<32&&input_.size()>=8;++count) {
        const auto opcode=read_u32(input_.data()),size=read_u32(input_.data()+4);
        if(size>1024*1024)throw std::runtime_error("Discord frame too large");
        if(input_.size()<size+8)return; // Keep fragmented headers/bodies across ticks.
        const auto body=input_.substr(8,size);input_.erase(0,size+8);
        auto value=body.empty()?json::object():json::parse(body);
        if(opcode==2)throw std::runtime_error("Discord disconnected");
        if(opcode==3){send(4,value);continue;}
        if(!value.is_object())throw std::runtime_error("Invalid Discord frame");
        const auto event=value.contains("evt")&&value["evt"].is_string()?value["evt"].get<std::string>():std::string{};
        if(event=="ERROR") {
            std::string reason="Discord rejected presence";
            if(value.contains("data")&&value["data"].is_object()) {
                const auto& data=value["data"];
                if(data.contains("code"))reason+=" code="+data["code"].dump();
                if(data.contains("message")&&data["message"].is_string())reason+=": "+data["message"].get<std::string>();
            }
            throw std::runtime_error(reason);
        }
        if(event.empty()&&value.value("cmd",std::string{})=="SET_ACTIVITY")diagnostics_.detail("Discord","SET_ACTIVITY acknowledged"+(value.contains("nonce")?" nonce="+value["nonce"].dump():""));
        if(event=="READY"){diagnostics_.info("Discord","Connected (READY)");ready_=true;retry_seconds_=1;last_.clear();last_content_.clear();status_="Connected";}
    }
}
void DiscordRPC::disconnect(const std::string& status) {
    diagnostics_.info("Discord",status+"; reconnect in "+std::to_string(retry_seconds_)+" seconds");
    connection_.reset();ready_=false;input_.clear();last_.clear();last_content_.clear();status_=status;
    next_=now()+std::chrono::seconds(retry_seconds_);retry_seconds_=std::min(retry_seconds_*2,60U);
}
void DiscordRPC::tick(const std::string& application_id,const json& activity) {tick(application_id,PreparedActivity(activity));}
void DiscordRPC::tick(const std::string& application_id,const PreparedActivity& activity) {
    try {
        // Stop/disable must bypass queued replies, errors, throttling and backoff.
        // Closing the IPC session also withdraws its presence if clear is rejected.
        if(activity.value.is_null()) {
            shutdown();next_={};retry_seconds_=1;status_="Stopped — presence cleared";return;
        }
        if(application_id_!=application_id) {
            shutdown();application_id_=application_id;next_={};retry_seconds_=1;
        }
        if(!valid_application_id(application_id_)||application_id_.empty()){status_="Invalid custom application ID";return;}
        auto current=now();
        if(!connection_) {
            if(current<next_)return;
            connection_=dial_?dial_():connect_discord();
            if(!connection_)throw std::runtime_error("Discord transport unavailable");
            ready_=false;input_.clear();
            send(0,{{"v",1},{"client_id",application_id_}});handshake_deadline_=current+std::chrono::seconds(2);
        }
        receive();
        if(!ready_) {if(now()>handshake_deadline_)throw std::runtime_error("Discord handshake timed out");return;}
        const auto& encoded=activity.encoded;
        bool changed=activity.content_key!=last_content_;
        // Coalesce meaningful changes; hover-clock changes ride the periodic replay.
        // Stop and disable were handled before receiving any queued replies.
        if(last_.empty()||((changed||current-last_sent_>=std::chrono::seconds(30))&&current-last_sent_>=std::chrono::seconds(1))) {
            diagnostics_.detail("Discord","Sending "+activity.value.value("details",std::string{})+" / "+activity.value.value("state",std::string{})+
                "; artwork="+(activity.value.contains("assets")?activity.value["assets"].value("large_image",std::string{"none"}):"none"));
            send_activity(encoded);
            last_=encoded;last_content_=activity.content_key;last_sent_=current;
        }
        status_="Connected — sharing music";
    } catch(const std::exception& e) {disconnect(e.what());}
}
void DiscordRPC::shutdown() noexcept {
    try {if(connection_&&ready_){send_activity("null");diagnostics_.info("Discord","Presence cleared; IPC session closed");}}catch(const std::exception& error){diagnostics_.info("Discord","Clear failed; closing IPC: "+std::string(error.what()));}catch(...){}
    connection_.reset();ready_=false;input_.clear();last_.clear();last_content_.clear();
}
}
