#include "test_support.h"
#include "../modules/diagnostics.h"
#include "../modules/musicbrainz.h"
struct LogHarness {
    std::vector<std::string> lines;
    std::vector<Diagnostics::Task> tasks;
    MonoClock::time_point now{};
    Diagnostics log{[this](const auto& line){lines.push_back(line);},[this](auto task){tasks.push_back(std::move(task));},[this]{return now;}};
    void flush(){auto pending=std::move(tasks);tasks.clear();for(auto& task:pending)task();}
    bool contains(const std::string& text)const{for(const auto& line:lines)if(line.find(text)!=std::string::npos)return true;return false;}
};
int main(){Suite s;
 s.test("normal logs are queued and verbose detail is opt-in",[]{LogHarness h;h.log.detail("lookup #1","secret detail");require(h.tasks.empty(),"Disabled verbose queued work");h.log.info("lookup #1","Lookup failed: HTTP 503");require(h.lines.empty()&&h.tasks.size()==1,"Worker printed synchronously");h.flush();require(h.contains("HTTP 503")&&!h.contains("secret detail"),"Normal logging lost failure or leaked verbose");auto copy=h.log;copy.set_verbose(true);h.log.detail("lookup #1","request URL");h.flush();require(h.contains("request URL"),"Runtime toggle not shared");});
 s.test("turning verbose off removes queued verbose details",[]{LogHarness h;h.log.set_verbose(true);h.log.detail("lookup","candidate");h.log.info("lookup","summary");h.log.set_verbose(false);h.flush();require(!h.contains("candidate")&&h.contains("summary"),"Queued detail printed after opt-out");});
 s.test("repeat suppression is bounded in time and keeps track scopes separate",[]{LogHarness h;h.log.info("lookup #1","failure");h.log.info("lookup #1","failure");h.log.info("lookup #2","failure");h.flush();require(h.lines.size()==2,"Duplicate logged or another track hidden");h.now+=std::chrono::seconds(31);h.log.info("lookup #1","failure");h.flush();require(h.lines.size()==3,"Repeat never became visible again");});
 s.test("queue flood is bounded and reports omitted messages",[]{LogHarness h;for(unsigned i=0;i<300;++i)h.log.info("lookup",std::to_string(i));require(h.tasks.size()==1,"Flood scheduled multiple UI callbacks");h.flush();require(h.lines.size()==257&&h.contains("44 messages omitted"),"Queue limit or drop summary incorrect");});
 s.test("concurrent workers share one bounded main-thread drain",[]{LogHarness h;std::vector<std::thread> threads;for(unsigned n=0;n<4;++n)threads.emplace_back([&,n]{for(unsigned i=0;i<100;++i)h.log.info("worker "+std::to_string(n),std::to_string(i));});for(auto& thread:threads)thread.join();require(h.lines.empty()&&h.tasks.size()==1,"Workers printed or duplicated callback");h.flush();require(h.lines.size()==257&&h.contains("144 messages omitted"),"Concurrent queue accounting incorrect");});
 s.test("console lines sanitize controls and bound oversized messages",[]{LogHarness h;h.log.info("lookup\n#1",std::string(2000,'x')+"\n");h.log.info("Discord","error\nnext\rline");h.flush();for(const auto& line:h.lines)require(line.size()<1800&&line.find_first_of("\n\r")==std::string::npos,"Console line was unbounded or multiline");});
 s.test("scheduled batches safely outlive the logger handle",[]{std::vector<Diagnostics::Task> tasks;std::vector<std::string> lines;{Diagnostics log([&](const auto& line){lines.push_back(line);},[&](auto task){tasks.push_back(std::move(task));});log.info("lookup","complete");}tasks.front()();require(lines.size()==1,"Pending callback used destroyed logger");});
 s.test("logging failures never escape into playback",[]{Diagnostics broken([](const auto&){throw std::runtime_error("writer");},[](auto task){task();});broken.info("lookup","normal");unsigned schedules=0;Diagnostics failed([](const auto&){},[&](auto){++schedules;throw std::runtime_error("scheduler");});failed.info("lookup","one");failed.info("lookup","two");require(schedules==2,"Scheduler failure permanently disabled draining");});
 s.test("real lookup diagnostics explain rejection and expose URLs only in verbose mode",[]{
    for(bool verbose:{false,true}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);LogHarness h;h.log.set_verbose(verbose);Track track;track.title="Song";track.artist="Artist";track.duration=180;
        const std::string recording="11111111-1111-4111-8111-111111111111",release="22222222-2222-4222-8222-222222222222";MonoClock::time_point now{};
        MusicBrainz mb(cache,[&](const std::string& url,const Cancelled&){
            if(url.find("recording/?query=")!=std::string::npos){json row={{"id",recording},{"score",100},{"title","Song"},{"length",180000},{"artist-credit",json::array({{{"name","Artist"}}})},{"releases",json::array({{{"id",release},{"title","Album"}}})}};auto wrong=row;wrong["artist-credit"]=json::array({{{"name","Other artist"}}});return HttpResponse{200,json{{"recordings",json::array({wrong,row})}}.dump(),""};}
            if(url.starts_with("https://musicbrainz.org/ws/2/release/"))return HttpResponse{200,json{{"id",release},{"title","Album"}}.dump(),""};
            return HttpResponse{200,json{{"images",json::array({{{"front",true},{"image","https://archive.org/cover.jpg"}}})}}.dump(),""};
        },{[&]{return now;},[&](auto target,const Cancelled&){now=target;}},h.log);
        auto result=mb.lookup(track,[]{return false;});h.flush();require(!result.artwork_url.empty()&&h.contains("Rejected 1 search candidates"),"Diagnostics changed lookup or missed summary");
        require(h.contains("artist mismatch")==verbose&&h.contains("https://archive.org/cover.jpg")==verbose&&h.contains("GET https://")==verbose,"Verbose gating failed");
    }
 });
 return s.finish();}
