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
Service::Service(std::filesystem::path folder,Settings settings,ServiceDependencies dependencies):cache_folder_(std::move(folder)),dependencies_(std::move(dependencies)),diagnostics_(dependencies_.diagnostics),settings_(std::move(settings)) {
    if(!valid_application_id(settings_.custom_application_id))throw std::invalid_argument("Invalid custom application ID");
    diagnostics_.set_verbose(settings_.verbose_logging);
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
        retry_requested_=false;
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
    diagnostics_.set_verbose(settings.verbose_logging);
    settings_=std::move(settings);++presence_revision_;presence_condition_.notify_all();
}
std::string Service::status() const {std::lock_guard lock(mutex_);return status_+" | "+lookup_status_;}
bool Service::retry_current_track() {
    std::lock_guard lock(mutex_);
    if(stopping_||!settings_.enabled||!snapshot_.active)return false;
    retry_requested_=true;++generation_;lookup_status_="Retry requested for current track";
    lookup_condition_.notify_all();return true;
}
void Service::presence_worker() {
    DiscordRPC rpc(dependencies_.dial,{},diagnostics_);PlaybackClock clock;PreparedActivity activity;
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
            auto changed=[&]{return stopping_||presence_revision_!=seen_revision;};
            if(!snapshot_.active||!settings_.enabled)presence_condition_.wait(lock,changed);
            else presence_condition_.wait_for(lock,std::chrono::milliseconds(snapshot_.paused?500:200),changed);
        }
    } catch (...) {std::lock_guard lock(mutex_);status_="Presence worker stopped after an internal error";}
    rpc.shutdown();
}
void Service::lookup_worker() {
    MetadataCache cache(cache_folder_);MusicBrainz brainz(cache,{},{},diagnostics_);
    std::uint64_t completed_generation=~std::uint64_t{};auto retry=MonoClock::time_point{};
    while(!stopping_) {
        Playback snapshot;std::uint64_t generation=0;bool refresh=false;
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
            generation=generation_.load();snapshot=snapshot_;refresh=retry_requested_;retry_requested_=false;
        }
        auto cancelled=[this,generation]{return stopping_.load()||generation_.load()!=generation;};
        const auto scope="lookup #"+std::to_string(generation)+" "+bounded_text(snapshot.track.title.empty()?snapshot.track.fallback_title:snapshot.track.title,100)+" / "+bounded_text(snapshot.track.artist.empty()?snapshot.track.fallback_artist:snapshot.track.artist,100);
        diagnostics_.detail(scope,"Lookup started");
        try {
            auto result=dependencies_.lookup?dependencies_.lookup(snapshot.track,cancelled):brainz.lookup_result(snapshot.track,cancelled,refresh);
            if(cancelled())continue;
            {
                std::lock_guard lock(mutex_);
                if(generation_.load()!=generation||!settings_.enabled||!snapshot_.active)continue;
                if(metadata_!=result.metadata){metadata_=std::move(result.metadata);++presence_revision_;presence_condition_.notify_all();}
                if(!metadata_.artwork_url.empty())lookup_status_="MusicBrainz artwork ready";
                else if(!metadata_.release_id.empty()||!metadata_.release_group_id.empty())lookup_status_="Album identified; no hosted cover art";
                else if(!metadata_.title.empty()&&!metadata_.artist.empty())lookup_status_="Song identified; no unambiguous album";
                else lookup_status_="No verified music match";
                diagnostics_.info(scope,lookup_status_+(metadata_.album.empty()?"":"; selected album: "+metadata_.album)+
                    (metadata_.release_id.empty()?"":"; release="+metadata_.release_id)+(metadata_.release_group_id.empty()?"":"; group="+metadata_.release_group_id));
                if(!metadata_.artwork_url.empty())diagnostics_.detail(scope,"Artwork URL: "+metadata_.artwork_url);
                completed_generation=generation;
                retry=MonoClock::now()+std::chrono::seconds(std::max<std::int64_t>(1,result.expires-unix_now()));
            }
        } catch(const LookupCancelled&) {diagnostics_.detail(scope,"Lookup cancelled (playback/settings changed)");continue;}
        catch(const LookupFailure& error) {
            std::lock_guard lock(mutex_);
            if(generation_.load()!=generation||!settings_.enabled||!snapshot_.active)continue;
            if(!metadata_.complete) {
                auto partial=error.metadata;
                if(partial.title.empty())partial.title=metadata_.title;
                if(partial.artist.empty())partial.artist=metadata_.artist;
                if(metadata_!=partial){metadata_=std::move(partial);++presence_revision_;presence_condition_.notify_all();}
            }
            const auto seconds=std::max<std::int64_t>(1,error.retry_at-unix_now());
            lookup_status_=(!metadata_.title.empty()&&!metadata_.artist.empty()?"Song identified; artwork lookup unavailable: ":"Music metadata unavailable: ")+bounded_text(error.what())+"; will retry";
            diagnostics_.info(scope,lookup_status_+" in "+std::to_string(seconds)+" seconds");
            // Keep bypassing the same cached miss if an explicit refresh failed.
            retry_requested_=refresh;
            completed_generation=generation;retry=MonoClock::now()+std::chrono::seconds(seconds);
        }
        catch(const std::exception& error) {
            std::lock_guard lock(mutex_);
            if(generation_.load()!=generation)continue;
            diagnostics_.info(scope,"Lookup failed: "+std::string(error.what())+"; retry in 60 seconds");
            lookup_status_="Music metadata unavailable: "+bounded_text(error.what())+"; will retry";completed_generation=generation;retry=MonoClock::now()+std::chrono::minutes(1);
            retry_requested_=refresh;
        }
    }
}
}
