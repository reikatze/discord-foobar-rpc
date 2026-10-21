#include "test_support.h"
#include "../modules/musicbrainz.h"
#include "../modules/http.h"
#include "../modules/presence.h"
#include <sstream>
#include <iomanip>
namespace {
const std::string recording="11111111-1111-4111-8111-111111111111",release="22222222-2222-4222-8222-222222222222",group="33333333-3333-4333-8333-333333333333";
struct Fixture {
    MonoClock::time_point now{};
    std::vector<std::string> urls;std::vector<MonoClock::time_point> times;
    std::function<HttpResponse(const std::string&)> response;
    MusicBrainz::Request request(){return [this](const std::string& url,const Cancelled& cancelled){if(cancelled())throw LookupCancelled();urls.push_back(url);times.push_back(now);return response(url);};}
    MusicBrainz::Timing timing(){return {[this]{return now;},[this](auto target,const Cancelled& cancelled){if(cancelled())throw LookupCancelled();now=std::max(now,target);}};}
};
json credits(const std::string& name){return json::array({{{"name",name}}});}
HttpResponse ok(json value){return {200,value.dump(),""};}
HttpResponse ordinary(const std::string& url){
    if(url.starts_with("https://musicbrainz.org/ws/2/recording/"))return ok({{"id",recording},{"title","Recovered song"},{"length",180000},{"artist-credit",credits("Recovered artist")},{"releases",json::array({{{"id",release},{"title","Album"}}})}});
    if(url.starts_with("https://musicbrainz.org/ws/2/release/"))return ok({{"id",release},{"title","Album"},{"release-group",{{"id",group}}}});
    if(url.starts_with("https://coverartarchive.org/"))return ok({{"images",json::array({{{"front",true},{"thumbnails",{{"500","https://archive.org/front.jpg"}}}}})}});
    throw std::runtime_error("Unexpected fixture endpoint");
}
Track identified(){Track t;t.recording_id=recording;t.release_id=release;t.duration=180;return t;}
}
int main(){Suite s;
 s.test("conflicting studio albums use response order rather than recording ID order",[]{
    const std::string other="44444444-4444-4444-8444-444444444444";
    for(bool fallback:{false,true})for(bool reverse:{false,true}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Artist - Song (Official Video).webm");
        const auto expected=reverse?other:group;unsigned searches=0,covers=0;
        f.response=[&](const std::string& url){
            if(url.find("recording/?query=")!=std::string::npos) {
                ++searches;json rows=json::array();
                if(url.find("recording%3A%22Song%22")!=std::string::npos) {
                    if(fallback&&url.find("video%3Afalse")==std::string::npos)
                        rows.push_back({{"id",other},{"score",100},{"title","Song"},{"artist-credit",credits("Artist")},{"video",true}});
                    else for(unsigned i=0;i<2;++i) {
                        const auto n=reverse?1-i:i;const auto album=n?other:group;
                        json live={{"id",other},{"status","Official"},{"release-group",{{"id",other},{"primary-type","Album"},{"secondary-types",json::array({"Live"})}}}};
                        json first={{"id",album},{"status","Official"},{"release-group",{{"id",album},{"primary-type","Album"}}}};
                        auto later=first;later["release-group"]["id"]=n?group:other;
                        rows.push_back({{"id",n?recording:release},{"score",100},{"title","Song"},{"artist-credit",credits("Artist")},{"releases",json::array({live,first,later})}});
                    }
                }
                return ok({{"recordings",rows}});
            }
            if(url.starts_with("https://musicbrainz.org/ws/2/release-group/")){require(url.find(expected)!=std::string::npos,"Album chosen by ID order");return ok({{"id",expected},{"title","First verified album"}});}
            if(url.starts_with("https://coverartarchive.org/")){++covers;require(url.find(expected)!=std::string::npos,"Cover did not use first verified album");return ordinary(url);}
            throw std::runtime_error("Unexpected ordered album request");
        };
        MusicBrainz mb(cache,f.request(),f.timing());const auto m=mb.lookup(t,[]{return false;});
        require(m.release_group_id==expected&&!m.artwork_url.empty()&&m.recording_id.empty()&&covers==1,"First verified album was not used");
        require(searches==(fallback?3U:2U),"Unnecessary album fallback search");
    }
 });
 s.test("unclassified releases do not veto verified studio artwork",[]{
    const std::string facelift="7d026b9c-9159-34b5-a40d-14f134718394",unknown="84369cde-83a3-4903-a4a9-0117fa874bab";
    for(int mode:{0,1,2,3,4,5}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.duration=286;
        apply_filename_fallback(t,"Man in the Box (Official Video) [TAqZb52sgpU] - Alice In Chains.webm");
        unsigned covers=0,details=0;
        f.response=[&](const std::string& url){
            if(url.find("recording/?query=")!=std::string::npos) {
                json rows=json::array();
                if(url.find("video%3Afalse")!=std::string::npos) {
                    json editions=json::array({{{"id",unknown},{"title","Unclassified edition"}}});
                    if(mode==3)for(unsigned i=0;i<7;++i){
                        std::ostringstream id;id<<std::hex<<std::setw(8)<<std::setfill('0')<<i<<"-1111-4111-8111-111111111111";
                        editions.push_back({{"id",id.str()},{"title","Unclassified edition"}});
                    }
                    rows.push_back({{"id",release},{"score",100},{"title","Man in the Box"},{"artist-credit",credits("Alice in Chains")},{"releases",editions}});
                    if(mode!=5) {
                        json studio={{"id",group},{"title","Facelift"},{"status","Official"},{"release-group",{{"id",facelift},{"primary-type","Album"}}}};
                        rows.push_back({{"id",recording},{"score",100},{"title","Man in the Box"},{"artist-credit",credits("Alice in Chains")},{"releases",json::array({studio})}});
                        if(mode==4){studio["release-group"]["id"]=group;studio["title"]="Conflicting studio album";rows.back()["releases"].push_back(studio);}
                    }
                } else if(url.find("recording%3A%22Man%20in%20the%20Box%22")!=std::string::npos) {
                    rows.push_back({{"id","5c6e1ff2-f262-48e9-ab14-2e87033fdd5f"},{"score",100},{"title","Man in the Box"},{"artist-credit",credits("Alice in Chains")},{"length",270000},{"video",true}});
                }
                return ok({{"recordings",rows}});
            }
            if(url.starts_with("https://musicbrainz.org/ws/2/release/")) {
                ++details;if(mode==2)return HttpResponse{404,"",""};
                return ok({{"id",unknown},{"status",mode==1?json("Official"):json(nullptr)},
                    {"release-group",{{"id",unknown},{"primary-type",mode==1?json(nullptr):json("Album")}}}});
            }
            if(url.starts_with("https://musicbrainz.org/ws/2/release-group/")) {
                require(url.find(facelift)!=std::string::npos,"Unclassified or conflicting album selected");return ok({{"id",facelift},{"title","Facelift"}});
            }
            if(url.starts_with("https://coverartarchive.org/")) {
                ++covers;require(url.find(facelift)!=std::string::npos,"Unclassified artwork requested");return ordinary(url);
            }
            throw std::runtime_error("Unexpected incomplete-release endpoint");
        };
        MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});
        require(m.title=="Man in the Box"&&m.artist=="Alice in Chains"&&m.recording_id.empty()&&m.duration==0,"Album artwork invented a recording identity");
        require(details>0&&details<=6,"Detail budget was ignored");
        if(mode<5){require(m.album=="Facelift"&&m.release_group_id==facelift&&!m.artwork_url.empty()&&covers==1,"Incomplete edition vetoed verified Facelift artwork");
            Playback p;p.active=true;p.track=t;PlaybackClock clock;require(make_activity(p,m,clock)["assets"]["large_image"]==m.artwork_url,"Cover absent from Discord activity");}
        else require(m.artwork_url.empty()&&m.release_group_id.empty()&&covers==0,"Unknown or conflicting albums selected artwork");
    }
 });
 s.test("duplicate releases do not exhaust the album detail budget",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Artist - Song (Official Video).webm");
    const std::string other="44444444-4444-4444-8444-444444444444";unsigned details=0;
    f.response=[&](const std::string& url){
        if(url.find("recording/?query=")!=std::string::npos){
            json rows=json::array();
            if(url.find("recording%3A%22Song%22")!=std::string::npos)for(unsigned i=0;i<8;++i){
                std::ostringstream id;id<<std::hex<<std::setw(8)<<std::setfill('0')<<i<<"-1111-4111-8111-111111111111";
                rows.push_back({{"id",id.str()},{"score",100},{"title","Song"},{"artist-credit",credits("Artist")},{"releases",json::array({{{"id",i==7?other:release},{"title","Album"}}})}});
            }
            return ok({{"recordings",rows}});
        }
        if(url.starts_with("https://musicbrainz.org/ws/2/release/")){++details;return ok({{"id",url.find(other)!=std::string::npos?other:release},{"title","Album"},{"status","Official"},{"release-group",{{"id",group},{"primary-type","Album"}}}});}
        if(url.starts_with("https://musicbrainz.org/ws/2/release-group/"))return ok({{"id",group},{"title","Album"}});
        return ordinary(url);
    };
    MusicBrainz mb(cache,f.request(),f.timing());const auto m=mb.lookup(t,[]{return false;});
    require(details==2&&!m.artwork_url.empty()&&m.release_group_id==group,"Duplicate editions consumed unique-release budget");
 });
 s.test("release failures preserve verified song fields without caching failed work",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;
    f.response=[](const std::string& url){if(url.starts_with("https://musicbrainz.org/ws/2/release/"))return HttpResponse{503,"","120"};return ordinary(url);};
    MusicBrainz mb(cache,f.request(),f.timing());auto t=identified();bool caught=false;
    try{mb.lookup(t,[]{return false;});}catch(const LookupFailure& error){caught=true;require(error.metadata.title=="Recovered song"&&error.metadata.artist=="Recovered artist"&&error.retry_at>=unix_now()+119,"Verified text or server retry deadline lost");}
    require(caught&&!cache.get(lookup_key(t)),"Failed release lookup was cached or swallowed");
 });
 s.test("manual refresh bypasses song misses and negative artwork without discarding positive covers",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;bool available=false;unsigned covers=0;
    f.response=[&](const std::string& url){if(url.starts_with("https://coverartarchive.org/")){++covers;if(!available)return HttpResponse{404,"",""};}return ordinary(url);};
    MusicBrainz mb(cache,f.request(),f.timing());auto t=identified();
    require(mb.lookup(t,[]{return false;}).artwork_url.empty(),"Missing cover unexpectedly resolved");
    const auto before=f.urls.size();available=true;mb.lookup(t,[]{return false;});require(f.urls.size()==before,"Normal lookup ignored cache");
    require(!mb.lookup_result(t,[]{return false;},true).metadata.artwork_url.empty(),"Manual retry reused negative artwork cache");
    const auto count=covers;mb.lookup_result(t,[]{return false;},true);require(covers==count,"Manual retry needlessly downloaded a positive cover URL");
 });
 s.test("video-only search results fall back to album audio with matching names",[]{
    for(const std::string filename:{"Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm","Man in the Box (Official Video) [TAqZb52sgpU] - Alice In Chains.webm"})
    for(int outcome:{0,1,2,3}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.duration=286;apply_filename_fallback(t,filename);
        unsigned searches=0,fallbacks=0,covers=0;
        f.response=[&](const std::string& url){
            if(url.find("recording/?query=")!=std::string::npos) {
                ++searches;json rows=json::array();
                const bool fallback=url.find("video%3Afalse")!=std::string::npos;
                if(fallback) {
                    ++fallbacks;
                    require(url.find("primarytype%3Aalbum")!=std::string::npos&&url.find("status%3Aofficial")!=std::string::npos,"Fallback lacks album constraints");
                    require(url.find("recording%3A%22Man%20in%20the%20Box%22")!=std::string::npos&&url.find("artist%3A%22Alice%20in%20Chains%22")!=std::string::npos,"Fallback lost verified names");
                    if(outcome==3)return HttpResponse{503,"","60"};
                    for(unsigned i=0;i<2;++i) {
                        auto album_group=json{{"id",outcome==1&&i?release:group},{"primary-type","Album"},{"secondary-types",json::array()}};
                        rows.push_back({{"id",i?release:recording},{"score",100},{"video",false},{"title","Man in the Box"},{"length",270000+i*1000},
                            {"artist-credit",credits(outcome==2?"Unrelated artist":"Alice in Chains")},
                            {"releases",json::array({{{"id",group},{"title","Facelift"},{"status","Official"},{"release-group",album_group}}})}});
                    }
                } else if(url.find("recording%3A%22Man%20in%20the%20Box%22")!=std::string::npos) {
                    for(const char* id:{"8ffa9ab2-0350-4a34-8034-aaf4e9451add","b30ce13f-aa23-4f86-9b0b-3aad62c7e0fb","2374e717-863c-4480-b49c-b575255788c1","ced9b0c5-a0f8-4e81-8999-af48b65b3beb","66418d1d-5559-4232-8d55-89fcca4dad7a"})
                        rows.push_back({{"id",id},{"score",100},{"video",true},{"title","Man in the Box"},{"length",270000},{"artist-credit",credits("Alice in Chains")}});
                }
                return ok({{"recordings",rows}});
            }
            if(url.starts_with("https://musicbrainz.org/ws/2/release-group/"))return ok({{"id",group},{"title","Facelift"}});
            if(url.starts_with("https://coverartarchive.org/")){++covers;require(url.find(group)!=std::string::npos,"Wrong album cover requested");return ordinary(url);}
            throw std::runtime_error("Unexpected album fallback endpoint");
        };
        MusicBrainz mb(cache,f.request(),f.timing());
        if(outcome==3){throws<std::exception>([&]{mb.lookup(t,[]{return false;});},"Rate limit ignored");require(!cache.get(lookup_key(t)),"Failed fallback cached");}
        else {
            const auto m=mb.lookup(t,[]{return false;});
            require(m.title=="Man in the Box"&&m.artist=="Alice in Chains"&&m.recording_id.empty()&&m.duration==0,"Album fallback changed recording identity or timing");
            if(outcome==0||outcome==1) {
                require(m.album=="Facelift"&&m.release_group_id==group&&!m.artwork_url.empty()&&m.complete&&covers==1,"Standalone video results blocked album artwork");
                Playback p;p.active=true;p.track=t;PlaybackClock clock;
                require(make_activity(p,m,clock)["assets"]["large_image"]==m.artwork_url,"Recovered cover absent from Discord presence");
            } else require(m.artwork_url.empty()&&m.release_group_id.empty()&&covers==0,"Conflicting or mismatched fallback selected artwork");
        }
        require(searches==3&&fallbacks==1,"Fallback was not bounded to one extra search");
    }
 });
 s.test("sparse studio and live recording search results recover one studio album",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.duration=286;apply_filename_fallback(t,"Man in the Box (Official Video) [TAqZb52sgpU] - Alice In Chains.webm");unsigned details=0;
    const std::string live_recording="44444444-4444-4444-8444-444444444444",live_release="55555555-5555-4555-8555-555555555555",live_group="66666666-6666-4666-8666-666666666666";
    f.response=[&](const std::string& url){
        if(url.find("recording/?query=")!=std::string::npos) {
            json rows=json::array();
            if(url.find("recording%3A%22Man%20in%20the%20Box%22")!=std::string::npos)for(bool live:{false,true})
                rows.push_back({{"id",live?live_recording:recording},{"score",100},{"title","Man in the Box"},{"length",live?286000:270000},{"artist-credit",credits("Alice in Chains")},{"releases",json::array({{{"id",live?live_release:release},{"title",live?"Live album":"Facelift"}}})}});
            return ok({{"recordings",rows}});
        }
        if(url.starts_with("https://musicbrainz.org/ws/2/release/")) {
            ++details;const bool live=url.find(live_release)!=std::string::npos;
            return ok({{"id",live?live_release:release},{"title",live?"Live album":"Facelift"},{"status","Official"},{"release-group",{{"id",live?live_group:group},{"primary-type","Album"},{"secondary-types",live?json::array({"Live"}):json::array()}}}});
        }
        if(url.starts_with("https://musicbrainz.org/ws/2/release-group/"))return ok({{"id",group},{"title","Facelift"}});
        require(url.find(live_group)==std::string::npos,"Live artwork selected");return ordinary(url);
    };
    MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});
    require(details==2&&m.title=="Man in the Box"&&m.artist=="Alice in Chains"&&m.album=="Facelift"&&m.recording_id.empty()&&!m.artwork_url.empty(),"Live recording blocked studio artwork consensus");
 });
 s.test("nullable release types preserve verified text without selecting an album",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Artist - Song (Official Video).webm");
    f.response=[&](const std::string& url){
        if(url.find("recording/?query=")!=std::string::npos) {
            json rows=json::array();if(url.find("recording%3A%22Song%22")!=std::string::npos)for(bool second:{false,true})
                rows.push_back({{"id",second?release:recording},{"score",100},{"title","Song"},{"artist-credit",credits("Artist")},{"releases",json::array({{{"id",group},{"status","Official"},{"release-group",{{"id",group},{"primary-type",nullptr},{"type",nullptr},{"secondary-types",nullptr}}}}})}});
            return ok({{"recordings",rows}});
        }
        throw std::runtime_error("Unexpected nullable-type lookup");
    };
    MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});require(m.title=="Song"&&m.artist=="Artist"&&m.artwork_url.empty(),"Nullable types lost text or selected artwork");
 });
 s.test("common filename patterns resolve canonical presence and artwork",[]{
    for(const std::string filename:{"01 - Artist - Song.flac","Song \xE2\x80\x93 Artist [HQ].MP3","Disc1-02 Artist - Song [FLAC].wv","[02] Artist_-_Song.m4a","02. Artist \xE2\x80\x94 Song (Official Lyric Video) [TAqZb52sgpU].webm"}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.title=filename;t.duration=180;apply_filename_fallback(t,filename);unsigned searches=0;
        f.response=[&](const std::string& url){
            if(url.find("recording/?query=")!=std::string::npos) {
                ++searches;
                require(url.find("Official")==std::string::npos&&url.find("TAqZb52sgpU")==std::string::npos&&url.find("FLAC")==std::string::npos&&url.find("HQ")==std::string::npos,"Filename noise reached lookup");
                if(url.find("recording%3A%22Song%22")!=std::string::npos&&url.find("artist%3A%22Artist%22")!=std::string::npos)
                    return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title","Song"},{"length",180000},{"artist-credit",credits("Artist")},{"releases",json::array({{{"id",release},{"title","Album"}}})}}})}});
                return ok({{"recordings",json::array()}});
            }
            return ordinary(url);
        };
        MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});
        Playback p;p.active=true;p.track=t;PlaybackClock clock;auto activity=make_activity(p,m,clock);
        require(searches==2&&m.complete&&activity["details"]=="Song"&&activity["state"]=="Artist"&&activity["assets"].contains("large_image"),"Pattern did not resolve presence and artwork");
    }
 });
 s.test("numbered title-only filenames recover artist with duration evidence",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.title="02 - Song.flac";t.duration=180;apply_filename_fallback(t,t.title);
    f.response=[&](const std::string& url){
        if(url.find("recording/?query=")!=std::string::npos){require(url.find("recording%3A%22Song%22")!=std::string::npos&&url.find("artist%3A")==std::string::npos,"Track number became artist");return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title","Song"},{"length",180000},{"artist-credit",credits("Artist")},{"releases",json::array({{{"id",release},{"title","Album"}}})}}})}});}
        return ordinary(url);
    };
    MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});require(m.title=="Song"&&m.artist=="Artist"&&!m.artwork_url.empty(),"Title-only pattern did not recover artwork");
 });
 s.test("download filename resolves canonical display and album cover",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.title="Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm";t.duration=286;apply_filename_fallback(t,t.title);f.response=[&](const std::string& url){if(url.find("recording/?query=")!=std::string::npos){if(url.find("artist%3A%22Man%20in%20the%20Box%22")!=std::string::npos)return ok({{"recordings",json::array()}});require(url.find("Man%20in%20the%20Box")!=std::string::npos&&url.find("artist%3A%22Alice%20In%20Chains%22")!=std::string::npos,"Filename hints absent from search");require(url.find("Official")==std::string::npos&&url.find("TAqZb52sgpU")==std::string::npos,"Video decoration sent as title");return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title","Man in the Box"},{"length",286000},{"artist-credit",credits("Alice in Chains")},{"releases",json::array({{{"id",release},{"title","Facelift"}}})}}})}});}if(url.starts_with("https://musicbrainz.org/ws/2/release/"))return ok({{"id",release},{"title","Facelift"},{"release-group",{{"id",group}}}});return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});require(m.title=="Man in the Box"&&m.artist=="Alice in Chains"&&m.album=="Facelift"&&m.artwork_url=="https://archive.org/front.jpg"&&m.complete,"Filename lookup did not recover song and cover");});
 s.test("filename hints still reject conflicting or ambiguous recordings",[]{for(bool ambiguous:{false,true}){TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm");f.response=[&](const std::string&){json a={{"id",recording},{"score",100},{"title","Man in the Box"},{"artist-credit",credits(ambiguous?"Alice in Chains":"Unrelated artist")}};auto rows=json::array({a});if(ambiguous){a["id"]=release;rows.push_back(a);}return ok({{"recordings",rows}});};MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});require(m.artwork_url.empty()&&m.recording_id.empty(),"Unverified filename match selected");}});
 s.test("both filename orders recover canonical presence and artwork",[]{
    for(const std::string filename:{"Artist - Song.webm","Song - Artist.webm"}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;
        apply_filename_fallback(t,filename);t.duration=180;unsigned searches=0;
        f.response=[&](const std::string& url){
            if(url.find("recording/?query=")!=std::string::npos) {
                ++searches;
                if(url.find("recording%3A%22Song%22")!=std::string::npos)
                    return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title","Song"},{"length",180000},{"artist-credit",credits("Artist")},{"releases",json::array({{{"id",release},{"title","Album"}}})}}})}});
                return ok({{"recordings",json::array()}});
            }
            return ordinary(url);
        };
        MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});
        require(searches==2&&m.title=="Song"&&m.artist=="Artist"&&!m.artwork_url.empty(),"Both orders were not verified");
        Playback p;p.active=true;p.track=t;PlaybackClock clock;auto activity=make_activity(p,m,clock);
        require(activity["details"]=="Song"&&activity["state"]=="Artist","Verified order not displayed");
    }
 });
 s.test("filename orders require a unique recording across all candidates",[]{
    for(bool same_order:{false,true}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Artist - Song.webm");
        f.response=[&](const std::string& url){
            const bool forward=url.find("recording%3A%22Song%22")!=std::string::npos;
            json row={{"id",forward?recording:group},{"score",forward?100:90},{"title",forward?"Song":"Artist"},{"artist-credit",credits(forward?"Artist":"Song")}};
            auto rows=json::array({row});
            if(same_order) {
                rows=json::array();
                if(forward){rows.push_back(row);row["id"]=group;row["score"]=90;rows.push_back(row);}
            }
            return ok({{"recordings",rows}});
        };
        MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});
        require(m.recording_id.empty()&&m.artwork_url.empty(),"Ranking chose an ambiguous filename interpretation");
    }
 });
 s.test("duration disambiguates filename orders and requires actual length evidence",[]{
    for(int mode:{0,1,2,3}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Song - Artist.webm");t.duration=180;
        f.response=[&](const std::string& url){
            if(url.find("recording/?query=")!=std::string::npos) {
                const bool correct=url.find("recording%3A%22Song%22")!=std::string::npos;
                json row={{"id",correct?recording:group},{"score",100},{"title",correct?"Song":"Artist"},{"artist-credit",credits(correct?"Artist":"Song")},{"releases",json::array({{{"id",release},{"title","Album"}}})}};
                if(correct&&mode==1){} // Missing length cannot verify a known duration.
                else row["length"]=correct?(mode==2?186000:mode==3?185000:180000):240000;
                return ok({{"recordings",json::array({row})}});
            }
            return ordinary(url);
        };
        MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});
        require((m.recording_id==recording)==(mode==0||mode==3),"Duration evidence or five-second boundary ignored");
    }
 });
 s.test("duplicate filename hits do not create ambiguity without duration",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Song - Artist.flac");
    f.response=[&](const std::string& url){
        if(url.find("recording/?query=")!=std::string::npos) {
            json rows=json::array();
            if(url.find("recording%3A%22Song%22")!=std::string::npos) {
                json row={{"id",recording},{"score",100},{"title","Song"},{"artist-credit",credits("Artist")},{"releases",json::array({{{"id",release},{"title","Album"}}})}};
                rows.push_back(row);rows.push_back(row);
            }
            return ok({{"recordings",rows}});
        }
        return ordinary(url);
    };
    MusicBrainz mb(cache,f.request(),f.timing());require(mb.lookup(t,[]{return false;}).recording_id==recording,"Duplicate ID treated as competing recording");
 });
 s.test("real tags use one order even with a reversed filename",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.title="Song";t.artist="Artist";t.duration=180;apply_filename_fallback(t,"Song - Artist.flac");unsigned searches=0;
    f.response=[&](const std::string& url){
        if(url.find("recording/?query=")!=std::string::npos){++searches;require(url.find("recording%3A%22Song%22")!=std::string::npos&&url.find("artist%3A%22Artist%22")!=std::string::npos,"Tags reversed");return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title","Song"},{"length",180000},{"artist-credit",credits("Artist")},{"releases",json::array({{{"id",release},{"title","Album"}}})}}})}});}
        return ordinary(url);
    };
    MusicBrainz mb(cache,f.request(),f.timing());require(!mb.lookup(t,[]{return false;}).artwork_url.empty()&&searches==1,"Tagged lookup did not retain single-order behavior");
 });
 s.test("reverse lookup errors and cancellation do not cache a partial match",[]{
    for(bool cancel:{false,true}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Artist - Song.flac");unsigned searches=0;
        f.response=[&](const std::string& url){
            require(url.find("recording/?query=")!=std::string::npos,"Artwork requested before checking both orders");
            if(++searches==2){if(cancel)throw LookupCancelled();return HttpResponse{500,"",""};}
            return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title","Song"},{"artist-credit",credits("Artist")}}})}});
        };
        MusicBrainz mb(cache,f.request(),f.timing());throws<std::exception>([&]{mb.lookup(t,[]{return false;});},"Incomplete verification accepted");
        require(!cache.get(lookup_key(t)),"Partial match cached");
    }
 });
 s.test("both decorated Man in the Box filenames recover album artwork for video edits",[]{
    for(const std::string filename:{"Alice In Chains - Man in the Box (Official Video) [TAqZb52sgpU].webm","Man in the Box (Official Video) [TAqZb52sgpU] - Alice In Chains.webm"})
    for(bool editions:{false,true}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.title=filename;t.duration=286;apply_filename_fallback(t,filename);
        const auto album_group=json{{"id",group},{"primary-type","Album"},{"secondary-types",json::array()}};
        const auto album_release=json{{"id",release},{"title","Facelift"},{"status","Official"},{"release-group",album_group}};
        unsigned searches=0;
        f.response=[&](const std::string& url){
            if(url.find("recording/?query=")!=std::string::npos) {
                ++searches;require(url.find("Official")==std::string::npos&&url.find("TAqZb52sgpU")==std::string::npos,"Decoration reached search");
                json rows=json::array();
                if(url.find("recording%3A%22Man%20in%20the%20Box%22")!=std::string::npos) {
                    json row={{"id",recording},{"score",100},{"title","Man in the Box"},{"length",270000},{"artist-credit",credits("Alice in Chains")},{"releases",json::array({album_release})}};
                    rows.push_back(row);
                    if(editions){row["id"]="44444444-4444-4444-8444-444444444444";row["length"]=271000;rows.push_back(row);}
                }
                return ok({{"recordings",rows}});
            }
            if(url.find("release-group/"+group)!=std::string::npos&&url.starts_with("https://musicbrainz.org/"))return ok({{"id",group},{"title","Facelift"}});
            if(url.starts_with("https://musicbrainz.org/ws/2/release/"))return ok(album_release);
            return ordinary(url);
        };
        MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});
        require(searches==2&&m.title=="Man in the Box"&&m.artist=="Alice in Chains"&&m.album=="Facelift"&&!m.artwork_url.empty(),"Video edit lost album artwork");
        require(!editions||m.recording_id.empty(),"Album consensus invented a recording ID");
        Playback p;p.active=true;p.track=t;PlaybackClock clock;auto activity=make_activity(p,m,clock);
        require(activity["details"]=="Man in the Box"&&activity["state"]=="Alice in Chains"&&activity["assets"]["large_image"]==m.artwork_url,"Final presence incorrect");
    }
 });
 s.test("album matching rejects differing names and live compilations",[]{
    for(int mode:{0,2,3}) {
        TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Artist - Song (Official Video).webm");t.duration=200;
        f.response=[&](const std::string& url){
            require(url.find("recording/?query=")!=std::string::npos,"Ambiguous album triggered art lookup");
            bool reverse=url.find("recording%3A%22Artist%22")!=std::string::npos;
            json rows=json::array();
            if(!reverse||mode==0)for(unsigned n=0;n<2;++n) {
                json album_group={{"id",mode==1&&n?release:group},{"primary-type","Album"},{"secondary-types",mode==2?json::array({"Live"}):mode==3?json::array({"Compilation"}):json::array()}};
                rows.push_back({{"id",reverse?(n?"55555555-5555-4555-8555-555555555555":"44444444-4444-4444-8444-444444444444"):(n?release:recording)},{"score",100},{"title",reverse?"Artist":"Song"},{"artist-credit",credits(reverse?"Song":"Artist")},{"length",180000},{"releases",json::array({{{"id",group},{"status","Official"},{"release-group",album_group}}})}});
            }
            return ok({{"recordings",rows}});
        };
        MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});require(m.artwork_url.empty()&&m.recording_id.empty(),"Conflicting album evidence accepted");
    }
 });
 s.test("studio album preferred over compilation releases for a unique video match",[]{
    TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;apply_filename_fallback(t,"Artist - Song (Official Video).webm");
    f.response=[&](const std::string& url){
        if(url.find("recording/?query=")!=std::string::npos){
            if(url.find("recording%3A%22Artist%22")!=std::string::npos)return ok({{"recordings",json::array()}});
            json studio={{"id",release},{"title","Studio album"},{"status","Official"},{"release-group",{{"id",group},{"primary-type","Album"},{"secondary-types",json::array()}}}};
            json compilation={{"id",recording},{"title","Hits"},{"status","Official"},{"release-group",{{"id",recording},{"primary-type","Album"},{"secondary-types",json::array({"Compilation"})}}}};
            return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title","Song"},{"artist-credit",credits("Artist")},{"releases",json::array({compilation,studio})}}})}});
        }
        if(url.starts_with("https://musicbrainz.org/ws/2/release/")){require(url.find(release)!=std::string::npos,"Compilation selected before studio album");return ok({{"id",release},{"title","Studio album"},{"release-group",{{"id",group}}}});}
        if(url.starts_with("https://coverartarchive.org/"))require(url.find(recording)==std::string::npos,"Compilation cover requested");
        return ordinary(url);
    };
    MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});require(m.album=="Studio album"&&!m.artwork_url.empty(),"Studio album preference failed");
 });
 s.test("tagged IDs recover missing fields and hosted cover art",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(identified(),[]{return false;});require(m.title=="Recovered song"&&m.artist=="Recovered artist","Missing fields not recovered");require(m.artwork_url=="https://archive.org/front.jpg"&&m.complete,"Artwork not resolved");require(f.urls.front().find("recording/"+recording)!=std::string::npos,"MBID lookup not prioritized");});
 s.test("complete local tags still resolve art; subsequent lookup uses URLs from disk",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());auto t=identified();t.title="Local song";t.artist="Local artist";mb.lookup(t,[]{return false;});require(f.urls.size()==2&&f.urls[0].starts_with("https://musicbrainz.org/ws/2/release/"),"Complete tags should use one release lookup plus artwork");
    require(f.urls[0].find("recordings")==std::string::npos,"Complete tags unnecessarily requested a track list");auto before=f.urls.size();MusicBrainz reopened(cache,f.request(),f.timing());reopened.lookup(t,[]{return false;});require(f.urls.size()==before,"Fresh disk cache missed");});
 s.test("album artwork cache is shared by different songs",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());auto t=identified();mb.lookup(t,[]{return false;});t.title="Another song";mb.lookup(t,[]{return false;});unsigned covers=0;for(const auto& url:f.urls)if(url.starts_with("https://coverartarchive.org/"))++covers;require(covers==1,"Album URL not shared");});
 s.test("recording searches preserve literal query and exact artist evidence",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;auto t=identified();t.recording_id.clear();t.release_id.clear();t.title="A/B & C";t.artist="Artist";
    f.response=[&](const std::string& url){if(url.find("recording/?query=")!=std::string::npos)return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title",t.title},{"length",180000},{"artist-credit",credits(t.artist)},{"releases",json::array({{{"id",release},{"title","Album"}}})}}})}});return ordinary(url);};
    MusicBrainz mb(cache,f.request(),f.timing());require(!mb.lookup(t,[]{return false;}).artwork_url.empty(),"Exact search did not match");require(f.urls[0].find("A%5C%2FB%20%5C%26%20C")!=std::string::npos,"Lucene/URL escaping incorrect");});
 s.test("ambiguous recordings never select artwork",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.title="Song";t.artist="Artist";t.duration=180;
    f.response=[&](const std::string&){json a={{"id",recording},{"score",100},{"title","Song"},{"length",180000},{"artist-credit",credits("Artist")}};auto b=a;b["id"]=release;return ok({{"recordings",json::array({a,b})}});};MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});require(m.page_url.empty()&&m.artwork_url.empty(),"Ambiguous recording selected");});
 s.test("missing artist search can fill artist with unique duration evidence",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.title="Recovered song";t.duration=180;
    f.response=[&](const std::string& url){if(url.find("recording/?query=")!=std::string::npos)return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title",t.title},{"length",180000},{"artist-credit",credits("Recovered artist")},{"releases",json::array({{{"id",release},{"title","Album"}}})}}})}});return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());require(mb.lookup(t,[]{return false;}).artist=="Recovered artist","Missing artist not recovered");});
 s.test("missing title resolved by release and disc/track number",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.release_id=release;t.disc_number=2;t.track_number=3;t.duration=180;
    f.response=[&](const std::string& url){if(url.starts_with("https://musicbrainz.org/ws/2/release/"))return ok({{"id",release},{"title","Album"},{"media",json::array({{{"position",2},{"tracks",json::array({{{"position",3},{"title","Recovered song"},{"length",180000},{"recording",{{"id",recording},{"title","Recovered song"},{"artist-credit",credits("Recovered artist")}}}}})}}})}});return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(t,[]{return false;});require(m.title=="Recovered song"&&m.artist=="Recovered artist","Disc/track recovery failed");});
 s.test("front cover preferred and HTTP archive URLs upgraded",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=[](const std::string& url){if(url.starts_with("https://coverartarchive.org/"))return ok({{"images",json::array({{{"front",false},{"image","https://archive.org/back.jpg"}},{{"front",true},{"image","http://archive.org/front.jpg"}}})}});return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());require(mb.lookup(identified(),[]{return false;}).artwork_url=="https://archive.org/front.jpg","Front/HTTPS selection incorrect");});
 s.test("missing release cover falls back to release-group cover",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=[](const std::string& url){if(url.starts_with("https://coverartarchive.org/release/"))return HttpResponse{404,"",""};return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());auto m=mb.lookup(identified(),[]{return false;});require(!m.artwork_url.empty(),"Release group fallback failed");require(f.urls.back().find("release-group/"+group)!=std::string::npos,"Fallback used wrong group");});
 s.test("MusicBrainz calls are spaced and Retry-After is respected",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());mb.lookup(identified(),[]{return false;});std::vector<MonoClock::time_point> mbtimes;for(std::size_t i=0;i<f.urls.size();++i)if(f.urls[i].starts_with("https://musicbrainz.org/"))mbtimes.push_back(f.times[i]);require(mbtimes.size()>=2&&mbtimes[1]-mbtimes[0]>=std::chrono::milliseconds(1100),"Rate limit spacing violated");
    Fixture limited;bool first=true;limited.response=[&](const std::string& url){if(first){first=false;return HttpResponse{429,"","120"};}return ordinary(url);};MusicBrainz throttled(cache,limited.request(),limited.timing());auto t=identified();t.title="uncached";throws<std::runtime_error>([&]{throttled.lookup(t,[]{return false;});},"429 should fail lookup");throttled.lookup(t,[]{return false;});require(limited.times[1]-limited.times[0]>=std::chrono::seconds(120),"Retry-After not respected");});
 s.test("transient errors and invalid JSON do not poison the cache",[]{for(bool malformed:{false,true}){TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=[&](const std::string&){return malformed?HttpResponse{200,"not json",""}:HttpResponse{500,"",""};};MusicBrainz mb(cache,f.request(),f.timing());throws<std::exception>([&]{mb.lookup(identified(),[]{return false;});},"Bad response accepted");require(!cache.get(lookup_key(identified())),"Error cached as a permanent miss");}});
 s.test("cancellation stops lookup before network/cache effects",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());throws<LookupCancelled>([&]{mb.lookup(identified(),[]{return true;});},"Cancellation ignored");require(f.urls.empty()&&!cache.get(lookup_key(identified())),"Cancelled lookup had effects");});
 s.test("HTTPS validation and encoding reject local paths",[]{require(!public_https_url("file:///cover.jpg")&&!public_https_url("https://user@archive.org/a"),"Unsafe URL accepted");require(url_encode("a /b") == "a%20%2Fb","URL encoding incorrect");HttpClient http;throws<LookupCancelled>([&]{http.get("https://musicbrainz.org",[]{return true;});},"HTTP cancellation did not precede transfer");});
 s.test("missing artist requires actual duration evidence, not a missing length",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;Track t;t.title="Song";t.duration=180;f.response=[](const std::string&){return ok({{"recordings",json::array({{{"id",recording},{"score",100},{"title","Song"},{"artist-credit",credits("Unknown")}}})}});};MusicBrainz mb(cache,f.request(),f.timing());require(mb.lookup(t,[]{return false;}).artist.empty(),"Missing response length was treated as duration evidence");});

 s.test("complete songs share compact release lookup and artwork URL",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());auto a=identified();a.title="Song one";a.artist="Artist";mb.lookup(a,[]{return false;});auto before=f.urls.size();auto b=a;b.title="Song two";b.recording_id="44444444-4444-4444-8444-444444444444";auto m=mb.lookup(b,[]{return false;});require(!m.artwork_url.empty()&&f.urls.size()==before,"Album playback repeated release/artwork HTTP requests");require(cache.maintenance_runs()==1,"Consecutive songs unnecessarily rescanned the cache");});
 s.test("release summary upgrades to tracks only for missing display fields",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;unsigned full=0,summary=0;f.response=[&](const std::string& url){if(url.starts_with("https://musicbrainz.org/ws/2/release/")){bool tracks=url.find("recordings")!=std::string::npos;if(tracks)++full;else ++summary;return ok({{"id",release},{"title","Album"},{"media",json::array({{{"position",1},{"tracks",json::array({{{"position",3},{"title","Recovered track"},{"recording",{{"id",recording},{"artist-credit",credits("Artist")}}}}})}}})}});}return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());auto a=identified();a.title="Tagged track";a.artist="Artist";mb.lookup(a,[]{return false;});Track b;b.release_id=release;b.track_number=3;b.disc_number=1;b.duration=180;auto recovered=mb.lookup(b,[]{return false;});require(recovered.title=="Recovered track"&&recovered.artist=="Artist","Full-track upgrade did not recover missing fields");require(summary==1&&full==1,"Summary/full request selection incorrect");a.title="Third tagged track";mb.lookup(a,[]{return false;});require(summary==1&&full==1,"Full cache did not also satisfy a summary lookup");});
 s.test("release-detail cache expires and remains bounded",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());Track t;t.title="Song";t.artist="Artist";t.duration=180;for(unsigned i=0;i<65;++i){std::ostringstream id;id<<std::hex<<std::setw(8)<<std::setfill('0')<<i<<"-2222-4222-8222-222222222222";t.release_id=id.str();mb.lookup(t,[]{return false;});}t.release_id="00000000-2222-4222-8222-222222222222";t.title="New first-release song";auto before=f.urls.size();mb.lookup(t,[]{return false;});require(f.urls.size()==before+1,"Oldest detail cache entry was not evicted");f.now+=std::chrono::hours(25);t.title="After expiry";before=f.urls.size();mb.lookup(t,[]{return false;});require(f.urls.size()==before+1,"Expired release details were reused");});
 s.test("group-only songs share compact group details",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=[](const std::string& url){if(url.starts_with("https://musicbrainz.org/ws/2/release-group/"))return ok({{"id",group},{"title","Album"}});return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());Track a;a.title="First";a.artist="Artist";a.duration=180;a.release_group_id=group;auto first=mb.lookup(a,[]{return false;});auto before=f.urls.size();a.title="Second";auto second=mb.lookup(a,[]{return false;});require(!first.artwork_url.empty()&&second.album=="Album"&&f.urls.size()==before,"Group-only songs repeated detail/artwork requests");require(f.urls.front().find("inc=artists")==std::string::npos,"Unused group artists were requested");});
 s.test("lookup result exposes cache expiry instead of resetting it",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());auto first=mb.lookup_result(identified(),[]{return false;});auto second=mb.lookup_result(identified(),[]{return false;});require(first.expires==second.expires&&first.expires>unix_now()+59LL*86400,"Cache hit reset or lost its expiry");});

 return s.finish();}
