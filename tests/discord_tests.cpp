#include "test_support.h"
int main(){Suite s;const std::string id="123456789012345678";const json a={{"type",2},{"details","Song"},{"state","Artist"}};
 s.test("Discord errors retain code and reason in normal diagnostics",[&]{
    std::vector<std::string> lines;Diagnostics log([&](const auto& line){lines.push_back(line);},[](auto task){task();});auto w=std::make_shared<MockWire>();w->auto_ready=true;DiscordRPC rpc(mock_dial(w),{},log);rpc.tick(id,a);
    w->inject(frame(1,{{"evt","ERROR"},{"data",{{"code",4000},{"message","invalid image"}}}}));rpc.tick(id,a);bool found=false;for(const auto& line:lines)if(line.find("4000")!=std::string::npos&&line.find("invalid image")!=std::string::npos)found=true;require(found,"Discord error diagnostic lacked reason");
 });
 s.test("clear precedes queued errors and idle ticks never reconnect",[&]{
    auto w=std::make_shared<MockWire>();w->auto_ready=true;DiscordRPC rpc(mock_dial(w));
    rpc.tick(id,nullptr);require(w->dials==0,"Idle presence opened a connection");
    rpc.tick(id,a);w->inject(frame(1,{{"evt","ERROR"}}));rpc.tick(id,nullptr);
    require(w->activities().back().is_null(),"Pending reply blocked immediate clear");
    auto count=w->activities().size();auto dials=w->dials;
    for(unsigned i=0;i<20;++i)rpc.tick(id,nullptr);
    require(w->activities().size()==count&&w->dials==dials,"Idle ticks resent clear or reconnected");
 });
 s.test("changing hover time does not flood Discord while art updates still send",[&]{
    auto w=std::make_shared<MockWire>();w->auto_ready=true;auto now=MonoClock::time_point{};DiscordRPC rpc(mock_dial(w),[&]{return now;});
    auto activity=a;activity["assets"]={{"small_text","0:00 elapsed"},{"small_image","play"}};rpc.tick(id,activity);
    for(unsigned i=1;i<30;++i){now+=std::chrono::seconds(1);activity["assets"]["small_text"]=std::to_string(i)+" elapsed";rpc.tick(id,activity);}
    require(w->activities().size()==1,"Hover clock generated repeated activity requests");
    now+=std::chrono::seconds(1);rpc.tick(id,activity);require(w->activities().size()==2,"Periodic hover refresh missing");
    now+=std::chrono::seconds(1);activity["assets"]["large_image"]="https://archive.org/cover.jpg";rpc.tick(id,activity);
    require(w->activities().back()["assets"]["large_image"]=="https://archive.org/cover.jpg","New artwork was suppressed with hover changes");
    rpc.tick(id,nullptr);require(w->activities().back().is_null(),"Clear was throttled after an artwork update");
 });
 s.test("handshake waits for fragmented READY and uses selected application",[&]{auto w=std::make_shared<MockWire>();auto now=MonoClock::time_point{};DiscordRPC rpc(mock_dial(w),[&]{return now;});rpc.tick(id,a);auto sent=w->frames();require(u32(sent[0].data())==0&&unframe(sent[0])["client_id"]==id,"Wrong handshake");auto ready=frame(1,{{"evt","READY"}});w->inject(ready.substr(0,5));rpc.tick(id,a);require(w->activities().empty(),"Premature presence");w->inject(ready.substr(5));rpc.tick(id,a);require(w->activities().back()==a,"No activity after READY");});
 s.test("null acknowledgments, ping-pong and duplicate suppression",[&]{auto w=std::make_shared<MockWire>();w->auto_ready=true;DiscordRPC rpc(mock_dial(w));rpc.tick(id,a);auto before=w->frames().size();w->inject(frame(1,{{"evt",nullptr},{"cmd","SET_ACTIVITY"}})+frame(3,{{"echo","probe"}}));rpc.tick(id,a);auto sent=w->frames();require(sent.size()==before+1&&u32(sent.back().data())==4,"PING did not produce only PONG");require(unframe(sent.back())["echo"]=="probe","PING payload not preserved");});
 s.test("throttle, periodic resend and immediate clear",[&]{auto w=std::make_shared<MockWire>();w->auto_ready=true;auto now=MonoClock::time_point{};DiscordRPC rpc(mock_dial(w),[&]{return now;});rpc.tick(id,a);auto b=a;b["details"]="Next";rpc.tick(id,b);require(w->activities().size()==1,"Updates not throttled");now+=std::chrono::seconds(1);rpc.tick(id,b);require(w->activities().back()==b,"Latest activity not sent");now+=std::chrono::seconds(30);rpc.tick(id,b);require(w->activities().size()==3,"Periodic resend missing");rpc.tick(id,nullptr);require(w->activities().back().is_null(),"Clear should bypass throttle");});
 s.test("disconnect backs off then replays current activity",[&]{auto w=std::make_shared<MockWire>();w->auto_ready=true;auto now=MonoClock::time_point{};DiscordRPC rpc(mock_dial(w),[&]{return now;});rpc.tick(id,a);w->inject(frame(2,json::object()));rpc.tick(id,a);auto dials=w->dials;auto b=a;b["details"]="New current song";rpc.tick(id,b);require(w->dials==dials,"Backoff ignored");now+=std::chrono::seconds(1);rpc.tick(id,b);require(w->dials==dials+1&&w->activities().back()==b,"Reconnect replayed stale activity");});
 s.test("application switch clears old activity and handshakes again",[&]{auto w=std::make_shared<MockWire>();w->auto_ready=true;DiscordRPC rpc(mock_dial(w));rpc.tick(id,a);rpc.tick("223456789012345678",a);auto values=w->activities();require(values.size()==3&&values[1].is_null()&&values[2]==a,"Application switch did not clear and replay");require(w->dials==2,"Application switch did not reconnect");});
 s.test("oversized frame and ERROR disconnect safely",[&]{for(bool oversized:{false,true}){auto w=std::make_shared<MockWire>();w->auto_ready=true;DiscordRPC rpc(mock_dial(w));rpc.tick(id,a);if(oversized){std::string header(8,'\0');header[0]=1;header[6]=32;w->inject(header);}else w->inject(frame(1,{{"evt","ERROR"}}));rpc.tick(id,a);require(rpc.status().find("Connected")==std::string::npos,"Malformed/error frame retained connection");}});
 s.test("handshake timeout and invalid application never share activity",[&]{auto w=std::make_shared<MockWire>();auto now=MonoClock::time_point{};DiscordRPC rpc(mock_dial(w),[&]{return now;});rpc.tick(id,a);now+=std::chrono::seconds(3);rpc.tick(id,a);require(rpc.status().find("timed out")!=std::string::npos,"Handshake timeout missing");auto before=w->dials;rpc.tick("invalid",a);require(w->dials==before,"Invalid ID dialed Discord");});
 s.test("shutdown clears presence and releases transport",[&]{auto w=std::make_shared<MockWire>();w->auto_ready=true;DiscordRPC rpc(mock_dial(w));rpc.tick(id,a);rpc.shutdown();require(w->activities().back().is_null(),"Shutdown left activity visible");});

 s.test("prepared payload is reused for connection checks and reconnect",[&]{auto w=std::make_shared<MockWire>();w->auto_ready=true;auto now=MonoClock::time_point{};DiscordRPC rpc(mock_dial(w),[&]{return now;});PreparedActivity prepared(a);const auto encoded=prepared.encoded;rpc.tick(id,prepared);for(unsigned i=0;i<10;++i)rpc.tick(id,prepared);require(prepared.encoded==encoded&&w->activities().size()==1,"Prepared payload was changed or resent unnecessarily");w->inject(frame(2,json::object()));rpc.tick(id,prepared);now+=std::chrono::seconds(1);rpc.tick(id,prepared);require(w->activities().back()==a,"Prepared reconnect payload incorrect");prepared.replace(nullptr);rpc.tick(id,prepared);require(w->activities().back().is_null(),"Prepared clear delayed");});

 return s.finish();}
