#include "model.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
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
        for(const char* known:{".webm",".mkv",".mp4",".mp3",".flac",".ogg",".opus",".wav",".m4a",".aac",".wma",".aiff",".aif",".mov",".avi",".m4v"})
            if(extension==known){stem.resize(dot);break;}
    }
    t.fallback_title=stem;t.fallback_artist.clear();
    auto clean=stem;
    // Strip only the conventional trailing 11-character video ID.
    if(clean.size()>=14&&clean.ends_with("]")&&clean[clean.size()-14]==' '&&clean[clean.size()-13]=='[') {
        auto id=clean.substr(clean.size()-12,11);
        if(std::all_of(id.begin(),id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';}))
            clean=trim(clean.substr(0,clean.size()-14));
    }
    for(bool changed=true;changed;) {
        changed=false;
        for(const char* suffix:{" (official video)"," (official music video)"," (official audio)"," (lyric video)"," (lyrics)"," [official video]"," [official music video]"," [official audio]"}) {
            const std::string ending=suffix;
            if(normalized(clean).ends_with(ending)) {clean=trim(clean.substr(0,clean.size()-ending.size()));changed=true;break;}
        }
    }
    const auto separator=clean.find(" - ");
    if(separator==std::string::npos)return;
    auto artist=trim(clean.substr(0,separator)),title=trim(clean.substr(separator+3));
    if(artist.empty()||title.empty())return;
    // Some containers store the download filename as TITLE. Only demote that
    // exact placeholder when no artist tag exists; actual tags keep precedence.
    const bool placeholder=t.artist.empty()&&!t.title.empty()&&
        (normalized(t.title)==normalized(filename)||normalized(t.title)==normalized(stem));
    if(!t.title.empty()&&!placeholder&&normalized(t.title)!=normalized(title))return;
    if(!t.artist.empty()&&normalized(t.artist)!=normalized(artist))return;
    if(placeholder)t.title.clear();
    t.fallback_title=title;t.fallback_artist=artist;
}
std::string lookup_key(const Track& t) {
    // Full canonical key is retained in cache records to detect any filename hash collision.
    // Recheck filename results cached before both orders were verified.
    const auto version=t.title.empty()&&t.artist.empty()&&!t.fallback_title.empty()&&
        !t.fallback_artist.empty()?"v3-filename-orders":"v2";
    return json::array({version, normalized(t.recording_id), normalized(t.release_id), normalized(t.release_group_id),
        normalized(t.title), normalized(t.artist), normalized(t.album), normalized(t.album_artist),
        t.track_number, t.disc_number, std::isfinite(t.duration) && t.duration > 0 ? static_cast<long long>(t.duration) : 0,
        t.title.empty() ? normalized(t.fallback_title) : "",t.artist.empty()?normalized(t.fallback_artist):""}).dump();
}
LookupResult::LookupResult(Metadata value,std::int64_t expiry):metadata(std::move(value)),expires(expiry?expiry:unix_now()+60LL*86400) {}
bool same_lookup_inputs(const Track& a,const Track& b) {
    return std::tie(a.recording_id,a.release_id,a.release_group_id,a.title,a.artist,a.album,a.album_artist,a.track_number,a.disc_number,a.duration,a.fallback_title,a.fallback_artist)==
        std::tie(b.recording_id,b.release_id,b.release_group_id,b.title,b.artist,b.album,b.album_artist,b.track_number,b.disc_number,b.duration,b.fallback_title,b.fallback_artist);
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
