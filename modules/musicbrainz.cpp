#include "musicbrainz.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <thread>
namespace music_tracker {
namespace {
void check(const Cancelled& cancelled) {if(cancelled())throw LookupCancelled();}
void wait_until(MonoClock::time_point target,const Cancelled& cancelled) {
    while(MonoClock::now()<target){check(cancelled);std::this_thread::sleep_for(std::chrono::milliseconds(25));}check(cancelled);
}
std::string literal(const std::string& value) {
    std::string out="\"";
    for(char c:bounded_text(value,512)){if(std::string("+-&|!(){}[]^\"~*?:\\/").find(c)!=std::string::npos)out+='\\';out+=c;}
    return out+'"';
}
std::string text_field(const json& value,const char* name) {
    return value.is_object()&&value.contains(name)&&value[name].is_string()?value[name].get<std::string>():std::string{};
}
std::string id_of(const json& value) {
    if (!value.is_object())return {};
    auto id=text_field(value,"id");return valid_mbid(id)?id:"";
}
std::string credited(const json& value) {
    if(!value.is_object()||!value.contains("artist-credit"))return {};
    return artist_credit(value["artist-credit"]);
}
int score_of(const json& value) {
    try {if(value.at("score").is_string())return std::stoi(value.at("score").get<std::string>());return value.at("score").get<int>();}catch(...){return 0;}
}
bool equal_name(const std::string& a,const std::string& b) {return !a.empty() && normalized(a)==normalized(b);}
void recording_metadata(const json& value,Metadata& result) {
    if(!value.is_object())return;
    result.recording_id=id_of(value);result.title=value.value("title",std::string{});result.artist=credited(value);
    if(value.contains("length") && value["length"].is_number())result.duration=value["length"].get<double>()/1000.0;
}
const json* pick_recording(const json& rows,const Track& t,const std::function<void(const json&,const char*)>& rejected={}) {
    if(!rows.is_array())return nullptr;
    const auto title=normalized(t.title.empty()?t.fallback_title:t.title);
    const auto artist=normalized(t.artist),album=normalized(t.album);
    const json* best=nullptr;double best_score=-1,second=-1;
    for(const auto& row:rows) {
        const auto rank=score_of(row);
        auto reject=[&](const char* reason){if(rejected)rejected(row,reason);};
        if(!row.is_object()||id_of(row).empty()||rank<90){reject("invalid ID or search score below 90");continue;}
        if(title.empty()||title!=normalized(row.value("title",std::string{}))){reject("title mismatch");continue;}
        // Without an artist, require album or duration context and a uniquely stronger candidate.
        if(!artist.empty()&&artist!=normalized(credited(row))){reject("artist mismatch");continue;}
        if(t.artist.empty()&&t.album.empty()) {
            if(t.duration<=0||!row.contains("length")||!row["length"].is_number()||std::abs(row["length"].get<double>()/1000-t.duration)>5){reject("missing or conflicting duration evidence");continue;}
        }
        if(t.duration>0 && row.contains("length")&&row["length"].is_number() && std::abs(row["length"].get<double>()/1000-t.duration)>5){reject("duration mismatch");continue;}
        double score=rank;
        if(!t.album.empty()&&row.contains("releases")&&row["releases"].is_array()) {
            bool matches=false;for(const auto& r:row["releases"])if(!album.empty()&&album==normalized(r.value("title",std::string{})))matches=true;
            if(!matches){reject("album mismatch");continue;}
            score+=15;
        }
        if(score>best_score){second=best_score;best_score=score;best=&row;}else second=std::max(second,score);
    }
    if(second>=0 && best_score-second<3){if(rejected&&best)rejected(*best,"ambiguous recording scores");return nullptr;}
    return best;
}
// Recording identity can remain ambiguous while exactly one studio album is known.
// Live/single/compilation appearances do not have to contain that studio album.
std::string unique_studio_album(const std::vector<json>& rows) {
    std::set<std::string> albums;
    for(const auto& row:rows) {
        if(!equal_name(row.value("title",std::string{}),rows.front().value("title",std::string{}))||
           !equal_name(credited(row),credited(rows.front())))return {};
        if(row.contains("releases")&&row["releases"].is_array())for(const auto& release:row["releases"]) {
            if(!release.is_object()||normalized(text_field(release,"status"))!="official"||
               !release.contains("release-group")||!release["release-group"].is_object())continue;
            const auto& group=release["release-group"];
            auto type=text_field(group,"primary-type");if(type.empty())type=text_field(group,"type");
            if(normalized(type)!="album")continue;
            if(group.contains("secondary-types")&&(!group["secondary-types"].is_array()||!group["secondary-types"].empty()))continue;
            if(auto id=id_of(group);!id.empty())albums.insert(id);
        }
    }
    return albums.size()==1?*albums.begin():std::string{};
}

void find_release_track(const json& release,const Track& t,Metadata& result) {
    if(!release.contains("media")||!release["media"].is_array())return;
    const bool has_recording=valid_mbid(t.recording_id);
    const auto recording_id=normalized(t.recording_id),title=normalized(t.title),fallback=normalized(t.fallback_title);
    std::vector<const json*> candidates;
    for(const auto& medium:release["media"]) {
        if(t.disc_number && medium.value("position",0U)!=t.disc_number)continue;
        if(!medium.contains("tracks")||!medium["tracks"].is_array())continue;
        for(const auto& track:medium["tracks"]) {
            if(!track.is_object()||!track.contains("recording")||!track["recording"].is_object())continue;
            const auto& rec=track["recording"];
            bool match=false;
            if(has_recording)match=recording_id==normalized(id_of(rec));
            else if(!title.empty())match=title==normalized(track.value("title",rec.value("title",std::string{})));
            else if(t.track_number)match=track.value("position",0U)==t.track_number;
            else if(!fallback.empty())match=fallback==normalized(track.value("title",rec.value("title",std::string{})));
            if(!match)continue;
            if(t.track_number&&track.value("position",0U)!=t.track_number)continue;
            if(t.duration>0&&track.contains("length")&&track["length"].is_number()&&std::abs(track["length"].get<double>()/1000-t.duration)>5)continue;
            candidates.push_back(&track);
        }
    }
    if(candidates.size()!=1)return;
    const auto& track=*candidates.front();recording_metadata(track["recording"],result);
    result.title=track.value("title",result.title);
    const auto artist=credited(track);if(!artist.empty())result.artist=artist;
    if(result.artist.empty())result.artist=credited(release);
    if(track.contains("length")&&track["length"].is_number())result.duration=track["length"].get<double>()/1000;
}
}
namespace {
json fields(const json& input,std::initializer_list<const char*> names) {
    json out=json::object();if(!input.is_object())return out;
    for(auto name:names)if(input.contains(name))out[name]=input[name];
    return out;
}
json compact_release(const json& input,bool tracks) {
    if(input.is_null())return nullptr;
    auto out=fields(input,{"id","title","status","artist-credit","release-group"});
    if(tracks&&input.contains("media")&&input["media"].is_array()) {
        out["media"]=json::array();
        for(const auto& medium:input["media"]) {
            auto kept=fields(medium,{"position"});kept["tracks"]=json::array();
            if(medium.contains("tracks")&&medium["tracks"].is_array())for(const auto& track:medium["tracks"]) {
                auto item=fields(track,{"position","title","length","artist-credit"});
                if(track.contains("recording"))item["recording"]=fields(track["recording"],{"id","title","length","artist-credit"});
                kept["tracks"].push_back(std::move(item));
            }
            out["media"].push_back(std::move(kept));
        }
    }
    return out;
}
}
void MusicBrainz::remember(const std::string& key,json value,bool tracks,std::chrono::seconds ttl) {
    if(value.dump().size()>256*1024)return;
    for(auto it=entities_.begin();it!=entities_.end();)if(it->second.expires<=now())it=entities_.erase(it);else ++it;
    if(!entities_.contains(key)&&entities_.size()>=64) {
        auto oldest=std::min_element(entities_.begin(),entities_.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
        entities_.erase(oldest);
    }
    entities_[key]={std::move(value),tracks,now()+ttl,++cache_sequence_};
}
json MusicBrainz::release_detail(const std::string& id,bool tracks,const Cancelled& cancelled) {
    check(cancelled);auto key="release:"+id;
    if(auto it=entities_.find(key);it!=entities_.end()&&it->second.expires>now()&&(!tracks||it->second.tracks)) {
        it->second.used=++cache_sequence_;return it->second.value;
    }
    auto detail=request("release/"+id+(tracks?"?inc=recordings+artist-credits+release-groups&fmt=json":"?inc=artist-credits+release-groups&fmt=json"),cancelled);
    auto compact=compact_release(detail,tracks);
    if(!compact.is_null())remember(key,compact,tracks,std::chrono::hours(24));
    return compact;
}
json MusicBrainz::group_detail(const std::string& id,const Cancelled& cancelled) {
    check(cancelled);auto key="group:"+id;
    if(auto it=entities_.find(key);it!=entities_.end()&&it->second.expires>now()) {it->second.used=++cache_sequence_;return it->second.value;}
    auto response=request("release-group/"+id+"?fmt=json",cancelled);
    auto detail=response.is_object()?fields(response,{"id","title"}):json(nullptr);
    if(!detail.is_null())remember(key,detail,false,std::chrono::hours(24));
    return detail;
}
json MusicBrainz::album_search(const std::string& query,const Cancelled& cancelled) {
    check(cancelled);auto key="album:"+query;
    if(auto it=entities_.find(key);it!=entities_.end()&&it->second.expires>now()) {it->second.used=++cache_sequence_;return it->second.value;}
    auto data=request("release/?query="+url_encode(query)+"&limit=10&fmt=json",cancelled);
    if(data.is_object()&&data.contains("releases")&&data["releases"].is_array()) {
        json compact={{"releases",json::array()}};
        for(const auto& release:data["releases"]){compact["releases"].push_back(compact_release(release,false));if(compact["releases"].size()>=10)break;}
        remember(key,compact,false,compact["releases"].empty()?std::chrono::hours(1):std::chrono::hours(24));
        return compact;
    }
    return data;
}
HttpResponse MusicBrainz::get(const std::string& url,const Cancelled& cancelled) {
    diagnostics_.detail(log_scope_,"GET "+url);
    try {
        auto response=requester_?requester_(url,cancelled):http_.get(url,cancelled);
        diagnostics_.detail(log_scope_,"HTTP "+std::to_string(response.status)+" "+url);return response;
    } catch(const LookupCancelled&){throw;}
    catch(const std::exception& error){diagnostics_.info(log_scope_,"Request failed: "+std::string(error.what()));throw;}
}
json MusicBrainz::request(const std::string& path,const Cancelled& cancelled) {
    if(timing_.wait)timing_.wait(next_request_,cancelled);else wait_until(next_request_,cancelled);
    check(cancelled);
    // One serial lookup worker owns this limiter. Also retained across cancellations/new tracks.
    next_request_=now()+std::chrono::milliseconds(1100);
    auto response=get("https://musicbrainz.org/ws/2/"+path,cancelled);
    check(cancelled);
    if(response.status==404){diagnostics_.detail(log_scope_,"MusicBrainz entity not found (HTTP 404)");return nullptr;}
    if(response.status==429||response.status==503) {
        unsigned seconds=60;
        try {seconds=static_cast<unsigned>(std::clamp(std::stol(response.retry_after),1L,3600L));}catch(...){}
        next_request_=std::max(next_request_,now()+std::chrono::seconds(seconds));
        diagnostics_.info(log_scope_,"MusicBrainz HTTP "+std::to_string(response.status)+"; retry after "+std::to_string(seconds)+" seconds");
        throw std::runtime_error("MusicBrainz rate limited; retry later");
    }
    if(response.status!=200)throw std::runtime_error("MusicBrainz HTTP "+std::to_string(response.status));
    return json::parse(response.body);
}
std::string MusicBrainz::cover(const std::string& kind,const std::string& id,const Cancelled& cancelled) {
    if(!valid_mbid(id))return {};
    const auto entity=kind+":"+id;
    if(auto cached=cache_.artwork(entity)){diagnostics_.detail(log_scope_,"Artwork cache hit: "+entity+(cached->empty()?" (no cover)":" URL="+*cached));return *cached;}
    auto response=get("https://coverartarchive.org/"+kind+"/"+id,cancelled);
    check(cancelled);
    if(response.status==404){diagnostics_.info(log_scope_,"No hosted cover: "+entity+" (HTTP 404)");cache_.put_artwork(entity,"");return {};}
    if(response.status!=200)throw std::runtime_error("Cover Art Archive HTTP "+std::to_string(response.status));
    auto data=json::parse(response.body);std::string url;
    if(data.contains("images")&&data["images"].is_array()) {
        for(const auto& image:data["images"]) {
            if(!image.is_object()||!image.value("front",false))continue;
            if(image.contains("thumbnails")&&image["thumbnails"].is_object())
                url=image["thumbnails"].value("500",image["thumbnails"].value("large",std::string{}));
            if(url.empty())url=image.value("image",std::string{});
            // Older archive records use http URLs, but the same archive endpoints support HTTPS.
            if(url.starts_with("http://"))url.replace(0,7,"https://");
            if(public_https_url(url))break;
            url.clear();
        }
    }
    diagnostics_.detail(log_scope_,url.empty()?"No usable HTTPS front cover: "+entity:"Selected artwork URL: "+url);
    cache_.put_artwork(entity,url);return url;
}
Metadata MusicBrainz::lookup(const Track& t,const Cancelled& cancelled) {return lookup_result(t,cancelled).metadata;}
LookupResult MusicBrainz::lookup_result(const Track& t,const Cancelled& cancelled) {
    log_scope_="MusicBrainz #"+std::to_string(++lookup_sequence_)+" "+bounded_text(t.title.empty()?t.fallback_title:t.title,100)+" / "+bounded_text(t.artist.empty()?t.fallback_artist:t.artist,100);
    check(cancelled);const auto key=t.cache_key.empty()?lookup_key(t):t.cache_key;
    if(auto cached=cache_.get_record(key)){diagnostics_.detail(log_scope_,"Metadata cache hit");return *cached;}
    unsigned rejection_count=0;
    auto rejected=[&](const json& row,const char* reason){++rejection_count;diagnostics_.detail(log_scope_,"Rejected candidate "+id_of(row)+" "+text_field(row,"title")+" / "+credited(row)+": "+reason);};
    auto batch=cache_.batch();
    Metadata result;json recording=nullptr,release=nullptr;
    if(valid_mbid(t.release_id))result.release_id=t.release_id;
    if(valid_mbid(t.release_group_id))result.release_group_id=t.release_group_id;
    if(valid_mbid(t.recording_id)) {
        result.recording_id=t.recording_id;
        if(t.title.empty()||t.artist.empty()||t.duration<=0||(result.release_id.empty()&&result.release_group_id.empty())) {
            recording=request("recording/"+url_encode(t.recording_id)+"?inc=artists+releases&fmt=json",cancelled);
            if(!recording.is_null())recording_metadata(recording,result);else result.recording_id.clear();
        }
    }
    const auto album=normalized(t.album),album_artist=normalized(t.album_artist.empty()?t.artist:t.album_artist);
    if(result.release_id.empty()&&recording.is_object()&&recording.contains("releases")&&recording["releases"].is_array()) {
        for(const auto& candidate:recording["releases"]) {
            if(!candidate.is_object()||(!album.empty()&&normalized(candidate.value("title",std::string{}))!=album))continue;
            if(auto id=id_of(candidate);!id.empty()){result.release_id=id;break;}
        }
    }
    // Album lookup can recover a missing song/artist using a tagged track/disc number.
    if(result.release_id.empty()&&!t.album.empty()&&!album_artist.empty()) {
        auto data=album_search("release:"+literal(t.album)+" AND artist:"+literal(t.album_artist.empty()?t.artist:t.album_artist),cancelled);
        std::vector<json> candidates;
        if(data.is_object()&&data.contains("releases")&&data["releases"].is_array())for(const auto& row:data["releases"]) {
            if(score_of(row)<90||album!=normalized(row.value("title",std::string{}))||album_artist!=normalized(credited(row))||id_of(row).empty())continue;
            if(row.value("status",std::string{})!="Official")continue;
            candidates.push_back(row);
        }
        // Same album editions may share artwork. Select a release only when unique; otherwise keep group identity.
        if(candidates.size()==1)result.release_id=id_of(candidates.front());
        else if(!candidates.empty()) {
            std::string group;bool same=true;
            for(const auto& row:candidates) {
                auto id=row.contains("release-group")?id_of(row["release-group"]):"";
                if(id.empty()){same=false;break;}if(group.empty())group=id;else if(group!=id)same=false;
            }
            if(same&&result.release_group_id.empty())result.release_group_id=group;
        }
        // If title is missing, evaluate a bounded set of album editions; accept only agreeing track identities.
        if(t.title.empty() && result.title.empty() && t.track_number && !candidates.empty()) {
            Metadata agreed;bool same=true;unsigned examined=0;
            for(const auto& row:candidates) {
                if(++examined>3){same=false;break;}
                auto detail=release_detail(id_of(row),true,cancelled);
                Metadata candidate;find_release_track(detail,t,candidate);
                if(candidate.title.empty()||candidate.recording_id.empty()){same=false;break;}
                if(agreed.title.empty()){agreed=candidate;release=detail;}
                else if(!equal_name(agreed.recording_id,candidate.recording_id)){same=false;break;}
            }
            if(same&&!agreed.title.empty()) {result.title=agreed.title;result.artist=agreed.artist;result.recording_id=agreed.recording_id;result.duration=agreed.duration;}
            else release=nullptr;
        }
    }
    if(recording.is_null() && result.recording_id.empty() && ((result.release_id.empty()&&result.release_group_id.empty())||t.title.empty()||t.artist.empty()||t.duration<=0) && (!t.title.empty()||!t.fallback_title.empty())) {
        Track search=t;
        if(search.title.empty())search.title=t.fallback_title;
        if(search.artist.empty())search.artist=t.fallback_artist;
        const bool filename_orders=t.title.empty()&&t.artist.empty()&&
            !t.fallback_title.empty()&&!t.fallback_artist.empty();
        // Tags retain their meaning. Only an untagged filename permits reversal.
        std::vector<Track> searches{search};
        if(filename_orders&&!equal_name(search.title,search.artist)) {
            std::swap(search.title,search.artist);
            searches.push_back(search);
        }
        json verified=nullptr;
        std::map<std::string,json> exact, duration_matches;
        for(const auto& interpretation:searches) {
            std::string query="recording:"+literal(interpretation.title);
            if(!interpretation.artist.empty())query+=" AND artist:"+literal(interpretation.artist);
            if(!t.album.empty())query+=" AND release:"+literal(t.album);
            auto data=request("recording/?query="+url_encode(query)+"&limit=10&fmt=json",cancelled);
            if(!data.is_object()||!data.contains("recordings")||!data["recordings"].is_array())continue;
            if(!filename_orders) {
                if(auto match=pick_recording(data["recordings"],interpretation,rejected))verified=*match;
                continue;
            }
            // Compare exact names first; duration is evidence, not a video-edit veto.
            for(const auto& row:data["recordings"]) {
                auto names=interpretation;names.duration=0;
                auto single=json::array({row});
                if(!pick_recording(single,names,rejected))continue;
                const auto id=id_of(row);
                exact.emplace(id,row);
                if(!std::isfinite(t.duration)||t.duration<=0)duration_matches.emplace(id,row);
                else if(row.contains("length")&&row["length"].is_number()) {
                    const auto seconds=row["length"].get<double>()/1000.0;
                    if(std::isfinite(seconds)&&std::abs(seconds-t.duration)<=5)duration_matches.emplace(id,row);
                    else if(!t.filename_has_video_hint)rejected(row,"duration mismatch");
                    else diagnostics_.detail(log_scope_,"Candidate "+id+": video duration differs; retaining exact names");
                } else if(!t.filename_has_video_hint)rejected(row,"missing duration evidence");
            }
        }
        if(filename_orders) {
            auto pool=t.filename_has_video_hint?exact:duration_matches;
            if(t.filename_has_video_hint&&!duration_matches.empty()) {
                // Video runtime can establish the name order, but a live rendition
                // of that length must not hide the song's original studio album.
                const auto& evidence=duration_matches.begin()->second;
                bool one_order=true;
                for(const auto& entry:duration_matches)
                    if(!equal_name(entry.second.value("title",std::string{}),evidence.value("title",std::string{}))||
                       !equal_name(credited(entry.second),credited(evidence)))one_order=false;
                if(one_order)for(auto it=pool.begin();it!=pool.end();) {
                    if(!equal_name(it->second.value("title",std::string{}),evidence.value("title",std::string{}))||
                       !equal_name(credited(it->second),credited(evidence)))it=pool.erase(it);
                    else ++it;
                }
            }
            bool same_names=!pool.empty();
            for(const auto& entry:pool)if(!equal_name(entry.second.value("title",std::string{}),pool.begin()->second.value("title",std::string{}))||
                !equal_name(credited(entry.second),credited(pool.begin()->second)))same_names=false;
            bool albums_complete=true;unsigned details_left=6;
            if(same_names) {
                // Search responses can omit release status/type information.
                for(auto& entry:pool)if(entry.second.contains("releases")&&entry.second["releases"].is_array())
                    for(auto& edition:entry.second["releases"]) {
                        if(!edition.is_object()||id_of(edition).empty())continue;
                        const bool missing=!edition.contains("status")||!edition.contains("release-group")||
                            !edition["release-group"].is_object()||
                            (!edition["release-group"].contains("primary-type")&&!edition["release-group"].contains("type"));
                        if(!missing)continue;
                        if(!details_left){albums_complete=false;continue;}
                        --details_left;
                        auto detail=release_detail(id_of(edition),false,cancelled);
                        if(detail.is_object())edition.update(detail);
                        if(!detail.is_object()||text_field(edition,"status").empty()||!edition.contains("release-group")||
                           (text_field(edition["release-group"],"primary-type").empty()&&text_field(edition["release-group"],"type").empty()))albums_complete=false;
                    }
                result.title=pool.begin()->second.value("title",std::string{});
                result.artist=credited(pool.begin()->second);
            }

            if(pool.size()==1)verified=pool.begin()->second;
            else if(!pool.empty()) {
                std::vector<json> candidates;for(const auto& entry:pool)candidates.push_back(entry.second);
                const auto album_id=albums_complete?unique_studio_album(candidates):std::string{};
                if(!album_id.empty()) {
                    result.title=candidates.front().value("title",std::string{});
                    result.artist=credited(candidates.front());
                    result.release_group_id=album_id;
                    diagnostics_.detail(log_scope_,"Selected unique studio album group "+album_id);
                    // No recording ID or remote duration is invented for agreeing editions.
                }
            }
        }
        if(!verified.is_null()) {
            recording=verified;recording_metadata(recording,result);
            if(filename_orders&&t.album.empty()&&result.release_group_id.empty()) {
                const auto studio=unique_studio_album({recording});
                if(!studio.empty()) {
                    result.release_group_id=studio;
                    json editions=json::array();
                    for(const auto& edition:recording["releases"])
                        if(edition.is_object()&&edition.contains("release-group")&&id_of(edition["release-group"])==studio)
                            editions.push_back(edition);
                    recording["releases"]=std::move(editions);
                }
            }
        }
    }
    if(rejection_count)diagnostics_.info(log_scope_,"Rejected "+std::to_string(rejection_count)+" search candidates");
    // Resolve releases belonging to the verified recording, preferring a tagged album.
    std::vector<std::string> artwork_releases;
    if(!result.release_id.empty())artwork_releases.push_back(result.release_id);
    if(recording.is_null()&&!result.recording_id.empty()&&((result.release_id.empty()&&result.release_group_id.empty())||t.title.empty()||t.artist.empty()||t.duration<=0)) {
        recording=request("recording/"+result.recording_id+"?inc=artists+releases&fmt=json",cancelled);
        if(recording.is_object()) {
            if(result.title.empty())result.title=recording.value("title",std::string{});
            if(result.artist.empty())result.artist=credited(recording);
        }
    }
    if(recording.is_object()&&recording.contains("releases")&&recording["releases"].is_array()) {
        for(const auto& row:recording["releases"]) {
            if(!row.is_object()||(!t.album.empty()&&album!=normalized(row.value("title",std::string{}))))continue;
            auto id=id_of(row);if(id.empty()||std::find(artwork_releases.begin(),artwork_releases.end(),id)!=artwork_releases.end())continue;
            artwork_releases.push_back(id);if(artwork_releases.size()>=3)break;
        }
    }
    if(result.release_id.empty()&&!artwork_releases.empty())result.release_id=artwork_releases.front();
    if(!result.release_id.empty()) {
        if(release.is_null()||id_of(release)!=result.release_id)
            release=release_detail(result.release_id,(t.title.empty()&&result.title.empty())||(t.artist.empty()&&result.artist.empty())||(t.duration<=0&&result.duration<=0),cancelled);
        if(release.is_object()) {
            result.album=release.value("title",std::string{});
            if(result.release_group_id.empty()&&release.contains("release-group"))result.release_group_id=id_of(release["release-group"]);
            if((t.title.empty()&&result.title.empty())||(t.artist.empty()&&result.artist.empty())||(t.duration<=0&&result.duration<=0))find_release_track(release,t,result);
        }
    }
    // A release-group ID still goes through MusicBrainz even when all local fields are complete.
    if(!result.release_group_id.empty()&&release.is_null()) {
        auto group=group_detail(result.release_group_id,cancelled);
        if(group.is_object())result.album=group.value("title",std::string{});
    }
    bool artwork_failed=false;
    for(const auto& id:artwork_releases) {
        try {result.artwork_url=cover("release",id,cancelled);}catch(const LookupCancelled&){throw;}catch(const std::exception& error){diagnostics_.info(log_scope_,"Artwork lookup failed: "+std::string(error.what()));artwork_failed=true;}
        if(!result.artwork_url.empty())break;
    }
    if(result.artwork_url.empty()&&!result.release_group_id.empty()) {
        try {result.artwork_url=cover("release-group",result.release_group_id,cancelled);}catch(const LookupCancelled&){throw;}catch(const std::exception& error){diagnostics_.info(log_scope_,"Artwork lookup failed: "+std::string(error.what()));artwork_failed=true;}
    }
    if(!result.recording_id.empty())result.page_url="https://musicbrainz.org/recording/"+result.recording_id;
    else if(!result.release_id.empty())result.page_url="https://musicbrainz.org/release/"+result.release_id;
    else if(!result.release_group_id.empty())result.page_url="https://musicbrainz.org/release-group/"+result.release_group_id;
    result.complete=!result.artwork_url.empty()&&(!t.title.empty()||!result.title.empty())&&(!t.artist.empty()||!result.artist.empty());
    check(cancelled);
    // Never turn a transient artwork outage into a long-lived negative result.
    auto ttl=result.complete?60LL*86400:result.page_url.empty()?3600LL:600LL;
    if(artwork_failed&&result.artwork_url.empty())ttl=60;
    const auto expires=cache_.put(key,result,ttl);return {std::move(result),expires};
}
}
