#pragma once
#include "model.h"
#include <filesystem>
#include <functional>
namespace music_tracker {
class SettingsStore {
public:
    virtual ~SettingsStore() = default;
    virtual Settings read() = 0;
    virtual void write(const Settings& settings) = 0;
};
void apply_settings(SettingsStore& store,const Settings& settings,const std::function<void()>& changed);
using DirectoryProbe=std::function<bool(const std::filesystem::path&)>;
std::filesystem::path choose_cache_folder(const std::filesystem::path& application_data,const DirectoryProbe& writable);
}
