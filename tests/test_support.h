#pragma once
#include "../modules/model.h"
#include "../modules/discord.h"
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <atomic>
using namespace music_tracker;
inline void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class Exception,class F> void throws(F f,const char* message){try{f();}catch(const Exception&){return;}throw std::runtime_error(message);}
struct Suite {
    int failures=0,total=0;
    void test(const char* name,const std::function<void()>& f){++total;try{f();std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++failures;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}}
    int finish(){std::cout<<total-failures<<'/'<<total<<" passed\n";return failures?1:0;}
};
struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory(){static std::atomic<unsigned> seq=0;path=std::filesystem::temp_directory_path()/("dmt-test-"+std::to_string(process_id())+"-"+std::to_string(++seq));std::filesystem::create_directories(path);}
    ~TemporaryDirectory(){std::error_code e;std::filesystem::remove_all(path,e);}
};
inline std::string frame(std::uint32_t opcode,const json& value){auto body=value.dump();std::string out;for(auto n:{opcode,static_cast<std::uint32_t>(body.size())})for(unsigned i=0;i<4;++i)out+=static_cast<char>((n>>(8*i))&255);return out+body;}
inline std::uint32_t u32(const char* p){std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=static_cast<std::uint32_t>(static_cast<unsigned char>(p[i]))<<(8*i);return n;}
inline json unframe(const std::string& value){require(value.size()>=8,"Frame has no header");return json::parse(value.substr(8));}
struct MockWire {
    std::mutex mutex;
    std::string incoming;
    std::vector<std::string> outgoing;
    unsigned dials=0;
    bool fail_send=false,fail_receive=false,auto_ready=false;
    void inject(std::string bytes){std::lock_guard lock(mutex);incoming+=bytes;}
    std::vector<std::string> frames(){std::lock_guard lock(mutex);return outgoing;}
    std::vector<json> activities(){std::lock_guard lock(mutex);std::vector<json> out;for(const auto& value:outgoing)if(u32(value.data())==1){auto j=unframe(value);if(j.value("cmd",std::string{})=="SET_ACTIVITY")out.push_back(j["args"]["activity"]);}return out;}
};
struct MockTransport final : DiscordTransport {
    explicit MockTransport(std::shared_ptr<MockWire> wire):wire(std::move(wire)){}
    std::shared_ptr<MockWire> wire;
    void send(const std::string& bytes) override {
        std::lock_guard lock(wire->mutex);if(wire->fail_send)throw std::runtime_error("Mock write failure");wire->outgoing.push_back(bytes);
        if(wire->auto_ready&&u32(bytes.data())==0)wire->incoming+=frame(1,{{"evt","READY"}});
    }
    std::string receive_available() override {std::lock_guard lock(wire->mutex);if(wire->fail_receive)throw std::runtime_error("Mock disconnect");auto out=std::move(wire->incoming);wire->incoming.clear();return out;}
};
inline DiscordRPC::Dial mock_dial(std::shared_ptr<MockWire> wire){return [wire]{std::lock_guard lock(wire->mutex);++wire->dials;return std::make_unique<MockTransport>(wire);};}
inline void eventually(const std::function<bool()>& f,int milliseconds=3000){auto until=MonoClock::now()+std::chrono::milliseconds(milliseconds);while(MonoClock::now()<until){if(f())return;std::this_thread::sleep_for(std::chrono::milliseconds(10));}require(false,"Timed out waiting for expected behavior");}
