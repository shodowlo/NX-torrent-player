#pragma once

#include <string>
#include <vector>

// User settings, persisted to sdmc:/switch/nx-torrent-player/config.json and
// opened with X from the browser. Everything here has a default that matches
// what the app did before it was configurable, so a missing (or corrupt) file
// is not an error -- it just means "defaults".
namespace config
{

enum class Tab
{
    LOCAL   = 0,
    STREMIO = 1,
};

struct Config
{
    // Which category the browser opens on.
    Tab startupTab = Tab::LOCAL;

    // Writes the log to APPDATA_LOG. Off by default: it is unbuffered and the
    // engine dumps a [stats] line every 2s, so it writes to the SD card
    // continuously for the whole session. Worth it when diagnosing, not
    // otherwise.
    bool logging = false;

    // Preferred track languages, as ISO-639-1 ("fr") or "auto" -- which means
    // the console's own language, so the app matches the system out of the box.
    std::string audioLang = "auto";
    std::string subLang   = "auto";

    // Show subtitles at all. Off means mpv loads none rather than picking one.
    bool subtitles = true;

    // Ask GitHub for a newer release at startup. On by default, but it is a
    // network call the user did not ask for -- offline or on a metered
    // connection, being able to turn it off matters.
    bool checkUpdates = true;

    // Hides 4K sources in the Stremio source list. On by default: they are the
    // heaviest streams in the swarm and the Switch outputs 1080p docked, so
    // they cost bandwidth we cannot show.
    bool hide4k = true;

    // Download-rate limiter (torrentfs_set_governor): once the playback buffer
    // is comfortably ahead, cap downloading to a backlog-tied rate instead of
    // bursting at wifi line rate — those bursts saturate the OS network core and
    // can stutter the console. Off by default: it trades download speed for
    // network calm, and it never limits anything while the buffer is under 10 s
    // anyway. Never touches streams that struggle to keep up.
    bool rateGovernor = false;

    // RAM streaming (torrentfs_set_ram_stream): keep verified pieces in a
    // bounded RAM window instead of writing them to the SD card. On by default
    // -- it removes the per-piece playback stutter (the SD write of a finished
    // piece hammers the filesystem core, the more so the bigger the piece). The
    // trade is that nothing is persisted and seeking far back re-downloads, and
    // it needs a full-RAM launch. Applies to the next video.
    bool ramStream = true;
};

// The live settings. Mutate, then call save().
Config& get();

// The console's language as ISO-639-1 ("fr"), or "en" if it cannot be read.
std::string consoleLang();

// An ISO-639-1 code (or "auto") turned into what mpv's alang/slang want: a
// comma-separated list, because a track's language tag can be 639-1 ("fr") or
// either 639-2 form ("fre" bibliographic / "fra" terminological) depending on
// who muxed the file. "" when there is nothing to prefer.
std::string mpvLangList(const std::string& code);

// The languages the Options screen offers, "auto" first. Codes and labels are
// index-matched.
const std::vector<std::string>& langCodes();
const std::vector<std::string>& langLabels();

// Reads config.json. Called once at startup, before the UI is built. Missing
// file / unreadable keys keep their default.
void load();

// Writes config.json. False if the SD card refused it.
bool save();

} // namespace config
