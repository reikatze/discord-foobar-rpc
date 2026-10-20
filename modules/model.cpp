#include "model.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <regex>
#include <sstream>
#include <tuple>
namespace music_tracker {
std::int64_t unix_now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
double epoch_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string bounded_text(std::string text, std::size_t bytes) {
    for (char& c : text) if (static_cast<unsigned char>(c) < 32) c = ' ';
    if (text.size() > bytes) {
        auto end = bytes;
        while (end && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
        text.resize(end);
    }
    return text;
}
std::string normalized(std::string value) {
    std::string out; bool space = false;
    for (unsigned char c : value) {
        if (std::isspace(c)) { space = !out.empty(); continue; }
        if (space) out += ' ';
        space = false; out += static_cast<char>(c < 128 ? std::tolower(c) : c);
    }
    return out;
}
bool valid_mbid(const std::string& s) {
    if (s.size() != 36) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}
bool valid_application_id(const std::string& s) {
    if (s.empty()) return true;
    if (s.size() > 20 || s[0] == '0') return false;
    std::uint64_t value = 0;
    for (unsigned char c : s) {
        if (c < '0' || c > '9') return false;
        auto digit = static_cast<unsigned>(c - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value != 0;
}
void apply_filename_fallback(Track& t,const std::string& filename) {
    auto trim=[](std::string value) {
        auto first=value.find_first_not_of(" \t\r\n"),last=value.find_last_not_of(" \t\r\n");
        return first==std::string::npos?std::string{}:value.substr(first,last-first+1);
    };
    auto stem=trim(filename);
    auto dot=stem.find_last_of('.');
    if(dot!=std::string::npos) {
        const auto extension=normalized(stem.substr(dot));
        for(const char* known:{".webm",".mkv",".mp4",".mp3",".flac",".ogg",".opus",".wav",".m4a",".aac",".wma",".aiff",".aif",".mov",".avi",".m4v",".alac",".ape",".wv",".mka",".aifc",".dsf",".dff",".m4b",".mp2",".oga"})
            if(extension==known){stem.resize(dot);break;}
    }
    t.fallback_title=stem;t.fallback_artist.clear();t.filename_has_video_hint=false;
    const auto original_stem=stem;
    // Strip explicit track/disc prefixes, keeping names such as 1979 and 311 intact.
    static const std::regex prefixes[]{
        std::regex(R"(^(?:(?:CD|Disc) ?)?[1-9][0-9]?[-.][0-9]{2,3}[ ._-]+(.+)$)",std::regex::icase),
        std::regex(R"(^\[[0-9]{1,3}\][ ._-]+(.+)$)"),
        std::regex(R"(^[0-9]{1,3}[.)][ 	]+(.+)$)"),
        std::regex(R"(^0[0-9]{1,2}(?:[ 	]+-[ 	]+|[ 	]+)(.+)$)")
    };
    for(const auto& prefix:prefixes) {
        std::smatch match;
        if(std::regex_match(stem,match,prefix)){stem=trim(match[1].str());break;}
    }
    bool video_hint=false,title_label=false;
    auto clean_part=[&](std::string value) {
        // Decorations belong to either side of Artist - Song / Song - Artist.
        for(bool changed=true;changed;) {
            changed=false;
            if(value.size()>=14&&value.ends_with("]")&&value[value.size()-14]==' '&&value[value.size()-13]=='[') {
                const auto id=value.substr(value.size()-12,11);
                if(std::all_of(id.begin(),id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';})) {
                    value=trim(value.substr(0,value.size()-14));video_hint=true;changed=true;continue;
                }
            }
            auto lower=value;
            std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(c<128?std::tolower(c):c);});
            for(const char* suffix:{" (official video)"," (official music video)"," (official audio)"," (official lyric video)"," (lyric video)"," (lyrics)"," [official video]"," [official music video]"," [official audio]"," [official lyric video]"," [lyric video]"," [lyrics]"}) {
                const std::string ending=suffix;
                if(lower.ends_with(ending)) {value=trim(value.substr(0,value.size()-ending.size()));video_hint=true;title_label=true;changed=true;break;}
            }
            if(changed)continue;
            for(const char* suffix:{" [hd]"," [hq]"," [4k]"," [1080p]"," [720p]"," [320kbps]"," [flac]"}) {
                const std::string ending=suffix;
                if(lower.ends_with(ending)) {value=trim(value.substr(0,value.size()-ending.size()));changed=true;break;}
            }
        }
        return value;
    };
    // Require a delimited separator; bare hyphens and underscores belong to names.
    std::size_t separator=std::string::npos,width=0;
    for(const std::string delimiter:{" - "," \xE2\x80\x93 "," \xE2\x80\x94 ","_-_"}) {
        auto found=stem.find(delimiter);
        if(found<separator){separator=found;width=delimiter.size();}
    }
    // Some containers store the download filename as TITLE. Real tags stay authoritative.
    const bool placeholder=t.artist.empty()&&!t.title.empty()&&
        (normalized(t.title)==normalized(filename)||normalized(t.title)==normalized(original_stem));
    if(separator==std::string::npos) {
        const auto title=clean_part(trim(stem));
        if(title.empty()||(!t.title.empty()&&!placeholder&&normalized(t.title)!=normalized(title)))return;
        if(placeholder)t.title.clear();
        t.fallback_title=title;t.filename_has_video_hint=video_hint;return;
    }
    auto artist=clean_part(trim(stem.substr(0,separator)));
    const bool left_title_label=title_label;title_label=false;
    auto title=clean_part(trim(stem.substr(separator+width)));
    // A title-specific video/lyric label gives an immediate orientation hint.
    // An arbitrary ID or quality label alone does not establish the order.
    if(left_title_label&&!title_label)std::swap(artist,title);
    if(artist.empty()||title.empty())return;
    auto compatible=[&](const std::string& candidate_artist,const std::string& candidate_title) {
        return (t.title.empty()||placeholder||normalized(t.title)==normalized(candidate_title))&&
            (t.artist.empty()||normalized(t.artist)==normalized(candidate_artist));
    };
    if(!compatible(artist,title)) {
        if(!compatible(title,artist))return;
        std::swap(artist,title);
    }
    if(placeholder)t.title.clear();
    t.fallback_title=title;t.fallback_artist=artist;t.filename_has_video_hint=video_hint;
}
std::string lookup_key(const Track& t) {
    // Full canonical key is retained in cache records to detect any filename hash collision.
    // Recheck filename results cached before the album-recording fallback.
    const bool filename=t.title.empty()&&t.artist.empty()&&!t.fallback_title.empty()&&!t.fallback_artist.empty();
    const auto version=filename?"v6-album-recordings":"v2";
    auto key=json::array({version, normalized(t.recording_id), normalized(t.release_id), normalized(t.release_group_id),
        normalized(t.title), normalized(t.artist), normalized(t.album), normalized(t.album_artist),
        t.track_number, t.disc_number, std::isfinite(t.duration) && t.duration > 0 ? static_cast<long long>(t.duration) : 0,
        t.title.empty() ? normalized(t.fallback_title) : "",t.artist.empty()?normalized(t.fallback_artist):""});
    if(filename)key.push_back(t.filename_has_video_hint);
    return key.dump();
}
LookupResult::LookupResult(Metadata value,std::int64_t expiry):metadata(std::move(value)),expires(expiry?expiry:unix_now()+60LL*86400) {}
bool same_lookup_inputs(const Track& a,const Track& b) {
    return std::tie(a.recording_id,a.release_id,a.release_group_id,a.title,a.artist,a.album,a.album_artist,a.track_number,a.disc_number,a.duration,a.fallback_title,a.fallback_artist,a.filename_has_video_hint)==
        std::tie(b.recording_id,b.release_id,b.release_group_id,b.title,b.artist,b.album,b.album_artist,b.track_number,b.disc_number,b.duration,b.fallback_title,b.fallback_artist,b.filename_has_video_hint);
}
Track merge_dynamic_track(const Track& previous,Track incoming) {
    const bool same_file=incoming.identity==previous.identity;
    const bool changed=(!incoming.title.empty()&&normalized(incoming.title)!=normalized(previous.title))||
        (!incoming.artist.empty()&&normalized(incoming.artist)!=normalized(previous.artist))||
        (!incoming.recording_id.empty()&&normalized(incoming.recording_id)!=normalized(previous.recording_id))||
        (!incoming.release_id.empty()&&normalized(incoming.release_id)!=normalized(previous.release_id));
    if(!same_file||changed)return incoming; // Never inherit song-specific fields/IDs across stream songs.
    auto fill=[](std::string& value,const std::string& old){if(value.empty())value=old;};
    fill(incoming.title,previous.title);fill(incoming.artist,previous.artist);
    fill(incoming.album,previous.album);fill(incoming.album_artist,previous.album_artist);
    fill(incoming.recording_id,previous.recording_id);fill(incoming.release_id,previous.release_id);fill(incoming.release_group_id,previous.release_group_id);
    if(!incoming.track_number)incoming.track_number=previous.track_number;
    if(!incoming.disc_number)incoming.disc_number=previous.disc_number;
    if(incoming.duration<=0)incoming.duration=previous.duration;
    return incoming;
}
std::string artist_credit(const json& v) {
    std::string out;
    if (!v.is_array()) return out;
    for (const auto& credit : v) {
        if (!credit.is_object()) continue;
        auto name=credit.value("name",std::string{});
        if(name.empty()&&credit.contains("artist")&&credit["artist"].is_object())name=credit["artist"].value("name",std::string{});
        out+=name;
        out += credit.value("joinphrase", std::string{});
    }
    return out;
}
std::string time_text(double seconds) {
    auto n = static_cast<long long>(std::isfinite(seconds) ? std::clamp(seconds, 0.0, 31536000.0) : 0);
    std::ostringstream s;
    if (n >= 3600) s << n / 3600 << ':' << std::setw(2) << std::setfill('0') << (n / 60) % 60;
    else s << n / 60;
    s << ':' << std::setw(2) << std::setfill('0') << n % 60; return s.str();
}
void to_json(json& j, const Metadata& m) {
    j = {{"title",m.title},{"artist",m.artist},{"album",m.album},{"recording_id",m.recording_id},
        {"release_id",m.release_id},{"release_group_id",m.release_group_id},{"artwork_url",m.artwork_url},
        {"page_url",m.page_url},{"duration",m.duration},{"complete",m.complete}};
}
void from_json(const json& j, Metadata& m) {
    m.title=j.value("title",std::string{});m.artist=j.value("artist",std::string{});m.album=j.value("album",std::string{});
    m.recording_id=j.value("recording_id",std::string{});m.release_id=j.value("release_id",std::string{});
    m.release_group_id=j.value("release_group_id",std::string{});m.artwork_url=j.value("artwork_url",std::string{});
    m.page_url=j.value("page_url",std::string{});m.duration=j.value("duration",0.0);m.complete=j.value("complete",false);
}
}
