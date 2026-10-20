#pragma once
#include "model.h"
#include "diagnostics.h"
#include <memory>
#include <functional>
namespace music_tracker {
class DiscordTransport {
public:
    virtual ~DiscordTransport() = default;
    virtual void send(const std::string& bytes)=0;
    virtual std::string receive_available()=0;
};
std::unique_ptr<DiscordTransport> connect_discord();
std::unique_ptr<DiscordTransport> connect_discord_at(const std::string& endpoint);
std::uint64_t process_id();
struct PreparedActivity {
    json value=nullptr;
    std::string encoded="null",content_key="null";
    PreparedActivity()=default;
    explicit PreparedActivity(json activity){replace(std::move(activity));}
    void replace(json activity){
        if(activity==value)return;
        value=std::move(activity);encoded=value.dump();
        auto content=value;
        if(content.is_object()&&content.contains("assets")&&content["assets"].is_object())content["assets"].erase("small_text");
        content_key=content.dump();
    }
};
class DiscordRPC {
public:
    using Dial = std::function<std::unique_ptr<DiscordTransport>()>;
    using Now = std::function<MonoClock::time_point()>;
    explicit DiscordRPC(Dial dial={},Now now={},Diagnostics diagnostics={}):dial_(std::move(dial)),now_(std::move(now)),diagnostics_(std::move(diagnostics)){}
    void tick(const std::string& application_id,const json& activity);
    void tick(const std::string& application_id,const PreparedActivity& activity);
    void shutdown() noexcept;
    const std::string& status() const {return status_;}
private:
    void send(std::uint32_t opcode,const json& value);
    void send_encoded(std::uint32_t opcode,const std::string& body);
    void send_activity(const std::string& encoded);
    void receive();
    void disconnect(const std::string& status);
    MonoClock::time_point now() const {return now_?now_():MonoClock::now();}
    Dial dial_;
    Now now_;
    Diagnostics diagnostics_;
    std::unique_ptr<DiscordTransport> connection_;
    std::string application_id_,input_,last_,last_content_,status_="Waiting for Discord";
    bool ready_=false;
    unsigned retry_seconds_=1;
    std::uint64_t nonce_=0;
    MonoClock::time_point next_{},handshake_deadline_{},last_sent_{};
};
}
