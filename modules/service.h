#pragma once
#include "model.h"
#include "discord.h"
#include "http.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
namespace music_tracker {
struct ServiceDependencies {
    DiscordRPC::Dial dial;
    std::function<LookupResult(const Track&,const Cancelled&)> lookup;
};
class Service {
public:
    Service(std::filesystem::path cache_folder,Settings settings,ServiceDependencies dependencies={});
    ~Service();
    void publish(Playback snapshot); // Main thread copies values; no foobar objects escape into workers.
    void configure(Settings settings);
    void stop() noexcept;
    std::string status() const;
private:
    void presence_worker();
    void lookup_worker();
    std::filesystem::path cache_folder_;
    ServiceDependencies dependencies_;
    mutable std::mutex mutex_;
    std::condition_variable presence_condition_,lookup_condition_;
    Settings settings_;
    Playback snapshot_;
    Metadata metadata_;
    std::string key_,status_="Starting",lookup_status_="Waiting for playback";
    std::atomic<bool> stopping_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::uint64_t presence_revision_=0;
    std::thread presence_thread_,lookup_thread_;
};
std::string effective_application_id(const Settings& settings);
}
