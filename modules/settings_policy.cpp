#include "settings_policy.h"
#include <stdexcept>
namespace music_tracker {
void apply_settings(SettingsStore& store,const Settings& settings,const std::function<void()>& changed) {
    if(!valid_application_id(settings.custom_application_id))throw std::invalid_argument("Custom application ID must be numeric, or blank for the built-in application.");
    store.write(settings);
    if(changed)changed();
}
std::filesystem::path choose_cache_folder(const std::filesystem::path& profile,const std::filesystem::path& fallback,const DirectoryProbe& writable) {
    const auto safe_probe=[&](const auto& folder){try{return !folder.empty()&&writable(folder);}catch(...){return false;}};
    if(safe_probe(profile))return profile;
    if(safe_probe(fallback))return fallback;
    return {};
}
}
