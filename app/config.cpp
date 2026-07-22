#include "config.hpp"

#include <borealis.hpp>

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "appdata.hpp"

namespace config
{
namespace
{

constexpr const char* kPath = APPDATA_DIR "/config.json";

Config cfg;

// The file is ours: a flat object of bools and one string, written by save()
// below. That is small enough that finding "key": <value> beats pulling a JSON
// parser into the build (same reasoning as app/stremio.cpp).
bool readBool(const std::string& body, const char* key, bool dflt)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k        = body.find(pat);
    if (k == std::string::npos) return dflt;
    size_t colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return dflt;
    size_t v = colon + 1;
    while (v < body.size() && (body[v] == ' ' || body[v] == '\t')) v++;
    if (body.compare(v, 4, "true") == 0) return true;
    if (body.compare(v, 5, "false") == 0) return false;
    return dflt;
}

std::string readStr(const std::string& body, const char* key,
                    const std::string& dflt)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k        = body.find(pat);
    if (k == std::string::npos) return dflt;
    size_t q1 = body.find('"', body.find(':', k + pat.size()));
    if (q1 == std::string::npos) return dflt;
    size_t q2 = body.find('"', q1 + 1);
    if (q2 == std::string::npos) return dflt;
    return body.substr(q1 + 1, q2 - q1 - 1);
}

// The languages offered, and what each maps to for mpv. Both 639-2 spellings
// are listed because a track tag can carry either: "fre" (bibliographic) or
// "fra" (terminological) for French, and mpv matches the tag as written.
struct Lang
{
    const char* code;   // what we store
    const char* label;  // what Options shows
    const char* mpv;    // what mpv is given
};

const Lang kLangs[] = {
    { "auto", "Console language", "" },  // resolved via consoleLang()
    { "en", "English", "en,eng" },
    { "fr", "French", "fr,fre,fra" },
    { "es", "Spanish", "es,spa" },
    { "de", "German", "de,ger,deu" },
    { "it", "Italian", "it,ita" },
    { "pt", "Portuguese", "pt,por" },
    { "nl", "Dutch", "nl,dut,nld" },
    { "ru", "Russian", "ru,rus" },
    { "ja", "Japanese", "ja,jpn" },
    { "ko", "Korean", "ko,kor" },
    { "zh", "Chinese", "zh,chi,zho" },
};

} // namespace

Config& get() { return cfg; }

std::string consoleLang()
{
    // setGetSystemLanguage packs the code as chars in a u64 ("fr", "en-US"),
    // NOT as a SetLanguage enum -- the first two bytes are the ISO-639-1 part,
    // which is all we need. borealis already calls setInitialize().
    u64 code = 0;
    if (R_FAILED(setGetSystemLanguage(&code))) return "en";
    char buf[9] = { 0 };
    std::memcpy(buf, &code, 8);
    if (!buf[0] || !buf[1]) return "en";
    return std::string(buf, 2);
}

std::string mpvLangList(const std::string& code)
{
    std::string c = code == "auto" ? consoleLang() : code;
    for (const auto& l : kLangs)
        if (c == l.code) return l.mpv;
    // A console language we do not have a row for: hand mpv the bare code, it
    // still matches tracks tagged with the 639-1 form.
    return c.empty() ? "" : c;
}

const std::vector<std::string>& langCodes()
{
    static std::vector<std::string> v = [] {
        std::vector<std::string> o;
        for (const auto& l : kLangs) o.push_back(l.code);
        return o;
    }();
    return v;
}

const std::vector<std::string>& langLabels()
{
    static std::vector<std::string> v = [] {
        std::vector<std::string> o;
        for (const auto& l : kLangs) o.push_back(l.label);
        return o;
    }();
    return v;
}

void load()
{
    FILE* f = std::fopen(kPath, "rb");
    if (!f)
    {
        brls::Logger::info("[config] no config.json, using defaults");
        return;
    }

    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    std::fclose(f);

    cfg.startupTab = readStr(body, "startupTab", "local") == "stremio"
                         ? Tab::STREMIO
                         : Tab::LOCAL;
    cfg.logging = readBool(body, "logging", cfg.logging);
    cfg.hide4k  = readBool(body, "hide4k", cfg.hide4k);
    cfg.rateGovernor = readBool(body, "rateGovernor", cfg.rateGovernor);
    cfg.ramStream    = readBool(body, "ramStream", cfg.ramStream);
    cfg.checkUpdates = readBool(body, "checkUpdates", cfg.checkUpdates);
    cfg.audioLang    = readStr(body, "audioLang", cfg.audioLang);
    cfg.subLang      = readStr(body, "subLang", cfg.subLang);
    cfg.subtitles    = readBool(body, "subtitles", cfg.subtitles);

    brls::Logger::info(
        "[config] startupTab={} logging={} hide4k={} checkUpdates={}",
        cfg.startupTab == Tab::STREMIO ? "stremio" : "local", cfg.logging,
        cfg.hide4k, cfg.checkUpdates);
}

bool save()
{
    FILE* f = std::fopen(kPath, "wb");
    if (!f)
    {
        brls::Logger::warning("[config] cannot write {}", kPath);
        return false;
    }
    std::fprintf(f,
                 "{\n"
                 "  \"startupTab\": \"%s\",\n"
                 "  \"logging\": %s,\n"
                 "  \"hide4k\": %s,\n"
                 "  \"rateGovernor\": %s,\n"
                 "  \"ramStream\": %s,\n"
                 "  \"checkUpdates\": %s,\n"
                 "  \"audioLang\": \"%s\",\n"
                 "  \"subLang\": \"%s\",\n"
                 "  \"subtitles\": %s\n"
                 "}\n",
                 cfg.startupTab == Tab::STREMIO ? "stremio" : "local",
                 cfg.logging ? "true" : "false", cfg.hide4k ? "true" : "false",
                 cfg.rateGovernor ? "true" : "false",
                 cfg.ramStream ? "true" : "false",
                 cfg.checkUpdates ? "true" : "false", cfg.audioLang.c_str(),
                 cfg.subLang.c_str(), cfg.subtitles ? "true" : "false");
    std::fclose(f);
    return true;
}

} // namespace config
