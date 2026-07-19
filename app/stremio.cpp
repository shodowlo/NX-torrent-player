#include "stremio.hpp"

#include <webp/decode.h>

// Metahub serves some posters as WebP and ignores an Accept header asking for
// anything else. nanovg decodes through stb_image, which has no WebP support --
// so those posters silently never appeared. Decode them here and re-encode to
// PNG (still compressed, and stb_image reads it back).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

// Header only: the implementation is already compiled into nanovg (which uses
// it to decode every image the UI shows), so this just links against it.
#include <stb_image.h>

#include <borealis/views/button.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "appdata.hpp"
#include "browse.hpp"
#include "http.hpp"
#include "json.hpp"

namespace
{

constexpr const char* kLoginUrl   = "https://api.strem.io/api/login";
constexpr const char* kLibraryUrl = "https://api.strem.io/api/datastoreGet";
constexpr const char* kKeyPath    = APPDATA_DIR "/stremio.authkey";
// Kept next to the key purely so Options can say which account is signed in --
// the API never needs it again once we hold an authKey.
constexpr const char* kEmailPath  = APPDATA_DIR "/stremio.email";







// One-liner for a dialog: the API and curl can both hand back long, multi-line
// text, which blew the dialog up and wrecked the layout behind it.
std::string oneLine(std::string s, size_t max = 120)
{
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    if (s.size() > max) s = s.substr(0, max - 1) + "…";
    return s;
}

// Converts a WebP payload to PNG bytes. Returns empty if `in` isn't WebP or
// can't be decoded. PNG keeps the cache compressed -- a raw RGBA dump of a
// poster is ~240 KB against ~18 KB here.
std::string webpToPng(const std::string& in)
{
    int w = 0, h = 0;
    uint8_t* rgba = WebPDecodeRGBA((const uint8_t*)in.data(), in.size(), &w, &h);
    if (!rgba) return "";

    std::string png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            ((std::string*)ctx)->append((const char*)data, size);
        },
        &png, w, h, 4, rgba, w * 4);
    WebPFree(rgba);
    return png;
}

// Hiding a Box is not enough to take its controls out of the focus ring:
// View::isFocusable() checks the *view's own* visibility, but
// Box::getDefaultFocus() recurses into children without checking the box's. So
// the buttons of a GONE box stay focusable, and since a GONE parent gets no
// layout, focus lands on a view sitting invisibly at 0,0 -- which is exactly
// what happens when the sign-in form is hidden behind the library.
// True if `v` is `ancestor` or sits under it.
bool isUnder(brls::View* v, brls::View* ancestor)
{
    for (; v; v = v->getParent())
        if (v == ancestor) return true;
    return false;
}

void setSubtreeFocusable(brls::View* v, bool focusable)
{
    v->setFocusable(focusable);
    if (auto* box = dynamic_cast<brls::Box*>(v))
        for (brls::View* child : box->getChildren())
            setSubtreeFocusable(child, focusable);
}

void dialog(const std::string& msg)
{
    auto* d = new brls::Dialog(oneLine(msg));
    d->addButton("OK", []() {});
    d->open();
}

// The raw text of the nested object at `key` ("state":{...}), braces included,
// or "". The json helpers scan flat across whatever they're given, so fields
// that exist both in the item and in a sub-object MUST be read from the
// sub-object's own text -- scanning the whole item picks whichever comes first.
std::string subObject(const std::string& o, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k        = o.find(pat);
    if (k == std::string::npos) return "";
    size_t open = o.find('{', k + pat.size());
    if (open == std::string::npos) return "";
    int depth  = 0;
    bool inStr = false;
    for (size_t i = open; i < o.size(); i++)
    {
        char c = o[i];
        if (inStr)
        {
            if (c == '\\') i++;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '{') depth++;
        else if (c == '}' && --depth == 0)
            return o.substr(open, i - open + 1);
    }
    return "";
}





} // namespace

namespace stremio
{

// Bumped every time we push newer watch progress to the account. Views remember
// the value they last rendered and reload when it moves -- a generation counter
// rather than a single-shot flag, so every level of a nested navigation (the
// library tab AND the episode list under it) can each refresh once instead of
// racing to consume one flag.
static uint32_t g_libraryGen = 0;

uint32_t libraryGen() { return g_libraryGen; }

void markLibraryStale() { g_libraryGen++; }

int64_t posterCacheBytes()
{
    int64_t total = 0;
    if (DIR* d = opendir(APPDATA_POSTERS))
    {
        struct dirent* e;
        while ((e = readdir(d)))
        {
            std::string p = std::string(APPDATA_POSTERS) + "/" + e->d_name;
            struct stat st;
            if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                total += st.st_size;
        }
        closedir(d);
    }
    return total;
}

void clearPosterCache()
{
    if (DIR* d = opendir(APPDATA_POSTERS))
    {
        struct dirent* e;
        while ((e = readdir(d)))
        {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            std::remove((std::string(APPDATA_POSTERS) + "/" + name).c_str());
        }
        closedir(d);
    }
}

// The last position we reported, kept so the episode/season lists can show fresh
// progress without another round-trip: Stremio tracks one position per show, and
// we just set it, so this IS the current truth for that item.
static LocalWatch g_lastWatch;

LocalWatch lastWatch() { return g_lastWatch; }

bool saveAuthKey(const std::string& key)
{
    FILE* f = std::fopen(kKeyPath, "w");
    if (!f) return false;
    std::fwrite(key.data(), 1, key.size(), f);
    std::fclose(f);
    return true;
}

std::string loadAuthKey()
{
    FILE* f = std::fopen(kKeyPath, "r");
    if (!f) return "";
    char buf[512] = { 0 };
    size_t n      = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

void clearAuthKey()
{
    std::remove(kKeyPath);
    std::remove(kEmailPath);
}

bool saveEmail(const std::string& email)
{
    FILE* f = std::fopen(kEmailPath, "w");
    if (!f) return false;
    std::fwrite(email.data(), 1, email.size(), f);
    std::fclose(f);
    return true;
}

std::string loadEmail()
{
    FILE* f = std::fopen(kEmailPath, "r");
    if (!f) return "";
    char buf[256] = { 0 };
    size_t n      = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    std::string s(buf, n);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// UI thread only.
static brls::View* libraryUpTarget = nullptr;

void setLibraryUpTarget(brls::View* target) { libraryUpTarget = target; }

// A blurred copy of a cached poster, for use as a full-screen background.
// Returns its path, or "" if the source could not be read.
//
// Made once and cached next to the source. The first version of this just
// shrank the poster to 24px and let the GPU stretch it back: averaging blocks
// of pixels IS a box filter, but a 50x bilinear upscale of it shows its own
// blocky structure -- the "blur" looked pixelated. So blur at a resolution the
// upscale can't expose: shrink to kBlurWidth, then run a real box blur over it,
// which leaves no high frequencies for the upscale to reveal.
std::string blurredPosterPath(const std::string& posterPath)
{
    if (posterPath.empty()) return "";
    // Deliberately not ".bg.png": that name belongs to the 24px version above,
    // and a cache hit never revalidates, so reusing it would keep serving the
    // pixelated one from everybody's SD card.
    std::string out = posterPath + ".blur.png";

    // Posters never change, so a hit is final.
    if (FILE* f = std::fopen(out.c_str(), "rb"))
    {
        std::fclose(f);
        return out;
    }

    int w = 0, h = 0, comp = 0;
    uint8_t* px = stbi_load(posterPath.c_str(), &w, &h, &comp, 3);
    if (!px || w <= 0 || h <= 0)
    {
        if (px) stbi_image_free(px);
        brls::Logger::warning("[stremio] blur: cannot decode {}", posterPath);
        return "";
    }

    // Enough resolution that stretching it to 1280 stays smooth, small enough
    // that the blur below is a handful of milliseconds.
    const int bw = 256;
    int bh       = (int)((int64_t)h * bw / w);
    if (bh < 1) bh = 1;

    // Box-average down to bw x bh (a plain nearest pick would alias).
    std::vector<uint8_t> img((size_t)bw * bh * 3);
    for (int y = 0; y < bh; y++)
    {
        int y0 = (int)((int64_t)y * h / bh), y1 = (int)((int64_t)(y + 1) * h / bh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < bw; x++)
        {
            int x0 = (int)((int64_t)x * w / bw), x1 = (int)((int64_t)(x + 1) * w / bw);
            if (x1 <= x0) x1 = x0 + 1;
            int acc[3] = { 0, 0, 0 }, n = 0;
            for (int yy = y0; yy < y1 && yy < h; yy++)
                for (int xx = x0; xx < x1 && xx < w; xx++)
                {
                    const uint8_t* p = px + ((size_t)yy * w + xx) * 3;
                    acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
                    n++;
                }
            uint8_t* d = &img[((size_t)y * bw + x) * 3];
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)(acc[c] / (n ? n : 1));
        }
    }
    stbi_image_free(px);

    // Separable box blur, three passes: box^3 is close enough to a gaussian
    // that nothing of the poster survives but its colours. Edges clamp, so the
    // border does not darken.
    const int radius = 14;
    std::vector<uint8_t> tmp(img.size());
    auto blurAxis = [&](std::vector<uint8_t>& src, std::vector<uint8_t>& dst,
                        int lineCount, int lineLen, int stepInLine,
                        int stepBetweenLines) {
        for (int l = 0; l < lineCount; l++)
        {
            const size_t base = (size_t)l * stepBetweenLines;
            for (int c = 0; c < 3; c++)
            {
                int sum = 0;
                auto at = [&](int i) -> uint8_t& {
                    return src[base + (size_t)i * stepInLine + c];
                };
                // Prime the running sum with the first window, clamped.
                for (int i = -radius; i <= radius; i++)
                    sum += at(i < 0 ? 0 : (i >= lineLen ? lineLen - 1 : i));
                const int win = radius * 2 + 1;
                for (int i = 0; i < lineLen; i++)
                {
                    dst[base + (size_t)i * stepInLine + c] = (uint8_t)(sum / win);
                    int out = i - radius, in = i + radius + 1;
                    sum -= at(out < 0 ? 0 : out);
                    sum += at(in >= lineLen ? lineLen - 1 : in);
                }
            }
        }
    };
    for (int pass = 0; pass < 3; pass++)
    {
        blurAxis(img, tmp, bh, bw, 3, (size_t)bw * 3);          // horizontal
        blurAxis(tmp, img, bw, bh, (size_t)bw * 3, 3);          // vertical
    }

    std::string png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            ((std::string*)ctx)->append((const char*)data, size);
        },
        &png, bw, bh, 3, img.data(), bw * 3);
    if (png.empty()) return "";

    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) return "";
    bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    if (!ok) { std::remove(out.c_str()); return ""; }

    brls::Logger::info("[stremio] blur {} ({}x{}) -> {}x{}", posterPath, w, h,
                       bw, bh);
    return out;
}

void loginAsync(const std::string& email, const std::string& password,
                std::function<void(LoginResult)> done)
{
    brls::async([email, password, done]() {
        LoginResult r;
        std::string body = "{\"type\":\"Login\",\"email\":\"" + json::escape(email) +
                           "\",\"password\":\"" + json::escape(password) +
                           "\",\"facebook\":false}";
        std::string resp, err;

        if (!http::postJson(kLoginUrl, body, resp, err))
        {
            r.error = err;
        }
        else
        {
            // Stremio answers 200 even for a bad password, with the reason in
            // "error", so the status code alone can't be trusted here.
            std::string key = json::str(resp, "authKey");
            if (!key.empty())
            {
                r.ok      = true;
                r.authKey = key;
            }
            else
            {
                std::string msg = json::str(resp, "message");
                r.error = msg.empty() ? "Wrong email or password" : msg;
            }
        }
        brls::sync([done, r]() { done(r); });
    });
}

void fetchLibraryAsync(const std::string& authKey,
                       std::function<void(LibraryResult)> done)
{
    brls::async([authKey, done]() {
        LibraryResult r;
        std::string body = "{\"authKey\":\"" + json::escape(authKey) +
                           "\",\"collection\":\"libraryItem\",\"all\":true}";
        std::string resp, err;

        if (!http::postJson(kLibraryUrl, body, resp, err))
        {
            r.error = err;
        }
        else
        {
            std::string msg = json::str(resp, "message");
            auto objs       = json::objects(resp, "result");
            if (objs.empty() && !msg.empty())
            {
                r.error = msg;
            }
            else
            {
                r.ok = true;
                int nRemoved = 0, nNoName = 0, nTemp = 0;
                for (const auto& o : objs)
                {
                    // Stremio keeps deleted entries in the collection flagged
                    // removed=true, and auto-adds anything you watch as
                    // temp=true (that's "Continue Watching", not a real library
                    // add). Both are worth counting: they explain a short list.
                    if (json::boolean(o, "removed")) { nRemoved++; continue; }
                    if (json::boolean(o, "temp")) nTemp++;

                    LibItem it;
                    it.id     = json::str(o, "_id");
                    it.name   = json::str(o, "name");
                    it.type   = json::str(o, "type");
                    it.poster = json::str(o, "poster");
                    // Watch state: read from the "state" object's own text, not
                    // the whole item -- the flat json scan takes the first key
                    // it finds, and an item field with the same name landing
                    // first skewed the progress ratio.
                    std::string st = subObject(o, "state");
                    if (!st.empty())
                    {
                        it.videoId      = json::str(st, "video_id");
                        it.timeOffsetMs = (double)json::integer(st, "timeOffset", 0);
                        it.durationMs   = (double)json::integer(st, "duration", 0);
                        if (it.timeOffsetMs > 0)
                            brls::Logger::info(
                                "[stremio] state {}: video={} off={}s dur={}s "
                                "-> {}%",
                                it.id, it.videoId,
                                (long)(it.timeOffsetMs / 1000),
                                (long)(it.durationMs / 1000),
                                it.durationMs > 0
                                    ? (int)(it.timeOffsetMs * 100 / it.durationMs)
                                    : -1);
                    }
                    if (it.name.empty()) { nNoName++; continue; }
                    r.items.push_back(it);
                }
                // Whether a short list is the account's doing or ours is not
                // guessable from the UI, so say it plainly.
                brls::Logger::info(
                    "[stremio] library: {} bytes, {} objects parsed -> {} shown "
                    "({} removed, {} unnamed, {} temp/continue-watching)",
                    resp.size(), objs.size(), r.items.size(), nRemoved, nNoName,
                    nTemp);
            }
        }
        brls::sync([done, r]() { done(r); });
    });
}

namespace
{

// Locates the value of `"key":` in a raw JSON object. Returns [vs, ve) covering
// the value text (quotes included for strings), or false if the key is absent.
bool valueSpan(const std::string& obj, const char* key, size_t& vs, size_t& ve)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k        = obj.find(pat);
    if (k == std::string::npos) return false;
    size_t colon = obj.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    size_t v = colon + 1;
    while (v < obj.size() && (obj[v] == ' ' || obj[v] == '\t')) v++;
    if (v >= obj.size()) return false;
    if (obj[v] == '"')
    {
        size_t e = v + 1;
        while (e < obj.size() && obj[e] != '"')
            e += (obj[e] == '\\') ? 2 : 1;
        if (e >= obj.size()) return false;
        vs = v;
        ve = e + 1;
        return true;
    }
    size_t e = v;
    while (e < obj.size() && obj[e] != ',' && obj[e] != '}' && obj[e] != ']') e++;
    vs = v;
    ve = e;
    return true;
}

// Sets `key` to `val` (val must already be JSON-encoded) in a raw object,
// replacing the existing value or inserting the field right after the object's
// opening brace when it is missing. False if neither worked.
bool setField(std::string& obj, const char* key, const std::string& val)
{
    size_t vs = 0, ve = 0;
    if (valueSpan(obj, key, vs, ve))
    {
        obj.replace(vs, ve - vs, val);
        return true;
    }
    size_t brace = obj.find('{');
    if (brace == std::string::npos) return false;
    obj.insert(brace + 1, std::string("\"") + key + "\":" + val + ",");
    return true;
}

} // namespace

// Rewrites the library item's watch state on the API. The item is fetched
// back first and edited in place (string surgery on the raw object), because
// datastorePut REPLACES the stored item: sending a rebuilt subset would strip
// whatever fields this client doesn't know about.
void pushWatchStateAsync(const std::string& authKey, const std::string& itemId,
                         const std::string& videoId, double posSec, double durSec)
{
    if (authKey.empty() || itemId.empty() || posSec <= 0) return;

    brls::async([authKey, itemId, videoId, posSec, durSec]() {
        std::string body = "{\"authKey\":\"" + json::escape(authKey) +
                           "\",\"collection\":\"libraryItem\",\"ids\":[\"" +
                           json::escape(itemId) + "\"]}";
        std::string resp, err;
        if (!http::postJson(kLibraryUrl, body, resp, err))
        {
            brls::Logger::warning("[stremio] watch-state get failed: {}", err);
            return;
        }
        auto objs = json::objects(resp, "result");
        if (objs.empty())
        {
            brls::Logger::warning("[stremio] watch-state: item {} not found",
                                  itemId);
            return;
        }
        std::string obj = objs[0];

        // Edit the "state" object's own text, then splice it back: replacing
        // fields across the whole item risks hitting a same-named field
        // outside state (the exact bug the library parse had).
        std::string pat = "\"state\"";
        size_t sk       = obj.find(pat);
        size_t sopen    = sk == std::string::npos ? std::string::npos
                                                  : obj.find('{', sk + pat.size());
        size_t sclose   = std::string::npos;
        if (sopen != std::string::npos)
        {
            int depth  = 0;
            bool inStr = false;
            for (size_t i = sopen; i < obj.size(); i++)
            {
                char c = obj[i];
                if (inStr)
                {
                    if (c == '\\') i++;
                    else if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') inStr = true;
                else if (c == '{') depth++;
                else if (c == '}' && --depth == 0) { sclose = i; break; }
            }
        }
        if (sclose == std::string::npos)
        {
            brls::Logger::warning("[stremio] watch-state: item {} has no state",
                                  itemId);
            return;
        }
        std::string st = obj.substr(sopen, sclose - sopen + 1);

        char num[32];
        long long ms = (long long)(posSec * 1000.0);
        std::snprintf(num, sizeof(num), "%lld", ms < 0 ? 0 : ms);
        bool ok = setField(st, "timeOffset", num);
        if (durSec > 0)
        {
            std::snprintf(num, sizeof(num), "%lld", (long long)(durSec * 1000.0));
            ok = setField(st, "duration", num) && ok;
        }
        if (!videoId.empty())
            ok = setField(st, "video_id",
                          "\"" + json::escape(videoId) + "\"") && ok;

        std::time_t tt = std::time(nullptr);
        std::tm g {};
        gmtime_r(&tt, &g);
        char iso[40];
        std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S.000Z", &g);
        std::string isoq = "\"" + std::string(iso) + "\"";
        setField(st, "lastWatched", isoq);

        obj.replace(sopen, sclose - sopen + 1, st);
        setField(obj, "_mtime", isoq);  // top-level field, edited on the item

        if (!ok)
        {
            brls::Logger::warning(
                "[stremio] watch-state: item {} state fields not updatable",
                itemId);
            return;
        }

        std::string put = "{\"authKey\":\"" + json::escape(authKey) +
                          "\",\"collection\":\"libraryItem\",\"changes\":[" +
                          obj + "]}";
        std::string resp2;
        if (!http::postJson("https://api.strem.io/api/datastorePut", put, resp2,
                            err))
            brls::Logger::warning("[stremio] watch-state put failed: {}", err);
        else
        {
            // Stremio now has newer progress than the library we loaded once at
            // sign-in. Bump the generation so the tab (and any episode list)
            // reloads when the user returns, and record the position so those
            // lists can show it without another fetch.
            g_lastWatch  = { itemId, videoId, posSec * 1000.0, durSec * 1000.0 };
            g_libraryGen++;
            brls::Logger::info("[stremio] watch-state {} @{}s/{}s pushed",
                               videoId.empty() ? itemId : videoId, (int)posSec,
                               (int)durSec);
        }
    });
}

// Where the poster for `id` lives on disk. Keyed on the item id, sanitised:
// ids look like "tt1234567" but series episodes carry ':' separators, which are
// not legal on FAT32.
std::string posterCachePath(const std::string& id)
{
    std::string safe;
    for (char c : id)
        safe += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    return std::string(APPDATA_POSTERS) + "/" + safe + ".jpg";
}

std::string cachedPosterPath(const std::string& id)
{
    if (id.empty()) return "";
    std::string path = posterCachePath(id);
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        std::fclose(f);
        return path;
    }
    return "";
}

// Downloads one image into `path` and calls back on the UI thread with it ("" on
// failure). Shared by the list thumbnails and the full-size background: both
// have to survive a host that answers WebP, or an HTML error page.
static void downloadImageAsync(const std::string& id, const std::string& url,
                        const std::string& path,
                        std::function<void(std::string)> done)
{
    brls::async([id, url, path, done]() {
        std::string body, err;
        // Ask for a format stb_image can actually decode.
        static const char* kAcceptImg = "Accept: image/jpeg,image/png;q=0.9";
        bool ok = http::get(url, body, err, kAcceptImg);

        // A poster host answering with an HTML error page would be cached as a
        // corrupt file forever (a hit never revalidates), so check the magic
        // bytes rather than trusting the response.
        auto b = [&](size_t i) {
            return i < body.size() ? (unsigned char)body[i] : 0u;
        };
        bool isJpeg = b(0) == 0xFF && b(1) == 0xD8;
        bool isPng  = b(0) == 0x89 && b(1) == 'P' && b(2) == 'N' && b(3) == 'G';
        bool isGif  = b(0) == 'G' && b(1) == 'I' && b(2) == 'F';
        bool isBmp  = b(0) == 'B' && b(1) == 'M';
        bool isWebp = b(0) == 'R' && b(1) == 'I' && b(2) == 'F' && b(3) == 'F' &&
                      body.size() > 12 && body.compare(8, 4, "WEBP") == 0;

        // Metahub serves WebP for some titles and ignores Accept, so asking for
        // JPEG doesn't help and there is nowhere else to fall back to: decode it
        // and store PNG instead.
        if (ok && isWebp)
        {
            std::string png = webpToPng(body);
            if (png.empty())
            {
                brls::Logger::warning("[stremio] poster {}: WebP decode failed", id);
                ok = false;
            }
            else
            {
                brls::Logger::info("[stremio] poster {}: WebP {} B -> PNG {} B",
                                   id, body.size(), png.size());
                body  = std::move(png);
                isPng = true;
            }
        }

        bool isImage = isJpeg || isPng || isGif || isBmp;

        if (ok && !isImage)
            brls::Logger::warning(
                "[stremio] poster {}: {} B, magic {:02X} {:02X} {:02X} {:02X} "
                "-- url {}",
                id, body.size(), b(0), b(1), b(2), b(3), url);

        if (ok && isImage)
        {
            if (FILE* f = std::fopen(path.c_str(), "wb"))
            {
                std::fwrite(body.data(), 1, body.size(), f);
                std::fclose(f);
            }
            else
                ok = false;
        }
        else if (ok)
        {
            ok = false;  // already reported above, with the magic bytes
        }

        std::string result = ok ? path : std::string();
        brls::sync([done, result]() { done(result); });
    });
}

// The artwork URL for `id`, or "" if there is none to be had. Not every library
// item carries one (Stremio only fills it in for some entries); metahub serves
// artwork keyed by the IMDB id, which every item has, so derive it rather than
// showing a blank slot.
// The IMDB id buried in a Stremio id, or "". Ids are colon-separated and the
// IMDB part is not always first: an episode is "tt123:1:3", but a trailer the
// user watched is "yt_id:trailer:tt1999890" -- those were the ones showing up
// with no artwork.
static std::string imdbIdOf(const std::string& id)
{
    size_t from = 0;
    while (from <= id.size())
    {
        size_t sep      = id.find(':', from);
        std::string seg = id.substr(from, sep == std::string::npos
                                              ? std::string::npos
                                              : sep - from);
        if (seg.rfind("tt", 0) == 0 && seg.size() > 2 &&
            seg.find_first_not_of("0123456789", 2) == std::string::npos)
            return seg;
        if (sep == std::string::npos) break;
        from = sep + 1;
    }
    return "";
}

static std::string artUrlOrMetahub(const std::string& id, const std::string& url)
{
    if (!url.empty()) return url;
    std::string imdb = imdbIdOf(id);
    if (imdb.empty())
    {
        brls::Logger::info("[stremio] no poster and no imdb id for {}", id);
        return "";
    }
    brls::Logger::info("[stremio] no poster field for {}, using metahub {}", id,
                       imdb);
    return "https://images.metahub.space/poster/medium/" + imdb + "/img";
}

// Rewrites a metahub URL to one of its size variants. It serves them all off the
// same path, so this is the whole difference between a list thumbnail and a
// background.
static std::string metahubSize(std::string u, const char* want)
{
    for (const char* have : { "/small/", "/medium/", "/large/" })
    {
        size_t k = u.find(have);
        if (k != std::string::npos) return u.replace(k, strlen(have), want);
    }
    return u;
}

void fetchPosterAsync(const std::string& id, const std::string& url,
                      std::function<void(std::string)> done)
{
    if (id.empty())
    {
        done("");
        return;
    }

    std::string src = artUrlOrMetahub(id, url);
    if (src.empty())
    {
        done("");
        return;
    }

    // Already cached: skip the network entirely. Posters never change, so a hit
    // is final -- this is what keeps a scroll through the library instant.
    std::string hit = cachedPosterPath(id);
    if (!hit.empty())
    {
        done(hit);
        return;
    }

    // A Switch row shows the poster at ~100px wide, so pulling the full-size art
    // would be several hundred KB per item to then throw the pixels away on
    // downscale. Ask for the small variant -- that IS the compression, done
    // server-side.
    downloadImageAsync(id, metahubSize(src, "/small/"), posterCachePath(id),
                       done);
}

void fetchHqArtAsync(const std::string& id, const std::string& url,
                     std::function<void(std::string)> done)
{
    if (id.empty())
    {
        done("");
        return;
    }

    std::string src = artUrlOrMetahub(id, url);
    if (src.empty())
    {
        done("");
        return;
    }

    // Kept apart from the thumbnail cache: same id, different image. Sharing the
    // key would mean whichever screen ran first decided the quality for both.
    std::string path = posterCachePath(id) + ".hq.jpg";
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        std::fclose(f);
        done(path);
        return;
    }

    downloadImageAsync(id, metahubSize(src, "/large/"), path, done);
}

// The account's addon collection, fetched once. It only changes when the user
// installs an addon on another device, which cannot happen mid-session, and it
// was being re-fetched for every episode and every film -- a full round-trip
// before each list could be shown. Only ever touched on the UI thread (the
// brls::sync below), so it needs no lock.
static AddonsResult addonCache;

void fetchAddonsAsync(const std::string& authKey,
                      std::function<void(AddonsResult)> done)
{
    if (addonCache.ok)
    {
        done(addonCache);
        return;
    }

    brls::async([authKey, done]() {
        AddonsResult r;
        std::string body =
            "{\"authKey\":\"" + json::escape(authKey) + "\",\"update\":true}";
        std::string resp, err;

        if (!http::postJson("https://api.strem.io/api/addonCollectionGet", body, resp,
                      err))
        {
            r.error = err;
        }
        else
        {
            r.ok = true;
            for (const auto& o : json::objects(resp, "addons"))
            {
                Addon a;
                a.name = json::str(o, "name");
                std::string url = json::str(o, "transportUrl");
                if (url.empty()) continue;

                // Every resource hangs off the manifest's directory.
                const std::string suffix = "/manifest.json";
                a.base = (url.size() > suffix.size() &&
                          url.compare(url.size() - suffix.size(), suffix.size(),
                                      suffix) == 0)
                             ? url.substr(0, url.size() - suffix.size())
                             : url;
                if (a.name.empty()) a.name = a.base;

                // "resources" is either ["stream",...] or a list of objects with
                // a "name" -- both shapes are legal in the addon spec.
                auto res = json::strings(o, "resources");
                if (res.empty())
                    for (const auto& ro : json::objects(o, "resources"))
                        res.push_back(json::str(ro, "name"));
                for (const auto& x : res)
                {
                    if (x == "meta") a.hasMeta = true;
                    if (x == "stream") a.hasStream = true;
                }
                a.types = json::strings(o, "types");
                r.addons.push_back(a);
            }
            brls::Logger::info("[stremio] {} addons ({} with stream)",
                               r.addons.size(),
                               [&] {
                                   int n = 0;
                                   for (auto& a : r.addons) n += a.hasStream;
                                   return n;
                               }());
        }
        brls::sync([done, r]() {
            // Only a good response is worth keeping: caching a failure would
            // pin the app to it for the rest of the session.
            if (r.ok) addonCache = r;
            done(r);
        });
    });
}

void clearAddonCache() { addonCache = AddonsResult(); }

void fetchMetaAsync(const std::string& addonBase, const std::string& type,
                    const std::string& id, std::function<void(MetaResult)> done)
{
    brls::async([addonBase, type, id, done]() {
        MetaResult r;
        std::string url =
            addonBase + "/meta/" + http::urlEncode(type) + "/" + http::urlEncode(id) + ".json";
        std::string resp, err;
        if (!http::get(url, resp, err))
        {
            r.error = err;
        }
        else
        {
            r.ok = true;
            for (const auto& o : json::objects(resp, "videos"))
            {
                Video v;
                v.id      = json::str(o, "id");
                v.season  = (int)json::integer(o, "season", -1);
                v.episode = (int)json::integer(o, "episode", -1);
                v.title   = json::str(o, "title");
                if (v.title.empty()) v.title = json::str(o, "name");
                v.thumbnail = json::str(o, "thumbnail");
                if (v.id.empty()) continue;
                r.videos.push_back(v);
            }
            if (r.videos.empty() && resp.find("\"meta\"") == std::string::npos)
                r.error = "Reponse inattendue de l'addon";
            brls::Logger::info("[stremio] meta {} -> {} videos", id,
                               r.videos.size());
        }
        brls::sync([done, r]() { done(r); });
    });
}

void fetchStreamsAsync(const std::string& addonBase, const std::string& type,
                       const std::string& id,
                       std::function<void(StreamsResult)> done)
{
    brls::async([addonBase, type, id, done]() {
        StreamsResult r;
        std::string url = addonBase + "/stream/" + http::urlEncode(type) + "/" +
                          http::urlEncode(id) + ".json";
        std::string resp, err;
        if (!http::get(url, resp, err))
        {
            r.error = err;
        }
        else
        {
            r.ok = true;
            for (const auto& o : json::objects(resp, "streams"))
            {
                Stream s;
                s.name     = json::str(o, "name");
                s.title    = json::str(o, "title");
                s.infoHash = json::str(o, "infoHash");
                s.url      = json::str(o, "url");
                // Season packs bundle every episode in one torrent; the addon
                // says which file this stream is. Absent -> -1 (largest file).
                s.fileIdx  = (int)json::integer(o, "fileIdx", -1);
                if (s.name.empty() && s.title.empty()) s.name = "Source";
                r.streams.push_back(s);
            }
            brls::Logger::info("[stremio] streams {} -> {}", id, r.streams.size());
        }
        brls::sync([done, r]() { done(r); });
    });
}

} // namespace stremio

StremioTab::StremioTab()
{
    this->setAxis(brls::Axis::COLUMN);
    this->setGrow(1.0f);

    brls::Theme theme = brls::Application::getTheme();
    // text_disabled is near-black on the dark theme; use the same light gray the
    // empty state uses so hints stay readable.
    NVGcolor hintColor = nvgRGB(190, 190, 195);

    // ---- sign-in form ----------------------------------------------------
    loginBox = new brls::Box();
    loginBox->setAxis(brls::Axis::COLUMN);
    loginBox->setJustifyContent(brls::JustifyContent::CENTER);
    loginBox->setAlignItems(brls::AlignItems::CENTER);
    loginBox->setGrow(1.0f);
    loginBox->setPadding(0, 60, 0, 60);

    auto* title = new brls::Label();
    title->setText("Stremio");
    title->setFontSize(28);
    title->setTextColor(theme.getColor("brls/text"));
    title->setMargins(0, 0, 16, 0);
    loginBox->addView(title);

    auto* hint = new brls::Label();
    hint->setText("Sign in to your Stremio account to see your library.");
    hint->setFontSize(18);
    hint->setTextColor(hintColor);
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    hint->setMargins(0, 0, 28, 0);
    loginBox->addView(hint);

    emailLabel = new brls::Label();
    emailLabel->setText("No email entered");
    emailLabel->setFontSize(20);
    emailLabel->setTextColor(theme.getColor("brls/text"));
    emailLabel->setMargins(0, 0, 20, 0);
    loginBox->addView(emailLabel);

    auto* emailBtn = new brls::Button();
    emailBtn->setText("Email");
    emailBtn->setWidth(360.0f);
    emailBtn->setMargins(0, 0, 12, 0);
    emailBtn->registerClickAction([this](brls::View*) { promptEmail(); return true; });
    loginBox->addView(emailBtn);

    auto* passBtn = new brls::Button();
    passBtn->setText("Password");
    passBtn->setWidth(360.0f);
    passBtn->setMargins(0, 0, 24, 0);
    passBtn->registerClickAction([this](brls::View*) { promptPassword(); return true; });
    loginBox->addView(passBtn);

    loginBtn = new brls::Button();
    loginBtn->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    loginBtn->setText("Sign in");
    loginBtn->setWidth(360.0f);
    loginBtn->registerClickAction([this](brls::View*) { doLogin(); return true; });
    loginBox->addView(loginBtn);

    statusLabel = new brls::Label();
    statusLabel->setText("");
    statusLabel->setFontSize(18);
    statusLabel->setTextColor(hintColor);
    statusLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    // Fixed height: without it the column re-centres every time this text
    // changes, so the whole form jumped around on a failed sign-in.
    statusLabel->setHeight(28.0f);
    statusLabel->setMargins(20, 0, 0, 0);
    loginBox->addView(statusLabel);

    this->addView(loginBox);

    // ---- library ---------------------------------------------------------
    libraryBox = new brls::Box();
    libraryBox->setAxis(brls::Axis::COLUMN);
    libraryBox->setGrow(1.0f);
    // setPadding(top, right, bottom, left)
    //  - no bottom padding: the list scrolls, so it only wasted a strip of
    //    screen and left the last row floating.
    //  - no right padding: ScrollingFrame pins its indicator to its OWN right
    //    edge (getWidth() - 14), so any padding here pushed the bar inwards on
    //    top of the row text. The frame now reaches the screen edge and the
    //    rows carry the inset instead.
    libraryBox->setPadding(32.0f, 0.0f, 0.0f, 60.0f);
    libraryBox->setVisibility(brls::Visibility::GONE);

    libStatus = new brls::Label();
    libStatus->setText("");
    libStatus->setFontSize(20);
    libStatus->setTextColor(hintColor);
    libStatus->setMargins(0, 60.0f, 16, 0);  // matches the inset the rows carry
    libraryBox->addView(libStatus);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    libList = new brls::Box();
    libList->setAxis(brls::Axis::COLUMN);
    // No margin here: ScrollingFrame::setContentView() detaches the content view
    // and forces setWidth(frameWidth), so anything set on this box is ignored.
    // The inset has to live on the rows themselves (see showLibrary).
    scroll->setContentView(libList);
    libraryBox->addView(scroll);

    this->addView(libraryBox);

    focusSub    = brls::Application::getGlobalFocusChangeEvent()->subscribe(
        [this](brls::View* v) { onGlobalFocus(v); });
    focusSubbed = true;

    // Y reloads the library on demand -- fires only while focus is on this tab.
    this->registerAction(
        "Reload", brls::BUTTON_Y,
        [this](brls::View*) {
            if (!authKey.empty()) loadLibrary();
            return true;
        },
        false, false, brls::SOUND_NONE);

    // Already signed in from a previous run: skip straight to the library.
    std::string saved = stremio::loadAuthKey();
    if (!saved.empty())
        onAuthenticated(saved, false);
}

StremioTab::~StremioTab()
{
    // Switching tabs deletes us immediately, but the network requests we
    // started keep running and land on the UI thread afterwards. Tell them we
    // are gone -- otherwise a fast tab switch crashes on a freed `this` (or a
    // freed poster Image).
    *alive     = false;
    *rowsAlive = false;
    if (focusSubbed)
        brls::Application::getGlobalFocusChangeEvent()->unsubscribe(focusSub);
}

void StremioTab::onGlobalFocus(brls::View* focused)
{
    // Focus landing back on a library row after playback pushed new progress:
    // reload once so the bars are current. Tracking the generation we last
    // rendered (rather than a shared flag) means the reload -- which re-fires
    // focus events -- does not loop, and a deeper list consuming the signal
    // does not rob us of it.
    if (!libList || !focused || !isUnder(focused, libList)) return;
    if (stremio::libraryGen() == seenGen) return;
    seenGen = stremio::libraryGen();
    // Defer the actual reload: we are inside a focus-change dispatch (the
    // activity above just popped and restored focus here), and tearing the row
    // tree down with clearViews() mid-dispatch is what crashed. brls::sync runs
    // it on the next UI-loop tick, at a safe point.
    auto live = alive;
    brls::sync([this, live]() {
        if (*live) loadLibrary();
    });
}

void StremioTab::promptEmail()
{
    brls::Application::getImeManager()->openForText(
        [this, live = alive](std::string out) {
            if (!*live) return;
            email = out;
            emailLabel->setText(email.empty() ? "No email entered" : email);
        },
        "Stremio email", "", 128, email);
}

void StremioTab::promptPassword()
{
    brls::Application::getImeManager()->openForText(
        [this, live = alive](std::string out) {
            if (!*live) return;
            password = out;
            // Never echo the password back to the screen.
            statusLabel->setText(password.empty() ? "" : "Password entered");
        },
        "Stremio password", "", 128, "");
}

void StremioTab::doLogin()
{
    if (email.empty() || password.empty())
    {
        dialog("Enter an email and a password.");
        return;
    }

    statusLabel->setText("Signing in...");
    loginBtn->setState(brls::ButtonState::DISABLED);

    stremio::loginAsync(email, password, [this, live = alive](stremio::LoginResult r) {
        if (!*live) return;
        loginBtn->setState(brls::ButtonState::ENABLED);
        statusLabel->setText("");
        if (r.ok)
        {
            // Only for the "signed in as" line in Options; the address the user
            // typed is the one they signed in with.
            stremio::saveEmail(email);
            onAuthenticated(r.authKey, true);
        }
        else
            dialog("Sign-in failed: " + r.error);
    });
}

void StremioTab::onAuthenticated(const std::string& key, bool announce)
{
    authKey = key;
    if (!stremio::saveAuthKey(key))
        brls::Logger::warning("[stremio] could not persist authKey");

    loginBox->setVisibility(brls::Visibility::GONE);
    // GONE alone leaves its buttons in the focus ring (see setSubtreeFocusable).
    setSubtreeFocusable(loginBox, false);
    libraryBox->setVisibility(brls::Visibility::VISIBLE);

    // The focus is still on the "Sign in" button we just made non-focusable
    // inside a GONE box, and there is nothing in the library to hand it to yet
    // -- it has not loaded. Leaving it there is not just an invisible cursor at
    // 0,0: pushActivity() stores the focused view on Application::focusStack,
    // and popActivity() hands it back via giveFocus(), which does NOTHING for a
    // non-focusable view. currentFocus would stay on the dialog's button, which
    // popActivity then deletes -- so dismissing "Signed in" left the focus
    // dangling and the next frame drew the highlight on freed memory.
    //
    // So park the focus on the library box itself (no highlight of its own)
    // until there are rows to move it to; showLibrary takes it from there.
    libraryBox->setFocusable(true);
    libraryBox->setHideHighlight(true);
    brls::Application::giveFocus(libraryBox);

    if (announce)
        dialog("Signed in");

    loadLibrary();
}

void StremioTab::parkFocusOffList()
{
    // A reload runs while a RowCell is the focused view (returning to the
    // library, or a rapid second reload). clearViews() frees that row, and the
    // next giveFocus() then dereferences it in onFocusLost() -- an immediate
    // crash. libraryBox is alive throughout; the caller rebuilds and hands focus
    // back to a row afterwards.
    brls::View* cur = brls::Application::getCurrentFocus();
    if (cur && libList && isUnder(cur, libList))
    {
        libraryBox->setFocusable(true);
        libraryBox->setHideHighlight(true);
        brls::Application::giveFocus(libraryBox);
    }
}

void StremioTab::loadLibrary()
{
    libStatus->setText("Loading library...");
    parkFocusOffList();

    *rowsAlive = false;                       // drop any in-flight poster fetch
    rowsAlive  = std::make_shared<bool>(true);
    libList->clearViews();

    // Warm the addon cache now, in parallel with the library: by the time a
    // title is picked it is already there, instead of costing a round-trip
    // before the addon list can be shown. The result is dropped -- the point is
    // the cache it fills (see fetchAddonsAsync).
    stremio::fetchAddonsAsync(authKey, [](stremio::AddonsResult) {});

    stremio::fetchLibraryAsync(authKey, [this, live = alive](stremio::LibraryResult r) {
        if (!*live) return;
        if (!r.ok)
        {
            libStatus->setText("Error");
            dialog("Library unavailable: " + r.error);
            return;
        }
        showLibrary(r.items);
    });
}

void StremioTab::showLibrary(const std::vector<stremio::LibItem>& items)
{
    parkFocusOffList();                       // never clearViews a focused row
    *rowsAlive = false;                       // same: these rows are about to die
    rowsAlive  = std::make_shared<bool>(true);
    libList->clearViews();

    if (items.empty())
    {
        libStatus->setText("Library is empty");
        return;
    }
    // "Library" here means what Stremio calls a library item: titles explicitly
    // added, plus whatever you watched (which Stremio auto-adds as a temporary
    // entry). It is not a full watch history.
    libStatus->setText(std::to_string(items.size()) +
                       " item(s) in your Stremio library");

    brls::Box* lastRow = nullptr;
    for (const auto& it : items)
    {
        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(140.0f);  // sized off the poster, not the text
        row->setPaddingLeft(16.0f);
        row->setPaddingRight(24.0f);
        // Margin, not padding, and on the row rather than the list: the focus
        // highlight draws OUTSIDE the row's bounds and the frame scissors to its
        // own width, so a row reaching the edge has its glow clipped. Padding
        // only moved the text and left the box (and its glow) at the edge; the
        // row itself has to stop short. Also keeps the scrolling indicator,
        // pinned to the frame's edge, in a lane of its own.
        row->setMarginRight(40.0f);
        row->setCornerRadius(6.0f);
        row->setFocusable(true);
        // Series open a season/episode picker, films go straight to sources.
        std::string key = authKey;
        row->registerClickAction([key, it](brls::View*) {
            openLibraryItem(key, it);
            return true;
        });
        // Tap gesture so the touchscreen works too (A-only otherwise).
        row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

        // Poster. Posters are 2:3, so fit rather than stretch. It arrives
        // asynchronously: the row draws immediately with an empty slot and the
        // artwork fills in, instead of the whole list waiting on the network.
        auto* art = new brls::Image();
        art->setDimensions(80.0f, 120.0f);  // 2:3, the poster aspect
        art->setScalingType(brls::ImageScalingType::FIT);
        art->setMarginRight(22.0f);
        row->addView(art);

        auto alive = rowsAlive;  // list may be rebuilt before the art lands
        stremio::fetchPosterAsync(it.id, it.poster,
                                  [art, alive](std::string path) {
                                      if (!*alive || path.empty()) return;
                                      art->setImageFromFile(path);
                                  });

        // Name over an optional watch-progress bar (where the account is in
        // this film / the show's last watched episode, from the library state).
        auto* textCol = new brls::Box();
        textCol->setAxis(brls::Axis::COLUMN);
        textCol->setJustifyContent(brls::JustifyContent::CENTER);
        textCol->setGrow(1.0f);

        auto* name = new brls::Label();
        name->setText(it.name);
        name->setFontSize(26.0f);  // scaled to the taller row
        name->setSingleLine(true);
        textCol->addView(name);

        double prog = it.progress();

        // For a show mid-episode, name the episode under the title: the videoId
        // is "ttID:season:episode", so the last two ':'-parts are what we show.
        // (The episode's own title needs a meta fetch we do not do per row.)
        if (prog > 0.005 && it.type == "series" && !it.videoId.empty())
        {
            std::string se = it.videoId;
            size_t p2 = se.rfind(':');
            size_t p1 = p2 == std::string::npos ? p2 : se.rfind(':', p2 - 1);
            if (p1 != std::string::npos && p2 != std::string::npos)
            {
                std::string ep = "Season " + se.substr(p1 + 1, p2 - p1 - 1) +
                                 " · Episode " + se.substr(p2 + 1);
                auto* epl = new brls::Label();
                epl->setText(ep);
                epl->setFontSize(18.0f);
                epl->setTextColor(nvgRGB(150, 150, 155));
                epl->setSingleLine(true);
                epl->setMarginTop(4.0f);
                textCol->addView(epl);
            }
        }

        if (prog > 0.005)
        {
            auto* track = new brls::Box();
            track->setWidthPercentage(100.0f);
            track->setHeight(5.0f);
            track->setCornerRadius(2.5f);
            track->setBackgroundColor(nvgRGBA(255, 255, 255, 40));
            track->setMarginTop(10.0f);

            auto* fill = new brls::Box();
            fill->setWidthPercentage((float)(prog * 100.0));
            fill->setHeight(5.0f);
            fill->setCornerRadius(2.5f);
            fill->setBackgroundColor(
                brls::Application::getTheme().getColor("brls/accent"));
            track->addView(fill);
            textCol->addView(track);
        }
        row->addView(textCol);

        auto* type = new brls::Label();
        type->setText(it.type == "series" ? "Show" : "Movie");
        type->setFontSize(19.0f);
        type->setTextColor(nvgRGB(150, 150, 155));
        type->setMarginLeft(16.0f);
        // Keep it on one line and let it hold its width: the full-width progress
        // bar in textCol otherwise squeezed this label until it wrapped.
        type->setSingleLine(true);
        type->setShrink(0.0f);
        row->addView(type);

        libList->addView(row);
        lastRow = row;
    }

    // Only the first row gets the escape route: every other one has a row above
    // it to move to.
    // Hand the parked focus (see onAuthenticated) to a real row, and stop the
    // box standing in for one -- a focusable Box returns *itself* from
    // getDefaultFocus(), which would keep the rows unreachable.
    if (!libList->getChildren().empty())
    {
        brls::View* focus = brls::Application::getCurrentFocus();
        bool parked = !focus || focus == libraryBox || isUnder(focus, loginBox);
        libraryBox->setFocusable(false);
        if (parked) brls::Application::giveFocus(libList->getChildren()[0]);
    }

    if (stremio::libraryUpTarget && !libList->getChildren().empty())
        libList->getChildren()[0]->setCustomNavigationRoute(
            brls::FocusDirection::UP, stremio::libraryUpTarget);

    // Breathing room under the last row, so scrolled to the end it sits above
    // the screen edge instead of against it. It goes on the row rather than on
    // libList: setContentView() ignores that box's own margins, and a padding
    // there would apply whether or not there is anything to scroll.
    if (lastRow) lastRow->setMarginBottom(32.0f);
}
