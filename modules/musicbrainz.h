#pragma once
#include "cache.h"
#include "http.h"
#include <unordered_map>
namespace music_tracker {
class MusicBrainz {
public:
    using Request = std::function<HttpResponse(const std::string&,const Cancelled&)>;
    struct Timing {std::function<MonoClock::time_point()> now;std::function<void(MonoClock::time_point,const Cancelled&)> wait;};
    explicit MusicBrainz(MetadataCache& cache,Request requester={},Timing timing={}):cache_(cache),requester_(std::move(requester)),timing_(std::move(timing)){}
    Metadata lookup(const Track& track,const Cancelled& cancelled);
    LookupResult lookup_result(const Track& track,const Cancelled& cancelled);
private:
    MetadataCache& cache_;
    HttpClient http_;
    Request requester_;
    Timing timing_;
    MonoClock::time_point now() const {return timing_.now?timing_.now():MonoClock::now();}
    HttpResponse get(const std::string& url,const Cancelled& cancelled);
    MonoClock::time_point next_request_{};
    json request(const std::string& path,const Cancelled& cancelled);
    struct EntityEntry {json value;bool tracks=false;MonoClock::time_point expires;std::uint64_t used=0;};
    std::unordered_map<std::string,EntityEntry> entities_;
    std::uint64_t cache_sequence_=0;
    void remember(const std::string& key,json value,bool tracks,std::chrono::seconds ttl);
    json release_detail(const std::string& id,bool tracks,const Cancelled& cancelled);
    json group_detail(const std::string& id,const Cancelled& cancelled);
    json album_search(const std::string& query,const Cancelled& cancelled);
    std::string cover(const std::string& kind,const std::string& id,const Cancelled& cancelled);
};
}
