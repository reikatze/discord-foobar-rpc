#include "presence.h"
#include <algorithm>
#include <cmath>
namespace music_tracker {
void PlaybackClock::reset() { identity_.clear(); timestamps_=nullptr; duration_=0; }
json PlaybackClock::timestamps(const Playback& p, double duration) {
    if (!p.active || p.paused || !std::isfinite(duration) || duration <= 0 || duration > 31536000 ||
        !std::isfinite(p.position) || p.position < 0 || p.position > duration || !std::isfinite(p.epoch)) {
        reset(); return nullptr;
    }
    if (timestamps_.is_null() || identity_ != p.track.identity || revision_ != p.clock_revision || duration_ != duration) {
        identity_=p.track.identity;revision_=p.clock_revision;duration_=duration;
        timestamps_={{"start",static_cast<std::int64_t>(std::floor(p.epoch-p.position))},
            {"end",static_cast<std::int64_t>(std::floor(p.epoch+duration-p.position))}};
    }
    return timestamps_;
}
json make_activity(const Playback& p, const Metadata& m, PlaybackClock& clock) {
    if (!p.active) { clock.reset(); return nullptr; }
    auto title = !p.track.title.empty() ? p.track.title : !m.title.empty() ? m.title : p.track.fallback_title;
    auto artist = !p.track.artist.empty() ? p.track.artist : !m.artist.empty() ? m.artist : p.track.fallback_artist;
    if (title.empty()) title="Unknown track";
    if (artist.empty()) artist="Unknown artist";
    // Discord owns the third row/progress rendering; timestamps provide elapsed and remaining time.
    json activity={{"type",2},{"name",bounded_text(title)},{"status_display_type",2},
        {"details",bounded_text(title)},{"state",bounded_text(artist)}};
    double duration = p.track.duration > 0 ? p.track.duration : m.duration;
    auto stamps=clock.timestamps(p,duration);
    if (!stamps.is_null()) activity["timestamps"]=stamps;
    json assets=json::object();
    if (!m.artwork_url.empty()) {
        assets["large_image"]=m.artwork_url;
        auto album = p.track.album.empty() ? m.album : p.track.album;
        assets["large_text"]=bounded_text(album.empty() ? title : album);
    }
    // Badge keys are optional developer-portal assets; missing uploads do not affect text presence.
    assets["small_image"]=p.paused ? "pause" : "play";
    double position=p.position;
    if (!p.paused) position += std::max(0.0,std::chrono::duration<double>(MonoClock::now()-p.sampled).count());
    if (duration > 0 && std::isfinite(duration)) {
        position=std::clamp(position,0.0,duration);
        assets["small_text"]=bounded_text(std::string(p.paused ? "Paused · " : "")+time_text(position)+" elapsed · "+time_text(duration-position)+" remaining");
    } else assets["small_text"]=p.paused ? "Paused" : "Playing";
    activity["assets"]=assets;
    if (!m.page_url.empty()) activity["details_url"]=m.page_url;
    return activity;
}
}
