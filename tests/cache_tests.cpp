#include "test_support.h"
#include "../modules/cache.h"
#include <iomanip>
#include <sstream>
int main(){Suite s;
 s.test("cache scans are throttled by time and write count",[]{
    TemporaryDirectory dir;MonoClock::time_point now{};MetadataCache cache(dir.path,[&]{return now;});
    cache.put("first",{},60);require(cache.maintenance_runs()==1,"Initial cache scan missing");
    for(unsigned i=0;i<127;++i)cache.put("next"+std::to_string(i),{},60);
    require(cache.maintenance_runs()==1,"Every write still scanned the directory");
    cache.put("threshold",{},60);require(cache.maintenance_runs()==2,"Write-count maintenance missing");
    now+=std::chrono::minutes(5);cache.put("first",{},60);
    require(cache.maintenance_runs()==3,"Timed maintenance missing");
 });
 s.test("deferred cache scans still enforce the record limit",[]{
    TemporaryDirectory dir;
    for(unsigned i=0;i<4095;++i){std::ostringstream name;name<<std::hex<<std::setw(16)<<std::setfill('0')<<i;std::ofstream(dir.path/(name.str()+".json"))<<"{}";}
    MetadataCache cache(dir.path);cache.put("first",{},60);cache.put("second",{},60);
    unsigned count=0;for(const auto& entry:std::filesystem::directory_iterator(dir.path))if(entry.path().extension()==".json")++count;
    require(count==4096&&cache.maintenance_runs()==2,"Throttling let cache exceed its limit");
 });
 s.test("metadata and artwork URL persist across cache instances",[]{TemporaryDirectory dir;Metadata m;m.title="Song";m.artist="Artist";m.artwork_url="https://archive.org/front.jpg";MetadataCache(dir.path).put("song",m,60);MetadataCache reopened(dir.path);require(reopened.get("song")->title=="Song","Disk round trip failed");reopened.put_artwork("release:id",m.artwork_url);require(MetadataCache(dir.path).artwork("release:id")==m.artwork_url,"Artwork cache not persistent");});
 s.test("expiry and negative artwork entries",[]{TemporaryDirectory dir;MetadataCache c(dir.path);c.put("expired",{},-1);require(!c.get("expired"),"Expired result reused");c.put_artwork("release:no-art","");auto hit=c.artwork("release:no-art");require(hit&&hit->empty(),"Negative artwork cache absent");});
 s.test("corrupt records and wrong keys cannot be reused",[]{TemporaryDirectory dir;MetadataCache c(dir.path);Metadata m;m.title="Song";c.put("key",m,60);auto path=std::filesystem::directory_iterator(dir.path)->path();{std::ofstream o(path);o<<"{broken";}require(!c.get("key"),"Corrupt cache parsed");c.put("key",m,60);json record;{std::ifstream i(path);i>>record;}record["key"]="different";{std::ofstream o(path);o<<record;}require(!c.get("key"),"Hash collision/wrong key reused");});
 s.test("oversized records and unsafe artwork URLs are ignored",[]{TemporaryDirectory dir;MetadataCache c(dir.path);Metadata m;m.artwork_url="file:///private/cover.jpg";c.put("unsafe",m,60);require(!c.get("unsafe"),"Local image URL allowed");c.put("large",{},60);for(const auto& entry:std::filesystem::directory_iterator(dir.path)){json j;std::ifstream(entry.path())>>j;if(j["key"]=="large"){std::ofstream o(entry.path());o<<std::string(65537,'x');}}require(!c.get("large"),"Oversized cache accepted");});
 s.test("disabled/unwritable cache does not throw or write elsewhere",[]{MetadataCache disabled({});disabled.put("key",{},60);require(!disabled.get("key"),"Empty path cache should miss");TemporaryDirectory dir;auto blocker=dir.path/"file";{std::ofstream f(blocker);f<<"block";}MetadataCache blocked(blocker/"cache");blocked.put("key",{},60);require(!blocked.get("key"),"Unwritable cache unexpectedly saved");});
 s.test("disk cache remains bounded and unrelated files survive trimming",[]{TemporaryDirectory dir;for(unsigned i=0;i<4097;++i){std::ostringstream name;name<<std::hex<<std::setw(16)<<std::setfill('0')<<i;std::ofstream(dir.path/(name.str()+".json"))<<"{}";}std::ofstream(dir.path/"unrelated.txt")<<"keep";MetadataCache c(dir.path);c.put("fresh",{},60);unsigned count=0;for(const auto& e:std::filesystem::directory_iterator(dir.path))if(e.path().extension()==".json")++count;require(count==4096&&std::filesystem::exists(dir.path/"unrelated.txt"),"Trim exceeded bound or removed unrelated data");});

 s.test("nested lookup batch maintains once and returns stored expiry",[]{TemporaryDirectory dir;MetadataCache c(dir.path);std::int64_t expiry=0;{auto batch=c.batch();expiry=c.put("one",{},60);{auto nested=c.batch();c.put_artwork("release:one","https://archive.org/front.jpg");c.put("two",{},60);}require(c.maintenance_runs()==0,"Maintenance ran before batch completion");}require(c.maintenance_runs()==1,"Batch did not maintain exactly once");auto record=c.get_record("one");require(record&&record->expires==expiry,"Cache expiry was not preserved");});

 return s.finish();}
