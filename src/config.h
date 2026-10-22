// Settings, kept in %APPDATA%\disky\config.json. The token is encrypted
// with DPAPI, so only this Windows user can read it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace disky {

struct Config {
    std::string token;
    // The web app's build number, and the day (Unix days) it was read.
    int buildNumber = 0;
    int64_t buildDay = 0;
    // Where the window was: x, y, width, height; width 0 for the default.
    int x = 0, y = 0, w = 0, h = 0;
    bool maximised = false;
    // The guild and channel last open; guild 0 for DMs.
    uint64_t guild = 0, channel = 0;
    bool notify = true;
    bool members = true;  // the member list is shown
    float volume = 0.5f;  // for videos
    std::vector<std::string> recentEmoji;  // newest first, as typed in a message
};

Config LoadConfig();
bool SaveConfig(const Config& c);

bool StartsWithWindows();
void SetStartWithWindows(bool on);

}  // namespace disky
