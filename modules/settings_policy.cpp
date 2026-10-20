#include "settings_policy.h"
#include <stdexcept>
namespace music_tracker {
void apply_settings(SettingsStore& store,const Settings& settings,const std::function<void()>& changed) {
    if(!valid_application_id(settings.custom_application_id))throw std::invalid_argument("Custom application ID must be numeric, or blank for the built-in application.");
    store.write(settings);
    if(changed)changed();
}
std::filesystem::path choose_cache_folder(const std::filesystem::path& application_data,const DirectoryProbe& writable) {
    if(application_data.empty())return {};
    auto folder=application_data/"discord-foobar-rpc";
    try {if(writable(folder))return folder;}catch(...){}
    return {};
}
}
