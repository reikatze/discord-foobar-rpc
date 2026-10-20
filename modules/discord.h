#pragma once
#include "model.h"
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
    std::string encoded="null";
    PreparedActivity()=default;
    explicit PreparedActivity(json activity):value(std::move(activity)),encoded(value.dump()){}
    void replace(json activity){if(activity!=value){value=std::move(activity);encoded=value.dump();}}
};
class DiscordRPC {
public:
    using Dial = std::function<std::unique_ptr<DiscordTransport>()>;
    using Now = std::function<MonoClock::time_point()>;
    explicit DiscordRPC(Dial dial={},Now now={}):dial_(std::move(dial)),now_(std::move(now)){}
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
    std::unique_ptr<DiscordTransport> connection_;
    std::string application_id_,input_,last_,status_="Waiting for Discord";
    bool ready_=false;
    unsigned retry_seconds_=1;
    std::uint64_t nonce_=0;
    MonoClock::time_point next_{},handshake_deadline_{},last_sent_{};
};
}
