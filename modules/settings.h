#pragma once
#include "model.h"
#include <filesystem>
namespace music_tracker {
Settings read_settings(); // All foobar SDK/config access stays on the main thread.
void write_settings(const Settings& settings);
std::filesystem::path select_cache_folder();
void settings_changed();
std::string component_status();
bool retry_current_track();
}
