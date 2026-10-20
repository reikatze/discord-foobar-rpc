#include "service.h"
#include "discord.h"
#include "musicbrainz.h"
#include "presence.h"
#include <algorithm>
namespace music_tracker {
// Kept exclusively in implementation. UI/configuration store only the optional override.
std::string effective_application_id(const Settings& settings) {
    return settings.custom_application_id.empty()?"1559503631592718376":settings.custom_application_id;
}
Service::Service(std::filesystem::path folder,Settings settings,ServiceDependencies dependencies):cache_folder_(std::move(folder)),dependencies_(std::move(dependencies)),settings_(std::move(settings)) {
    if(!valid_application_id(settings_.custom_application_id))throw std::invalid_argument("Invalid custom application ID");
    HttpClient::global_init();
    presence_thread_=std::thread([this]{presence_worker();});
    try{lookup_thread_=std::thread([this]{lookup_worker();});}catch(...){stop();throw;}
}
Service::~Service(){stop();}
void Service::stop() noexcept {
    stopping_=true;++generation_;presence_condition_.notify_all();lookup_condition_.notify_all();
    if(presence_thread_.joinable())presence_thread_.join();
    if(lookup_thread_.joinable())lookup_thread_.join();
}
void Service::publish(Playback snapshot) {
    std::lock_guard lock(mutex_);
    const bool changed=snapshot.active!=snapshot_.active||snapshot.track.identity!=snapshot_.track.identity||!same_lookup_inputs(snapshot.track,snapshot_.track);
    if(changed) {
        key_=snapshot.active?lookup_key(snapshot.track):"";
        metadata_={};++generation_;lookup_status_=snapshot.active?"Looking up music":"Stopped";
        lookup_condition_.notify_all();
    }
    const bool immediate=changed||snapshot.paused!=snapshot_.paused||snapshot.clock_revision!=snapshot_.clock_revision;
    snapshot.track.cache_key=key_;snapshot_=std::move(snapshot);
    if(immediate){++presence_revision_;presence_condition_.notify_all();}
}
void Service::configure(Settings settings) {
    if(!valid_application_id(settings.custom_application_id))throw std::invalid_argument("Invalid custom application ID");
    std::lock_guard lock(mutex_);
    if(settings==settings_)return;
    if(settings.enabled!=settings_.enabled){++generation_;metadata_={};lookup_condition_.notify_all();}
    settings_=std::move(settings);++presence_revision_;presence_condition_.notify_all();
}
std::string Service::status() const {std::lock_guard lock(mutex_);return status_+" | "+lookup_status_;}
void Service::presence_worker() {
    DiscordRPC rpc(dependencies_.dial);PlaybackClock clock;PreparedActivity activity;
    Playback snapshot;Metadata metadata;Settings settings;
    std::uint64_t seen_revision=~std::uint64_t{};
    auto next_format=MonoClock::time_point{};
    std::string application;
    try {
        while(!stopping_) {
            bool rebuild=false;
            {
                std::lock_guard lock(mutex_);
                rebuild=seen_revision!=presence_revision_||(snapshot_.active&&!snapshot_.paused&&settings_.enabled&&MonoClock::now()>=next_format);
                if(rebuild){snapshot=snapshot_;metadata=metadata_;settings=settings_;seen_revision=presence_revision_;}
            }
            if(rebuild) {
                activity.replace(settings.enabled?make_activity(snapshot,metadata,clock):json(nullptr));
                if(!settings.enabled)clock.reset();
                application=effective_application_id(settings);
                next_format=MonoClock::now()+std::chrono::seconds(1);
            }
            rpc.tick(application,activity); // Connection checks reuse the already encoded payload.
            std::unique_lock lock(mutex_);status_=rpc.status();
            presence_condition_.wait_for(lock,std::chrono::milliseconds(200),[&]{return stopping_||presence_revision_!=seen_revision;});
        }
    } catch (...) {std::lock_guard lock(mutex_);status_="Presence worker stopped after an internal error";}
    rpc.shutdown();
}
void Service::lookup_worker() {
    MetadataCache cache(cache_folder_);MusicBrainz brainz(cache);
    std::uint64_t completed_generation=~std::uint64_t{};auto retry=MonoClock::time_point{};
    while(!stopping_) {
        Playback snapshot;std::uint64_t generation=0;
        {
            std::unique_lock lock(mutex_);
            for(;;) {
                if(stopping_)return;
                if(!settings_.enabled||!snapshot_.active) {
                    lookup_condition_.wait(lock,[&]{return stopping_||(settings_.enabled&&snapshot_.active);});
                    continue;
                }
                if(generation_.load()!=completed_generation||MonoClock::now()>=retry)break;
                lookup_condition_.wait_until(lock,retry,[&]{return stopping_||!settings_.enabled||!snapshot_.active||generation_.load()!=completed_generation;});
            }
            generation=generation_.load();snapshot=snapshot_;
        }
        auto cancelled=[this,generation]{return stopping_.load()||generation_.load()!=generation;};
        try {
            auto result=dependencies_.lookup?dependencies_.lookup(snapshot.track,cancelled):brainz.lookup_result(snapshot.track,cancelled);
            if(cancelled())continue;
            {
                std::lock_guard lock(mutex_);
                if(generation_.load()!=generation||!settings_.enabled||!snapshot_.active)continue;
                if(metadata_!=result.metadata){metadata_=std::move(result.metadata);++presence_revision_;presence_condition_.notify_all();}
                lookup_status_=metadata_.artwork_url.empty()?"No verified cover art":"MusicBrainz artwork ready";
                completed_generation=generation;
                retry=MonoClock::now()+std::chrono::seconds(std::max<std::int64_t>(1,result.expires-unix_now()));
            }
        } catch(const LookupCancelled&) {continue;}
        catch(const std::exception&) {
            std::lock_guard lock(mutex_);
            if(generation_.load()!=generation)continue;
            lookup_status_="Music metadata unavailable; will retry";completed_generation=generation;retry=MonoClock::now()+std::chrono::minutes(1);
        }
    }
}
}
