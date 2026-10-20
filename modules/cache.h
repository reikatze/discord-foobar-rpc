#pragma once
#include "model.h"
#include <filesystem>
#include <optional>
namespace music_tracker {
class MetadataCache {
public:
    explicit MetadataCache(std::filesystem::path folder):folder_(std::move(folder)){}
    std::optional<LookupResult> get_record(const std::string& key) const;
    std::optional<Metadata> get(const std::string& key) const;
    std::int64_t put(const std::string& key,const Metadata& metadata,std::int64_t ttl) const;
    std::optional<std::string> artwork(const std::string& entity) const;
    void put_artwork(const std::string& entity,const std::string& url) const;
    // Cache is owned by the serial lookup worker. A batch performs maintenance only once.
    class Batch {
    public:
        explicit Batch(const MetadataCache& cache):cache_(cache){++cache_.batch_depth_;}
        ~Batch(){cache_.end_batch();}
        Batch(const Batch&)=delete;
        Batch& operator=(const Batch&)=delete;
    private:const MetadataCache& cache_;
    };
    Batch batch() const {return Batch(*this);}
    std::size_t maintenance_runs() const {return maintenance_runs_;}
private:
    mutable unsigned batch_depth_=0;
    mutable bool dirty_=false,prepared_=false;
    mutable std::size_t maintenance_runs_=0;
    void end_batch() const noexcept;
    void maintain() const noexcept;
    std::filesystem::path folder_;
    std::filesystem::path filename(const std::string& key) const;
    void trim() const;
};
}
