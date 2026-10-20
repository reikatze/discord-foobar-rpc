#include "cache.h"
#include "http.h"
#include <algorithm>
#include <cctype>
#include <vector>
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace music_tracker {
std::filesystem::path MetadataCache::filename(const std::string& key) const {
    // Stable FNV-1a filename; exact key verification makes collisions harmless cache misses.
    std::uint64_t hash=14695981039346656037ULL;
    for (unsigned char c:key) {hash^=c;hash*=1099511628211ULL;}
    std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<hash;
    return folder_/(s.str()+".json");
}
std::optional<LookupResult> MetadataCache::get_record(const std::string& key) const {
    if (folder_.empty())return std::nullopt;
    try {
        auto path=filename(key);
        if (std::filesystem::file_size(path)>65536)return std::nullopt;
        std::ifstream stream(path,std::ios::binary);json record;stream>>record;
        if (record.value("version",0)!=1 || record.value("key",std::string{})!=key || record.value("expires",std::int64_t{})<=unix_now())return std::nullopt;
        auto value=record.at("metadata").get<Metadata>();
        if (!value.artwork_url.empty() && !public_https_url(value.artwork_url))return std::nullopt;
        if (!value.page_url.empty() && !value.page_url.starts_with("https://musicbrainz.org/"))return std::nullopt;
        return LookupResult{std::move(value),record.at("expires").get<std::int64_t>()};
    } catch (...) {return std::nullopt;}
}
std::optional<Metadata> MetadataCache::get(const std::string& key) const {
    if(auto record=get_record(key))return std::move(record->metadata);
    return std::nullopt;
}
std::int64_t MetadataCache::put(const std::string& key,const Metadata& metadata,std::int64_t ttl) const {
    const auto expires=unix_now()+ttl;
    if(folder_.empty())return expires;
    std::filesystem::path temporary;
    try {
        if(!prepared_){std::filesystem::create_directories(folder_);prepared_=true;}
        auto path=filename(key);const bool existed=std::filesystem::exists(path);temporary=path;temporary += ".tmp";
        json record={{"version",1},{"key",key},{"expires",expires},{"metadata",metadata}};
        {std::ofstream stream(temporary,std::ios::binary|std::ios::trunc);stream<<record.dump();stream.flush();if(!stream)throw std::runtime_error("Cache write failed");}
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cache replace failed");
#else
        std::filesystem::permissions(temporary,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
        std::filesystem::rename(temporary,path);
#endif
        dirty_=true;++writes_since_scan_;if(!existed)++new_entries_;
        if(!batch_depth_)maintain();
    } catch (...) {std::error_code error;if(!temporary.empty())std::filesystem::remove(temporary,error);}
    return expires;
}
void MetadataCache::end_batch() const noexcept {if(batch_depth_)--batch_depth_;if(!batch_depth_)maintain();}
void MetadataCache::maintain() const noexcept {
    if(!dirty_)return;
    try {
        const auto now=now_?now_():MonoClock::now();
        if(scanned_&&now<next_scan_&&writes_since_scan_<128&&known_entries_+new_entries_<=4096)return;
        trim();dirty_=false;scanned_=true;writes_since_scan_=0;new_entries_=0;next_scan_=now+std::chrono::minutes(5);++maintenance_runs_;
    }catch(...){} // Retry maintenance after a future write.
}
std::optional<std::string> MetadataCache::artwork(const std::string& entity) const {
    if (auto hit=get("art:"+entity))return hit->artwork_url;
    return std::nullopt;
}
void MetadataCache::put_artwork(const std::string& entity,const std::string& url) const {
    Metadata m;m.artwork_url=url;m.complete=!url.empty();put("art:"+entity,m,url.empty()?3600:60LL*86400);
}
void MetadataCache::trim() const {
    struct Entry {std::filesystem::path path;std::filesystem::file_time_type time;};
    std::vector<Entry> entries;
    for (const auto& entry:std::filesystem::directory_iterator(folder_)) {
        auto name=entry.path().filename().string();
        if (!entry.is_regular_file()||name.size()!=21||entry.path().extension()!=".json")continue;
        if (!std::all_of(name.begin(),name.begin()+16,[](unsigned char c){return std::isxdigit(c);}))continue;
        entries.push_back({entry.path(),{}});
    }
    known_entries_=entries.size();
    if (entries.size()<=4096)return;
    for(auto& entry:entries)entry.time=std::filesystem::last_write_time(entry.path);
    std::sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.time<b.time;});
    for(std::size_t i=0;i<entries.size()-4096;++i){std::error_code error;if(std::filesystem::remove(entries[i].path,error))--known_entries_;}
}
}
