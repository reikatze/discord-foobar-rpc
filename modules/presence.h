#pragma once
#include "model.h"
namespace music_tracker {
class PlaybackClock {
public:
    json timestamps(const Playback& p, double duration);
    void reset();
private:
    std::string identity_;
    std::uint64_t revision_ = 0;
    double duration_ = 0;
    json timestamps_;
};
json make_activity(const Playback& p, const Metadata& m, PlaybackClock& clock);
}
