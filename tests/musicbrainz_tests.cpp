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

 s.test("complete songs share compact release lookup and artwork URL",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());auto a=identified();a.title="Song one";a.artist="Artist";mb.lookup(a,[]{return false;});auto before=f.urls.size();auto b=a;b.title="Song two";b.recording_id="44444444-4444-4444-8444-444444444444";auto m=mb.lookup(b,[]{return false;});require(!m.artwork_url.empty()&&f.urls.size()==before,"Album playback repeated release/artwork HTTP requests");require(cache.maintenance_runs()==2,"Each uncached song should maintain its batched writes once");});
 s.test("release summary upgrades to tracks only for missing display fields",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;unsigned full=0,summary=0;f.response=[&](const std::string& url){if(url.starts_with("https://musicbrainz.org/ws/2/release/")){bool tracks=url.find("recordings")!=std::string::npos;if(tracks)++full;else ++summary;return ok({{"id",release},{"title","Album"},{"media",json::array({{{"position",1},{"tracks",json::array({{{"position",3},{"title","Recovered track"},{"recording",{{"id",recording},{"artist-credit",credits("Artist")}}}}})}}})}});}return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());auto a=identified();a.title="Tagged track";a.artist="Artist";mb.lookup(a,[]{return false;});Track b;b.release_id=release;b.track_number=3;b.disc_number=1;b.duration=180;auto recovered=mb.lookup(b,[]{return false;});require(recovered.title=="Recovered track"&&recovered.artist=="Artist","Full-track upgrade did not recover missing fields");require(summary==1&&full==1,"Summary/full request selection incorrect");a.title="Third tagged track";mb.lookup(a,[]{return false;});require(summary==1&&full==1,"Full cache did not also satisfy a summary lookup");});
 s.test("release-detail cache expires and remains bounded",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());Track t;t.title="Song";t.artist="Artist";t.duration=180;for(unsigned i=0;i<65;++i){std::ostringstream id;id<<std::hex<<std::setw(8)<<std::setfill('0')<<i<<"-2222-4222-8222-222222222222";t.release_id=id.str();mb.lookup(t,[]{return false;});}t.release_id="00000000-2222-4222-8222-222222222222";t.title="New first-release song";auto before=f.urls.size();mb.lookup(t,[]{return false;});require(f.urls.size()==before+1,"Oldest detail cache entry was not evicted");f.now+=std::chrono::hours(25);t.title="After expiry";before=f.urls.size();mb.lookup(t,[]{return false;});require(f.urls.size()==before+1,"Expired release details were reused");});
 s.test("group-only songs share compact group details",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=[](const std::string& url){if(url.starts_with("https://musicbrainz.org/ws/2/release-group/"))return ok({{"id",group},{"title","Album"}});return ordinary(url);};MusicBrainz mb(cache,f.request(),f.timing());Track a;a.title="First";a.artist="Artist";a.duration=180;a.release_group_id=group;auto first=mb.lookup(a,[]{return false;});auto before=f.urls.size();a.title="Second";auto second=mb.lookup(a,[]{return false;});require(!first.artwork_url.empty()&&second.album=="Album"&&f.urls.size()==before,"Group-only songs repeated detail/artwork requests");require(f.urls.front().find("inc=artists")==std::string::npos,"Unused group artists were requested");});
 s.test("lookup result exposes cache expiry instead of resetting it",[]{TemporaryDirectory dir;MetadataCache cache(dir.path);Fixture f;f.response=ordinary;MusicBrainz mb(cache,f.request(),f.timing());auto first=mb.lookup_result(identified(),[]{return false;});auto second=mb.lookup_result(identified(),[]{return false;});require(first.expires==second.expires&&first.expires>unix_now()+59LL*86400,"Cache hit reset or lost its expiry");});

 return s.finish();}
