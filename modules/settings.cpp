#include <foobar2000/SDK/foobar2000.h>
#include "settings.h"
#include "settings_policy.h"
#include <fstream>
#include <cstdlib>
#ifdef _WIN32
#include <shlobj.h>
#endif
namespace music_tracker {
namespace {
const GUID enabled_guid={0x46c37cd9,0x5bba,0x4ea1,{0x91,0x26,0x00,0xad,0x3a,0x56,0x04,0x60}};
const GUID app_guid={0x0a17ecc1,0xf916,0x42e2,{0x84,0x7b,0x97,0x31,0x62,0x30,0x74,0x9b}};
cfg_bool enabled(enabled_guid,true);
cfg_string custom_id(app_guid,"");
bool writable(const std::filesystem::path& folder) {
    if(folder.empty())return false;
    try {
        std::filesystem::create_directories(folder);
        auto probe=folder/("write-test-"+std::to_string(unix_now())+".tmp");
        {std::ofstream stream(probe,std::ios::binary);if(!stream)return false;stream<<"test";stream.flush();if(!stream)return false;}
        std::filesystem::remove(probe);return true;
    }catch(...){return false;}
}
}
namespace {
class FoobarSettingsStore final : public SettingsStore {
public:
    Settings read() override {pfc::string8 id;custom_id.get(id);return {static_cast<bool>(enabled),id.c_str()};}
    void write(const Settings& settings) override {enabled=settings.enabled;custom_id=settings.custom_application_id.c_str();}
};
}
Settings read_settings(){FoobarSettingsStore store;return store.read();}
void write_settings(const Settings& settings) {FoobarSettingsStore store;apply_settings(store,settings,settings_changed);}
std::filesystem::path select_cache_folder() {
    // Authoritative host profile includes portable profiles and custom foobar profile locations.
    // Never write into a DLL folder or signed macOS .component bundle merely because it is nearby.
    std::filesystem::path profile;
    try {
        pfc::string8 native;
        if(filesystem::g_get_native_path(core_api::get_profile_path(),native)) {
            profile=std::filesystem::u8path(native.c_str())/"discord-foobar-rpc"/"metadata-cache";

        }
    }catch(...){}
    std::filesystem::path fallback;
#ifdef _WIN32
    PWSTR appdata=nullptr;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,KF_FLAG_DEFAULT,nullptr,&appdata))) {
        fallback=std::filesystem::path(appdata)/"discord-foobar-rpc"/"metadata-cache";CoTaskMemFree(appdata);
    }
#elif defined(__APPLE__)
    if(const char* home=std::getenv("HOME"))fallback=std::filesystem::u8path(home)/"Library"/"Application Support"/"discord-foobar-rpc"/"metadata-cache";
#endif
    return choose_cache_folder(profile,fallback,writable); // Memory-only behavior: metadata/presence continue even when both locations are unwritable.
}
}
