#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>
namespace music_tracker {
using json = nlohmann::json;
using MonoClock = std::chrono::steady_clock;
struct Settings {
    bool enabled = true;
    std::string custom_application_id; // Empty means built-in. Never store the built-in ID here.
    bool operator==(const Settings&) const = default;
};
struct Track {
    std::string identity, title, artist, album, album_artist, fallback_title, fallback_artist;
    std::string recording_id, release_id, release_group_id;
    unsigned track_number = 0, disc_number = 0;
    double duration = 0;
    bool filename_has_video_hint = false; // Video edits need not have the album recording length.
    std::string cache_key; // Computed once by Service when lookup inputs change.
};
struct Playback {
    Track track;
    bool active = false, paused = false;
    double position = 0;
    double epoch = 0;
    MonoClock::time_point sampled = MonoClock::now();
    std::uint64_t clock_revision = 0;
};
struct Metadata {
    std::string title, artist, album, recording_id, release_id, release_group_id;
    std::string artwork_url, page_url;
    double duration = 0;
    bool complete = false;
    bool operator==(const Metadata&) const = default;
};
struct LookupResult {
    Metadata metadata;
    std::int64_t expires = 0;
    LookupResult(Metadata value={},std::int64_t expiry=0);
};
std::int64_t unix_now();
double epoch_now();
std::string bounded_text(std::string text, std::size_t bytes = 120);
std::string normalized(std::string value);
bool valid_mbid(const std::string& value);
bool valid_application_id(const std::string& value);
std::string lookup_key(const Track& track);
bool same_lookup_inputs(const Track& a,const Track& b);
void apply_filename_fallback(Track& track,const std::string& filename);
Track merge_dynamic_track(const Track& previous,Track incoming);
std::string artist_credit(const json& value);
std::string time_text(double seconds);
void to_json(json& value, const Metadata& metadata);
void from_json(const json& value, Metadata& metadata);
}
