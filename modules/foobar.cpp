#include <foobar2000/SDK/foobar2000.h>
#include "foobar.h"
#include "service.h"
#include "settings.h"
#include <cmath>
#include <memory>
namespace music_tracker {
namespace {
std::unique_ptr<Service> service;
std::string joined_tag(const file_info& info,const char* name) {
    std::string out;
    for(t_size i=0;i<info.meta_get_count_by_name(name);++i) {
        const auto* value=info.meta_get(name,i);if(!value||!*value)continue;
        if(!out.empty())out+=", ";out+=value;
    }
    return bounded_text(out,512);
}
std::string single_tag(const file_info& info,const char* name) {auto value=info.meta_get(name,0);return value?bounded_text(value,512):"";}
unsigned number_tag(const file_info& info,const char* name) {
    try {auto n=std::stoul(single_tag(info,name));return n<=9999?static_cast<unsigned>(n):0;}catch(...){return 0;}
}
class PlaybackEvents final : public play_callback_impl_base {
public:
    PlaybackEvents():play_callback_impl_base(flag_on_playback_new_track|flag_on_playback_stop|flag_on_playback_seek|flag_on_playback_pause|flag_on_playback_edited|flag_on_playback_dynamic_info_track|flag_on_playback_time) {refresh_track();}
    void refresh_settings(){if(service)service->configure(read_settings());publish();}
private:
    Playback snapshot_;
    void read_track(metadb_handle_ptr item,const file_info* dynamic=nullptr) {
        auto identity=std::string(item->get_path())+"#"+std::to_string(item->get_subsong_index());
        Track t;t.identity=std::move(identity);
        file_info_impl stored;const file_info* info=dynamic;
        if(!info&&item->get_info(stored))info=&stored;
        if(info) {
            t.title=single_tag(*info,"TITLE");t.artist=joined_tag(*info,"ARTIST");t.album=single_tag(*info,"ALBUM");t.album_artist=joined_tag(*info,"ALBUM ARTIST");
            if(t.album_artist.empty())t.album_artist=joined_tag(*info,"ALBUMARTIST");
            t.recording_id=single_tag(*info,"MUSICBRAINZ_TRACKID");
            if(t.recording_id.empty())t.recording_id=single_tag(*info,"MUSICBRAINZ_RECORDINGID");
            t.release_id=single_tag(*info,"MUSICBRAINZ_ALBUMID");t.release_group_id=single_tag(*info,"MUSICBRAINZ_RELEASEGROUPID");
            t.track_number=number_tag(*info,"TRACKNUMBER");t.disc_number=number_tag(*info,"DISCNUMBER");t.duration=info->get_length();
        }
        if(dynamic)t=merge_dynamic_track(snapshot_.track,std::move(t));
        pfc::string8 filename;
        if(filesystem::g_get_display_name_short(item->get_path(),filename)) {
            t.fallback_title=filename.c_str();auto dot=t.fallback_title.find_last_of('.');if(dot!=std::string::npos)t.fallback_title.resize(dot);
            const std::string path=item->get_path();
            if(!dynamic&&(path.starts_with("file://")||path.find("://")==std::string::npos))apply_filename_fallback(t,filename.c_str());
        }
        auto control=playback_control::get();double length=control->playback_get_length();
        if(std::isfinite(length)&&length>0)t.duration=length;
        if(!same_lookup_inputs(t,snapshot_.track)||t.identity!=snapshot_.track.identity)++snapshot_.clock_revision;
        snapshot_.track=std::move(t);
    }
    void refresh_track(const file_info* dynamic=nullptr) noexcept {
        try {
            auto control=playback_control::get();metadb_handle_ptr item;
            if(!control->get_now_playing(item)){snapshot_.active=false;publish();return;}
            read_track(item,dynamic);snapshot_.active=control->is_playing();snapshot_.paused=control->is_paused();
            snapshot_.position=control->playback_get_position();publish();
        }catch(...){snapshot_.active=false;publish();}
    }
    void publish() noexcept {
        try {snapshot_.epoch=epoch_now();snapshot_.sampled=MonoClock::now();if(service)service->publish(snapshot_);}catch(...){}
    }
    void on_playback_new_track(metadb_handle_ptr) noexcept override {++snapshot_.clock_revision;refresh_track();}
    void on_playback_stop(play_control::t_stop_reason) noexcept override {snapshot_.active=false;++snapshot_.clock_revision;publish();}
    void on_playback_seek(double position) noexcept override {snapshot_.position=position;++snapshot_.clock_revision;publish();}
    void on_playback_pause(bool paused) noexcept override {snapshot_.paused=paused;
        try{snapshot_.position=playback_control::get()->playback_get_position();}catch(...){}
        ++snapshot_.clock_revision;publish();}
    void on_playback_edited(metadb_handle_ptr) noexcept override {refresh_track();}
    void on_playback_dynamic_info_track(const file_info& info) noexcept override {++snapshot_.clock_revision;refresh_track(&info);}
    void on_playback_time(double position) noexcept override {snapshot_.position=position;publish();}
};
std::unique_ptr<PlaybackEvents> events;
}
void start_component() {
    if(core_api::is_quiet_mode_enabled())return;
    try {
        ServiceDependencies dependencies;
        dependencies.diagnostics=Diagnostics(
            [](const std::string& line){if(core_api::are_services_available())console::print(line.c_str());},
            [](Diagnostics::Task task){fb2k::inMainThread(std::move(task));});
        service=std::make_unique<Service>(select_cache_folder(),read_settings(),std::move(dependencies));
        events=std::make_unique<PlaybackEvents>();
    }catch(const std::exception&){events.reset();service.reset();console::print("discord-foobar-rpc: initialization failed; playback remains available.");}
}
void stop_component() noexcept {events.reset();service.reset();}
void settings_changed(){if(events)events->refresh_settings();}
std::string component_status(){return service?service->status():"Component inactive";}
}
