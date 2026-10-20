#include "test_support.h"
#include "../modules/service.h"
#include "../modules/musicbrainz.h"
Playback song(const std::string& name){Playback p;p.active=true;p.track.identity=name;p.track.title=name;p.track.artist="Artist";p.track.duration=180;p.epoch=epoch_now();return p;}
int main(){Suite s;
 s.test("transient retry failure preserves an already complete presence",[]{
    auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<unsigned> calls=0;
    ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled&)->LookupResult{
        Metadata m;m.title="Verified song";m.artist="Artist";m.artwork_url="https://archive.org/keep.jpg";m.complete=true;
        if(++calls>1)throw LookupFailure({},"Offline",unix_now()+3600);
        return m;
    };
    Service service({}, {true,"123456789012345678"},d);service.publish(song("Song"));
    eventually([&]{auto a=w->activities();return !a.empty()&&a.back().is_object()&&a.back()["assets"].value("large_image",std::string{})=="https://archive.org/keep.jpg";});
    require(service.retry_current_track(),"Retry rejected");eventually([&]{return calls.load()==2&&service.status().find("will retry")!=std::string::npos;});
    auto a=w->activities();require(a.back()["assets"]["large_image"]=="https://archive.org/keep.jpg","Transient failure discarded a good cover");
 });
 s.test("manual retry refreshes current playback and retains useful presence",[]{
    auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<unsigned> calls=0;
    ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled&){
        Metadata m;m.title="Recovered song";m.artist="Recovered artist";
        if(++calls==1)throw LookupFailure(m,"Album lookup offline",unix_now()+3600);
        m.artwork_url="https://archive.org/recovered.jpg";m.complete=true;return LookupResult{m,unix_now()+3600};
    };
    Service service({}, {true,"123456789012345678"},d);require(!service.retry_current_track(),"Stopped playback accepted retry");
    auto p=song("file");p.track.title.clear();p.track.artist.clear();p.track.fallback_title="Filename";p.paused=true;service.publish(p);
    eventually([&]{return service.status().find("Song identified; artwork lookup unavailable")!=std::string::npos;});
    eventually([&]{auto a=w->activities();return !a.empty()&&a.back().is_object()&&a.back()["details"]=="Recovered song";});
    require(service.retry_current_track(),"Active paused track rejected retry");
    eventually([&]{return calls.load()==2&&service.status().find("artwork ready")!=std::string::npos;});
    eventually([&]{auto a=w->activities();return !a.empty()&&a.back().is_object()&&a.back()["assets"].value("large_image",std::string{})=="https://archive.org/recovered.jpg";});
    service.configure({false,"123456789012345678"});require(!service.retry_current_track(),"Disabled component accepted retry");
 });
 s.test("manual retry cancels in-flight work without accepting stale metadata",[]{
    auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<unsigned> calls=0;std::atomic<bool> cancelled=false;
    ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled& cancel)->LookupResult{
        if(++calls==1){while(!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(2));cancelled=true;Metadata stale;stale.title="Stale";return stale;}
        Metadata m;m.title="Current";m.artist="Artist";return m;
    };
    Service service({}, {true,"123456789012345678"},d);service.publish(song("Song"));eventually([&]{return calls.load()==1;});
    require(service.retry_current_track(),"Retry rejected");eventually([&]{return calls.load()==2&&cancelled.load();});
    for(const auto& activity:w->activities())if(activity.is_object())require(activity.value("details",std::string{})!="Stale","Stale retry result published");
 });
 s.test("verbose preference updates diagnostics without restarting lookup",[]{
    auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<unsigned> calls=0;Diagnostics log;
    ServiceDependencies d;d.diagnostics=log;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled&){++calls;return Metadata{};};
    Service service({}, {true,"123456789012345678",false},d);service.publish(song("Song"));eventually([&]{return calls.load()==1;});
    service.configure({true,"123456789012345678",true});require(log.verbose()&&calls==1,"Verbose toggle restarted lookup or did not apply");
    service.configure({true,"123456789012345678",false});require(!log.verbose()&&calls==1,"Verbose disable did not apply");
 });
 s.test("status distinguishes an unidentified album from unavailable cover art",[]{
    for(bool album:{false,true}) {
        auto w=std::make_shared<MockWire>();w->auto_ready=true;ServiceDependencies d;d.dial=mock_dial(w);
        d.lookup=[&](const Track&,const Cancelled&){Metadata m;m.title="Song";m.artist="Artist";if(album)m.release_group_id="33333333-3333-4333-8333-333333333333";return m;};
        Service service({}, {true,"123456789012345678"},d);service.publish(song("Song"));
        eventually([&]{return service.status().find(album?"Album identified; no hosted cover art":"Song identified; no unambiguous album")!=std::string::npos;});
    }
 });
 s.test("stopping playback clears promptly while metadata lookup is blocked",[]{
    auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<bool> started=false,cancel_seen=false;
    ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled& cancel)->Metadata{
        started=true;while(!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(2));cancel_seen=true;throw LookupCancelled();
    };
    Service service({}, {true,"123456789012345678"},d);auto p=song("Song");service.publish(p);
    eventually([&]{auto activities=w->activities();return started.load()&&!activities.empty()&&activities.back().is_object();});
    p.active=false;service.publish(p);
    eventually([&]{auto activities=w->activities();return cancel_seen.load()&&!activities.empty()&&activities.back().is_null();},500);
 });
 s.test("slow obsolete lookup never overwrites the current song",[]{auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<bool> old_started=false,cancelled=false;ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track& t,const Cancelled& cancel){if(t.title=="Old"){old_started=true;while(!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(5));cancelled=true;Metadata stale;stale.artwork_url="https://archive.org/old.jpg";return stale;}Metadata m;m.title=t.title;m.artwork_url="https://archive.org/new.jpg";return m;};Service service({}, {true,"123456789012345678"},d);service.publish(song("Old"));eventually([&]{return old_started.load();});service.publish(song("New"));eventually([&]{auto values=w->activities();return !values.empty()&&values.back().is_object()&&values.back().value("details",std::string{})=="New"&&values.back().contains("assets")&&values.back()["assets"].value("large_image",std::string{})=="https://archive.org/new.jpg";});require(cancelled,"Obsolete lookup not cancelled");for(const auto& j:w->activities())if(j.is_object()&&j.value("details",std::string{})=="New")require(j["assets"].value("large_image",std::string{})!="https://archive.org/old.jpg","Old cover overwrote new track");});
 s.test("disable clears, cancels lookup, and enable reuses current playback",[]{auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<unsigned> lookups=0;std::atomic<bool> cancel_seen=false;ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled& cancelled){if(++lookups==1){while(!cancelled())std::this_thread::sleep_for(std::chrono::milliseconds(5));cancel_seen=true;throw LookupCancelled();}return Metadata{};};Service service({}, {true,"123456789012345678"},d);service.publish(song("Song"));eventually([&]{return lookups.load()==1&&!w->activities().empty();});service.configure({false,"123456789012345678"});eventually([&]{auto a=w->activities();return cancel_seen.load()&&!a.empty()&&a.back().is_null();});service.configure({true,"123456789012345678"});eventually([&]{auto a=w->activities();return lookups.load()>=2&&!a.empty()&&a.back().is_object();});});
 s.test("lookup failures preserve basic local-tag presence",[]{auto w=std::make_shared<MockWire>();w->auto_ready=true;ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[](const Track&,const Cancelled&)->Metadata{throw std::runtime_error("Offline");};Service service({}, {true,"123456789012345678"},d);service.publish(song("Song"));eventually([&]{auto a=w->activities();return !a.empty()&&a.back().is_object()&&a.back()["details"]=="Song";});eventually([&]{return service.status().find("unavailable")!=std::string::npos;});});
 s.test("custom ID change reconnects without restarting metadata lookup",[]{auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<unsigned> calls=0;ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled&){++calls;return Metadata{};};Service service({}, {true,"123456789012345678"},d);service.publish(song("Song"));eventually([&]{return calls.load()==1&&!w->activities().empty();});service.configure({true,"223456789012345678"});eventually([&]{for(const auto& f:w->frames())if(u32(f.data())==0&&unframe(f)["client_id"]=="223456789012345678")return true;return false;});require(calls==1,"ID change unnecessarily invalidated metadata");throws<std::invalid_argument>([&]{service.configure({false,"bad"});},"Invalid service settings accepted");});
 s.test("shutdown cancels pending lookup and clears presence",[]{auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<bool> started=false,ended=false;ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled& cancel)->Metadata{started=true;while(!cancel())std::this_thread::sleep_for(std::chrono::milliseconds(5));ended=true;throw LookupCancelled();};Service service({}, {true,"123456789012345678"},d);service.publish(song("Song"));eventually([&]{return started.load()&&!w->activities().empty();});service.stop();require(ended,"Lookup survived shutdown");require(w->activities().back().is_null(),"Shutdown did not clear Discord");service.stop();});

 s.test("ordinary playback updates reuse lookup key and do not refresh before expiry",[]{auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<unsigned> calls=0;std::mutex mutex;std::string observed;ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track& t,const Cancelled&){std::lock_guard lock(mutex);observed=t.cache_key;++calls;return LookupResult{{},unix_now()+3600};};Service service({}, {true,"123456789012345678"},d);auto p=song("Song");service.publish(p);eventually([&]{return calls.load()==1;});for(unsigned i=0;i<20;++i){p.position=i;p.epoch=epoch_now();service.publish(p);}std::this_thread::sleep_for(std::chrono::milliseconds(300));require(calls==1,"Playback ticks triggered redundant metadata work");{std::lock_guard lock(mutex);require(observed==lookup_key(p.track),"Lookup worker did not receive the memoized key");}service.configure({true,"123456789012345678"});require(calls==1,"Unchanged settings invalidated lookup");});
 s.test("lookup refreshes at returned expiry while paused",[]{auto w=std::make_shared<MockWire>();w->auto_ready=true;std::atomic<unsigned> calls=0;ServiceDependencies d;d.dial=mock_dial(w);d.lookup=[&](const Track&,const Cancelled&){auto count=++calls;return LookupResult{{},unix_now()+(count==1?1:3600)};};Service service({}, {true,"123456789012345678"},d);auto p=song("Song");p.paused=true;service.publish(p);eventually([&]{return calls.load()>=2;},2500);require(calls==2,"Expiry did not produce exactly one refresh");});

 return s.finish();}
