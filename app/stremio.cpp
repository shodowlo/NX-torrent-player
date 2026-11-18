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

#include <algorithm>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "appdata.hpp"
#include "browse.hpp"
#include "config.hpp"
#include "http.hpp"
#include "json.hpp"
#include "theme.hpp"
#include "i18n.hpp"

namespace
{

// config::listStyle, as compared here -- the list builders further down, and
// the Left/Right handling in the tab, which has to leave the directions alone
// when the titles are laid out horizontally. Anything else means the default.
constexpr const char* kStyleClassic = "classic";
constexpr const char* kStylePosters = "posters";

bool posterStyle() { return config::get().listStyle == kStylePosters; }

// The screen inset the list gives up in the poster style so the strips can run
// to the edge: carried by the first card of a strip, and by everything else on
// the page (the search bar, the section headings).
constexpr float kPosterInset = 60.0f;

// The row styles' own left padding (newRowShell), so a heading lines up with
// the posters under it rather than sitting inside them.
constexpr float kRowInset = 16.0f;

// The card style's two shared measures: the height the poster, the panel and
// the row all share (the cursor wraps them, so they must agree), and the radius
// the poster, the panel and that cursor are all drawn with.
constexpr float kCardHeight = 132.0f;
constexpr float kCardRadius = 12.0f;

// The poster style: an upright card, artwork on top and the text under it. The
// art is exactly 2:3, the shape a poster is drawn in, so filling the box takes
// nothing off the sides and next to nothing off the top and bottom (the usual
// source is 680x1000, a hair short of 2:3). The whole card fits the shortest
// screen the app runs on: 720 logical minus the header, the footer and the
// insets finishList adds.
constexpr float kPosterCardW = 210.0f;
constexpr float kPosterArtH  = 315.0f;
constexpr float kPosterCardH = 434.0f;

// How much of a catalog a section shows before the See More tile. A Cinemeta
// catalog is a hundred titles; building all of them (and asking for a hundred
// posters) is what made Movies and Shows slow to appear.
constexpr size_t kSectionMax = 15;

// The Stremio tab's field, painted behind a pushed page so it does not land on
// borealis' flat theme colour. Same gradient as BrowserFrame draws (top-right
// to bottom-left), read from the scheme as it draws so it follows the accent.
class GradientFrame : public brls::AppletFrame
{
  public:
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override
    {
        nvgBeginPath(vg);
        nvgRect(vg, x, y, w, h);
        nvgFillPaint(vg, nvgLinearGradient(vg, x + w, y, x, y + h,
                                           theme::gradTop(), theme::gradBottom()));
        nvgFill(vg);
        brls::AppletFrame::draw(vg, x, y, w, h, style, ctx);
    }
};

// How close to the end of a section's scroll counts as "the user is at the
// bottom" -- roughly one line of posters, so the next page is asked for while
// there is still something to look at.
constexpr float kGrowMargin = 480.0f;

// A section page that says when its scroll nears the bottom, so the catalog can
// be extended in place. borealis' ScrollingFrame fires no scroll event, so this
// is a per-frame comparison of its offset against the content it holds -- which
// has the advantage of catching a touch flick as well as a focus step. It fires
// on EVERY frame the scroll sits near the bottom; the handler is what
// de-duplicates (see the loading/exhausted flags in openSection).
class GrowingSection : public brls::Box
{
  public:
    std::function<void()> onNearBottom;
    brls::ScrollingFrame* scroll = nullptr;
    brls::Box* list              = nullptr;

    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override
    {
        brls::Box::draw(vg, x, y, w, h, style, ctx);
        if (!onNearBottom || !scroll || !list) return;
        float visible = scroll->getHeight();
        float content = list->getHeight();
        // Nothing to scroll yet: either the page is still being laid out
        // (heights are 0 before the first layout pass) or it all fits, and
        // asking for more of a catalog nobody has scrolled is wrong.
        if (content <= visible || visible <= 0.0f) return;
        if (scroll->getContentOffsetY() + visible >= content - kGrowMargin)
            onNearBottom();
    }
};

// Holds a page the tab has already built -- the full contents of a section.
// Footer only: the header would repeat the title the page already carries.
class SectionActivity : public brls::Activity
{
  public:
    SectionActivity(brls::View* content, std::function<void()> onGone)
        : content(content), onGone(std::move(onGone))
    {
    }

    // Retires the page's liveness tokens. B pops this screen while its poster
    // downloads -- and possibly a catalog page -- are still in flight, and
    // every one of them holds a raw pointer into the view tree that is about to
    // be freed. The tab's own token is no good here: the tab is still alive, so
    // it would not stop any of them.
    ~SectionActivity() override
    {
        if (onGone) onGone();
    }

    brls::View* createContentView() override
    {
        auto* frame = new GradientFrame();
        frame->pushContentView(content);
        frame->setHeaderVisibility(brls::Visibility::GONE);
        return frame;
    }

  private:
    brls::View* content;
    std::function<void()> onGone;
};

// The default meta provider. Every catalog the tab shows comes from it, and so
// does the genre list on a section page.
constexpr const char* kCinemeta   = "https://v3-cinemeta.strem.io";

constexpr const char* kLoginUrl   = "https://api.strem.io/api/login";
constexpr const char* kGetUserUrl = "https://api.strem.io/api/getUser";
// Stremio's link service (stremio-core's LinkRequest, API v2).
constexpr const char* kLinkCreateUrl = "https://link.stremio.com/api/v2/create?type=Create";
constexpr const char* kLinkReadUrl   = "https://link.stremio.com/api/v2/read?type=Read&code=";
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

// Ids the account currently holds, refreshed by every fetchLibraryAsync and
// edited in place by setLibraryMemberAsync. UI thread only.
static std::set<std::string> g_libIds;
// g_libIds being empty is ambiguous on its own -- an account with nothing in it
// looks exactly like one that has never been read.
static bool g_libFetched = false;

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

// Sink that puts the library item count in the header (see the header comment).
static std::function<void(const std::string&)> libraryCountSink;

void setLibraryCountSink(std::function<void(const std::string&)> sink)
{
    libraryCountSink = std::move(sink);
}

static void setLibraryCount(const std::string& text)
{
    if (libraryCountSink) libraryCountSink(text);
}

// The live Stremio tab's view cycler, so R/L work from the header tab bar too.
static std::function<void(int)> viewCycler;

void setViewCycler(std::function<void(int)> cycler)
{
    viewCycler = std::move(cycler);
}

// The header's view tab bar (top-right). The sink, set by main.cpp, highlights
// the active view (index) or hides the bar (-1); reportView pushes the current
// view to it. The selector, set by the live tab, lets a header button jump to a
// view. Mirrors the viewCycler wiring above.
static std::function<void(int)> viewTabSink;
static std::function<void(int)> viewSelector;

void setViewTabSink(std::function<void(int)> sink) { viewTabSink = std::move(sink); }
void setViewSelector(std::function<void(int)> sel) { viewSelector = std::move(sel); }

// Called by the tab whenever its view (or sign-in state) changes; -1 hides the
// bar. A no-op if the header never registered a sink (e.g. the PC test build).
static void reportView(int index)
{
    if (viewTabSink) viewTabSink(index);
}

void selectActiveView(int index)
{
    if (viewSelector) viewSelector(index);
}

// Button labels, index-matched to the View enum / cycle order.
const std::vector<std::string>& viewLabels()
{
    static const std::vector<std::string> labels = {
        std::string(" ") + tr("Continue"),
        std::string(" ") + tr("Movies"),
        std::string(" ") + tr("Shows"),
        std::string(" ") + tr("Library"),
        std::string(" ") + tr("Search"),
    };
    return labels;
}

void cycleActiveView(int dir)
{
    if (viewCycler) viewCycler(dir);
}

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
                r.error = msg.empty() ? tr("Wrong email or password") : msg;
            }
        }
        brls::sync([done, r]() { done(r); });
    });
}

void createLinkAsync(std::function<void(LinkCode)> done)
{
    brls::async([done]() {
        LinkCode c;
        std::string resp, err;
        if (!http::get(kLinkCreateUrl, resp, err))
        {
            c.error = err;
        }
        else
        {
            c.code = json::str(resp, "code");
            c.link = json::str(resp, "link");
            std::string qr = json::str(resp, "qrcode");
            if (c.code.empty() || qr.empty())
            {
                std::string msg = json::str(resp, "message");
                c.error = msg.empty() ? tr("unexpected answer") : msg;
            }
            // Stremio renders the QR itself (a 396px PNG with its quiet zone),
            // so there is no QR encoder to carry here.
            else if (!http::get(qr, c.qrPng, err, "Accept: image/png"))
            {
                c.error = err;
            }
            else
            {
                c.ok = true;
            }
        }
        brls::Logger::info("[stremio] link code {} ({})", c.code,
                           c.ok ? "ok" : c.error);
        brls::sync([done, c]() { done(c); });
    });
}

void readLinkAsync(const std::string& code, std::function<void(LoginResult)> done)
{
    brls::async([code, done]() {
        LoginResult r;
        std::string resp, err;
        std::string url = std::string(kLinkReadUrl) + http::urlEncode(code);
        if (!http::get(url, resp, err))
            r.error = err;
        else
        {
            r.authKey = json::str(resp, "authKey");
            r.ok      = !r.authKey.empty();
            if (!r.ok) r.error = json::str(resp, "message");
        }
        brls::sync([done, r]() { done(r); });
    });
}

void fetchEmailAsync(const std::string& authKey,
                     std::function<void(std::string)> done)
{
    brls::async([authKey, done]() {
        std::string body = "{\"type\":\"GetUser\",\"authKey\":\"" +
                           json::escape(authKey) + "\"}";
        std::string resp, err, email;
        if (http::postJson(kGetUserUrl, body, resp, err))
            email = json::str(resp, "email");
        brls::sync([done, email]() { done(email); });
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
                    // Stremio flags entries removed=true (dropped from the "+
                    // Library" grid) and temp=true (auto-added by watching, i.e.
                    // Continue Watching). Keep BOTH: Continue Watching is driven by
                    // watch state, so a removed-but-watched title still belongs
                    // there (Stremio shows it too) -- it was skipping these that
                    // hid shows finished on another device. The Library view
                    // filters them out via the flags below.
                    LibItem it;
                    it.removed = json::boolean(o, "removed");
                    it.temp    = json::boolean(o, "temp");
                    if (it.removed) nRemoved++;
                    if (it.temp) nTemp++;
                    it.id     = json::str(o, "_id");
                    it.name   = json::str(o, "name");
                    it.type   = json::str(o, "type");
                    it.poster = json::str(o, "poster");
                    it.year   = json::str(o, "year");
                    it.mtime  = json::str(o, "_mtime");
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
                // What inLibrary() answers from. Everything the account
                // holds, whether or not it survived the filters above -- a
                // "temp" (auto-added by watching) item IS in the library as far
                // as the +/- button is concerned; only "removed" is not.
                g_libIds.clear();
                g_libFetched = true;
                for (const auto& o : objs)
                {
                    std::string id = json::str(o, "_id");
                    if (!id.empty() && !json::boolean(o, "removed", false))
                        g_libIds.insert(id);
                }

                // Most recently viewed first: watching bumps _mtime, and the ISO
                // timestamps compare lexicographically. Items without an _mtime
                // (should not happen) sort to the bottom.
                std::stable_sort(r.items.begin(), r.items.end(),
                                 [](const LibItem& a, const LibItem& b) {
                                     return a.mtime > b.mtime;
                                 });
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

bool isStreamAddonHidden(const std::string& name)
{
    // Lives here rather than beside the source list that first needed it: the
    // Account screen has to mark the same addons, and two copies of a blocklist
    // is one copy too many.
    // Not translatable: these are matched against the names the account's
    // addons report, which are what they are whatever language the UI is in.
    static const char* kHidden[] = {
        "WatchHub", "Local Files", "Peario",
        "Public Domain Movies", "Public Domain Foreign Movies",
    };
    for (const char* bad : kHidden)
        if (name.find(bad) != std::string::npos) return true;
    return false;
}

int libraryCount()
{
    return g_libFetched ? (int)g_libIds.size() : -1;
}

bool inLibrary(const std::string& itemId)
{
    return !itemId.empty() && g_libIds.count(itemId) > 0;
}

// A libraryItem built from scratch, for a title the account has never seen --
// anything opened from a catalog or a search. Stremio fills the rest in itself;
// these are the fields it will not do without.
static std::string newLibraryItem(const LibItem& item, const std::string& iso)
{
    std::string o = "{";
    o += "\"_id\":\"" + json::escape(item.id) + "\",";
    o += "\"name\":\"" + json::escape(item.name) + "\",";
    o += "\"type\":\"" + json::escape(item.type.empty() ? "movie" : item.type) +
         "\",";
    o += "\"poster\":\"" + json::escape(item.poster) + "\",";
    o += "\"posterShape\":\"poster\",";
    o += "\"background\":\"\",\"logo\":\"\",";
    o += "\"year\":\"" + json::escape(item.year) + "\",";
    o += "\"_ctime\":\"" + iso + "\",\"_mtime\":\"" + iso + "\",";
    o += "\"removed\":false,\"temp\":false,";
    o += "\"state\":{\"lastWatched\":\"\",\"timeOffset\":0,\"duration\":0,"
         "\"video_id\":\"\",\"watched\":\"\",\"flaggedWatched\":0,"
         "\"noNotif\":false,\"season\":0,\"episode\":0,"
         "\"overallTimeWatched\":0,\"timesWatched\":0}";
    o += "}";
    return o;
}

void setLibraryMemberAsync(const std::string& authKey, const LibItem& item,
                           bool add, std::function<void(bool)> done)
{
    if (authKey.empty() || item.id.empty())
    {
        done(false);
        return;
    }

    // Flip the local answer now, on the UI thread, so the button responds to the
    // press instead of to the round trip. Put back if the API refuses.
    if (add)
        g_libIds.insert(item.id);
    else
        g_libIds.erase(item.id);
    g_libraryGen++;
    markLibraryStale();

    brls::async([authKey, item, add, done]() {
        std::time_t tt = std::time(nullptr);
        std::tm g {};
        gmtime_r(&tt, &g);
        char isoBuf[40];
        std::strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%S.000Z", &g);
        std::string iso  = isoBuf;
        std::string isoq = "\"" + iso + "\"";

        // Read the stored item back first and edit it in place: datastorePut
        // REPLACES what is there, so rebuilding a subset would strip every field
        // this client does not know about (the same reason the watch-state push
        // does it this way).
        std::string body = "{\"authKey\":\"" + json::escape(authKey) +
                           "\",\"collection\":\"libraryItem\",\"ids\":[\"" +
                           json::escape(item.id) + "\"]}";
        std::string resp, err;
        std::string obj;
        if (http::postJson(kLibraryUrl, body, resp, err))
        {
            auto objs = json::objects(resp, "result");
            if (!objs.empty()) obj = objs[0];
        }
        // Never stored: adding has to create it. Removing one that was never
        // there is a no-op we can report as done.
        if (obj.empty())
        {
            if (!add)
            {
                brls::sync([done]() { done(true); });
                return;
            }
            obj = newLibraryItem(item, iso);
        }
        else
        {
            setField(obj, "removed", add ? "false" : "true");
            // "temp" marks an entry auto-added by watching. An explicit + makes
            // it a real library entry; leaving it set would let Stremio drop it
            // again on its own.
            setField(obj, "temp", "false");
            setField(obj, "_mtime", isoq);
        }

        std::string put = "{\"authKey\":\"" + json::escape(authKey) +
                          "\",\"collection\":\"libraryItem\",\"changes\":[" +
                          obj + "]}";
        std::string resp2;
        bool ok = http::postJson("https://api.strem.io/api/datastorePut", put,
                                 resp2, err);
        if (!ok)
            brls::Logger::warning("[stremio] library {} failed: {}",
                                  add ? "add" : "remove", err);
        else
            brls::Logger::info("[stremio] library {} {}", add ? "+" : "-",
                               item.id);

        brls::sync([done, ok, add, id = item.id]() {
            // Put the local answer back if the account did not take it.
            if (!ok)
            {
                if (add)
                    g_libIds.erase(id);
                else
                    g_libIds.insert(id);
            }
            done(ok);
        });
    });
}

void clearWatchStateAsync(const std::string& authKey,
                          const std::string& itemId,
                          std::function<void(bool)> done)
{
    if (authKey.empty() || itemId.empty())
    {
        done(false);
        return;
    }

    // Anything the UI reads before the round trip finishes.
    if (g_lastWatch.itemId == itemId) g_lastWatch = LocalWatch();
    g_libraryGen++;

    brls::async([authKey, itemId, done]() {
        std::string body = "{\"authKey\":\"" + json::escape(authKey) +
                           "\",\"collection\":\"libraryItem\",\"ids\":[\"" +
                           json::escape(itemId) + "\"]}";
        std::string resp, err;
        if (!http::postJson(kLibraryUrl, body, resp, err))
        {
            brls::Logger::warning("[stremio] continue-watching get failed: {}",
                                  err);
            brls::sync([done]() { done(false); });
            return;
        }
        auto objs = json::objects(resp, "result");
        if (objs.empty())
        {
            // Not on the account at all: nothing to clear, and the row it was
            // drawn from is stale. Report success so the UI drops it.
            brls::sync([done]() { done(true); });
            return;
        }
        std::string obj = objs[0];

        // Edit the "state" object's own text, then splice it back -- same
        // reasoning as pushWatchStateAsync: field names repeat outside it.
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
        if (sclose != std::string::npos)
        {
            std::string st = obj.substr(sopen, sclose - sopen + 1);
            // The three fields Continue Watching is derived from: a position, a
            // duration to measure it against, and which video it belongs to.
            setField(st, "timeOffset", "0");
            setField(st, "duration", "0");
            setField(st, "video_id", "\"\"");
            obj.replace(sopen, sclose - sopen + 1, st);
        }

        // A temp entry is one the account holds only because it was watched.
        // Emptying its state would leave it in the library as a blank row, so
        // it goes; anything added on purpose stays where the user put it.
        bool wasTemp = json::boolean(obj, "temp", false);
        if (wasTemp) setField(obj, "removed", "true");

        std::time_t tt = std::time(nullptr);
        std::tm g {};
        gmtime_r(&tt, &g);
        char iso[40];
        std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S.000Z", &g);
        setField(obj, "_mtime", "\"" + std::string(iso) + "\"");

        std::string put = "{\"authKey\":\"" + json::escape(authKey) +
                          "\",\"collection\":\"libraryItem\",\"changes\":[" +
                          obj + "]}";
        std::string resp2;
        bool ok = http::postJson("https://api.strem.io/api/datastorePut", put,
                                 resp2, err);
        if (!ok)
            brls::Logger::warning("[stremio] continue-watching put failed: {}",
                                  err);
        else
            brls::Logger::info("[stremio] continue-watching cleared {}", itemId);
        // g_libIds is UI-thread-only, so the membership edit rides back with
        // the result rather than being made here on the worker.
        brls::sync([done, ok, wasTemp, itemId]() {
            if (ok && wasTemp) g_libIds.erase(itemId);
            done(ok);
        });
    });
}

// Rewrites the library item's watch state on the API. The item is fetched
// back first and edited in place (string surgery on the raw object), because
// datastorePut REPLACES the stored item: sending a rebuilt subset would strip
// whatever fields this client doesn't know about.
void pushWatchStateAsync(const std::string& authKey, const std::string& itemId,
                         const std::string& videoId, double posSec, double durSec)
{
    if (authKey.empty() || itemId.empty() || posSec <= 0) return;

    // Update the local record NOW, on the calling (UI) thread, before the network
    // round-trip. The detail screens read g_lastWatch to draw progress bars and
    // resume points; doing this only after the PUT succeeded meant backing out of
    // playback showed the stale position (the PUT lands seconds later, if at all)
    // -- and wrote g_lastWatch from a worker thread, racing the UI reads.
    g_lastWatch = { itemId, videoId, posSec * 1000.0, durSec * 1000.0 };
    g_libraryGen++;

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
            brls::Logger::info("[stremio] watch-state {} @{}s/{}s pushed",
                               videoId.empty() ? itemId : videoId, (int)posSec,
                               (int)durSec);
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
                    if (x == "subtitles") a.hasSubtitles = true;
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

void clearAddonCache()
{
    addonCache = AddonsResult();
    clearMetaCache();  // signing in as somebody else re-reads everything
}

// Meta answers, kept for the session. An episode list does not change while
// the app is running, and the alternative is a network round trip on a path
// that has to feel instant: brls::async runs its tasks on ONE thread, one after
// another (thread.cpp), so a meta fetch fired while an episode screen is still
// pulling its sources waits for those to finish first. That was the wait on
// the prev/next chevrons -- the series screen has already fetched this exact
// meta, so with a cache they need no request at all.
//
// Bounded, because a MetaResult carries every episode of a series and a long
// session would otherwise walk through a lot of them.
static std::map<std::string, MetaResult> metaCache;
static std::deque<std::string> metaOrder;
constexpr size_t kMetaCacheMax = 8;

void clearMetaCache()
{
    metaCache.clear();
    metaOrder.clear();
}

void fetchMetaAsync(const std::string& addonBase, const std::string& type,
                    const std::string& id, std::function<void(MetaResult)> done)
{
    std::string key = addonBase + "|" + type + "|" + id;
    auto hit        = metaCache.find(key);
    if (hit != metaCache.end())
    {
        // Through the sync queue rather than straight through, so a hit and a
        // miss deliver the same way -- next frame, on the UI thread. Callers
        // push a loading screen before this returns and swap it for the result;
        // answering inside the call would have them pop what they just pushed
        // in the same breath.
        MetaResult r = hit->second;
        brls::sync([done, r]() { done(r); });
        return;
    }

    brls::async([addonBase, type, id, key, done]() {
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

            // The descriptive fields live on the "meta" object itself, alongside
            // the "videos" array. Read them from that block so the flat scanner
            // does not pick a same-named field out of a nested video.
            std::string meta = subObject(resp, "meta");
            const std::string& src = meta.empty() ? resp : meta;
            r.description = json::str(src, "description");
            r.releaseInfo = json::str(src, "releaseInfo");
            if (r.releaseInfo.empty()) r.releaseInfo = json::str(src, "year");
            r.runtime    = json::str(src, "runtime");
            r.imdbRating = json::str(src, "imdbRating");

            for (const auto& o : json::objects(resp, "videos"))
            {
                Video v;
                v.id      = json::str(o, "id");
                v.season  = (int)json::integer(o, "season", -1);
                v.episode = (int)json::integer(o, "episode", -1);
                v.title   = json::str(o, "title");
                if (v.title.empty()) v.title = json::str(o, "name");
                v.thumbnail = json::str(o, "thumbnail");
                v.released  = json::str(o, "released");
                if (v.released.empty()) v.released = json::str(o, "firstAired");
                v.overview  = json::str(o, "overview");
                if (v.overview.empty()) v.overview = json::str(o, "description");
                if (v.id.empty()) continue;
                r.videos.push_back(v);
            }
            if (r.videos.empty() && resp.find("\"meta\"") == std::string::npos)
                r.error = tr("Unexpected answer from the addon");
            brls::Logger::info("[stremio] meta {} -> {} videos", id,
                               r.videos.size());
        }
        brls::sync([done, r, key]() {
            if (r.ok)
            {
                if (metaCache.emplace(key, r).second)
                {
                    metaOrder.push_back(key);
                    while (metaOrder.size() > kMetaCacheMax)
                    {
                        metaCache.erase(metaOrder.front());
                        metaOrder.pop_front();
                    }
                }
            }
            done(r);
        });
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
                if (s.name.empty() && s.title.empty()) s.name = tr("Source");
                r.streams.push_back(s);
            }
            brls::Logger::info("[stremio] streams {} -> {}", id, r.streams.size());
        }
        brls::sync([done, r]() { done(r); });
    });
}

// Where a downloaded subtitle lands. Keyed on the addon-side id when there is
// one (URLs carry tokens that change between calls), sanitised the same way the
// poster cache is -- FAT32 refuses ':' and friends.
static std::string subtitleCachePath(const Subtitle& s)
{
    std::string key = s.id.empty() ? s.url : (s.addon + "_" + s.id);
    std::string safe;
    for (char c : key)
        safe += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
    // Names come from an addon, so cap them; the tail is the distinguishing
    // part of both an id and a URL.
    if (safe.size() > 90) safe = safe.substr(safe.size() - 90);

    // The extension is not cosmetic: mpv picks the subtitle demuxer from it.
    // Read it off the URL's path, before any query string.
    std::string path = s.url.substr(0, s.url.find('?'));
    std::string ext  = ".srt";
    for (const char* e : { ".vtt", ".ass", ".ssa", ".sub" })
        if (path.size() > 4 &&
            path.compare(path.size() - 4, 4, e) == 0)
            ext = e;
    return std::string(APPDATA_SUBS) + "/" + safe + ext;
}

void fetchSubtitlesAsync(const std::string& authKey, const std::string& type,
                         const std::string& videoId,
                         std::function<void(SubtitlesResult)> done)
{
    if (authKey.empty() || videoId.empty())
    {
        SubtitlesResult r;
        r.error = tr("Not a Stremio playback");
        done(r);
        return;
    }

    // Normally free: the collection is cached after the first call of the
    // session (see fetchAddonsAsync).
    fetchAddonsAsync(authKey, [type, videoId, done](AddonsResult ar) {
        if (!ar.ok)
        {
            SubtitlesResult r;
            r.error = ar.error;
            done(r);
            return;
        }

        auto list = std::make_shared<std::vector<Addon>>();
        for (const auto& a : ar.addons)
            if (a.hasSubtitles && a.supportsType(type)) list->push_back(a);

        if (list->empty())
        {
            // Nothing installed that serves subtitles: not a failure, just an
            // empty answer. The player says "no subtitle addon" rather than
            // reporting an error nobody can act on.
            SubtitlesResult r;
            r.ok = true;
            done(r);
            return;
        }

        brls::async([list, type, videoId, done]() {
            SubtitlesResult r;
            // One addon at a time rather than in parallel. These run while the
            // engine is streaming, and the console's socket pool is what the
            // whole app is short of (see the ENOBUFS note in the vendored
            // borealis) -- a couple of extra seconds here costs nothing, the
            // list is fetched long before anyone opens the menu.
            for (const auto& a : *list)
            {
                std::string url = a.base + "/subtitles/" + http::urlEncode(type) +
                                  "/" + http::urlEncode(videoId) + ".json";
                std::string resp, err;
                if (!http::get(url, resp, err))
                {
                    brls::Logger::warning("[stremio] subtitles from {} failed: {}",
                                          a.name, err);
                    if (r.error.empty()) r.error = err;
                    continue;  // one addon down; the others may still answer
                }
                r.ok = true;
                for (const auto& o : json::objects(resp, "subtitles"))
                {
                    Subtitle s;
                    s.url   = json::str(o, "url");
                    s.lang  = json::str(o, "lang");
                    s.id    = json::str(o, "id");
                    s.addon = a.name;
                    if (s.url.empty()) continue;

                    bool dup = false;
                    for (const auto& x : r.subs)
                        if (x.url == s.url) { dup = true; break; }
                    if (!dup) r.subs.push_back(s);
                }
            }

            // OpenSubtitles alone can answer with dozens of files for a popular
            // title. Past a point they are the same subtitle from different
            // rips, and a selector nobody can scroll is worse than a short one.
            constexpr size_t kMaxSubs = 40;

            // The preferred language first -- stable, so each language keeps the
            // order its addon sent (which is roughly best-match first).
            std::string want = config::preferredSubLang();
            std::string wantLabel = config::langLabelFor(want);
            std::stable_sort(r.subs.begin(), r.subs.end(),
                             [&](const Subtitle& a, const Subtitle& b) {
                                 bool pa = config::langLabelFor(a.lang) == wantLabel;
                                 bool pb = config::langLabelFor(b.lang) == wantLabel;
                                 return pa && !pb;
                             });
            if (r.subs.size() > kMaxSubs) r.subs.resize(kMaxSubs);

            brls::Logger::info("[stremio] subtitles {} -> {} ({} addons)", videoId,
                               r.subs.size(), list->size());
            brls::sync([done, r]() { done(r); });
        });
    });
}

void downloadSubtitleAsync(const Subtitle& sub,
                           std::function<void(std::string)> done)
{
    std::string path = subtitleCachePath(sub);
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        std::fclose(f);
        done(path);
        return;
    }

    std::string url = sub.url;
    brls::async([url, path, done]() {
        std::string err;
        bool ok = http::download(url, path, err);
        if (!ok)
            brls::Logger::warning("[stremio] subtitle download failed: {}", err);
        std::string out = ok ? path : std::string();
        brls::sync([done, out]() { done(out); });
    });
}

// {base}/catalog/{type}/{id}[/{extras}].json. Stremio puts the extras in a path
// segment of their own, spelled "k=v&k=v": the VALUES are percent-encoded, the
// separators are not -- which is why this is built by hand rather than by
// running the whole segment through urlEncode.
static std::string catalogUrl(const std::string& addonBase,
                              const std::string& type,
                              const std::string& catalogId,
                              const CatalogQuery& q)
{
    std::string url = addonBase + "/catalog/" + http::urlEncode(type) + "/" +
                      http::urlEncode(catalogId);
    std::string extra;
    if (!q.genre.empty()) extra = "genre=" + http::urlEncode(q.genre);
    if (q.skip > 0)
    {
        if (!extra.empty()) extra += "&";
        extra += "skip=" + std::to_string(q.skip);
    }
    if (!extra.empty()) url += "/" + extra;
    return url + ".json";
}

// Manifests are static for the life of a session and a genre list is a few
// hundred bytes, so one read per (addon, type, catalog) is plenty -- reopening
// a section must not go back to the network for a list that cannot have
// changed. UI thread only, like addonCache.
static std::map<std::string, std::vector<std::string>> genreCache;

void fetchCatalogGenresAsync(const std::string& addonBase,
                             const std::string& type,
                             const std::string& catalogId,
                             std::function<void(std::vector<std::string>)> done)
{
    std::string key = addonBase + "|" + type + "|" + catalogId;
    auto hit        = genreCache.find(key);
    if (hit != genreCache.end())
    {
        done(hit->second);
        return;
    }

    brls::async([addonBase, type, catalogId, key, done]() {
        std::vector<std::string> genres;
        std::string resp, err;
        if (!http::get(addonBase + "/manifest.json", resp, err))
        {
            brls::Logger::warning("[stremio] manifest {} failed: {}", addonBase,
                                  err);
        }
        else
        {
            for (const auto& cat : json::objects(resp, "catalogs"))
            {
                if (json::str(cat, "type") != type) continue;
                if (json::str(cat, "id") != catalogId) continue;
                for (const auto& ex : json::objects(cat, "extra"))
                    if (json::str(ex, "name") == "genre")
                        genres = json::strings(ex, "options");
                // Manifests written before "extra" existed list them straight
                // on the catalog instead.
                if (genres.empty()) genres = json::strings(cat, "genres");
                break;
            }
            brls::Logger::info("[stremio] genres {}/{} -> {}", type, catalogId,
                               genres.size());
        }
        // Cached even when empty: a catalog with no genres must not be asked
        // again every time its section is opened.
        brls::sync([key, genres, done]() {
            genreCache[key] = genres;
            done(genres);
        });
    });
}

void fetchCatalogAsync(const std::string& addonBase, const std::string& type,
                       const std::string& catalogId,
                       std::function<void(LibraryResult)> done)
{
    fetchCatalogAsync(addonBase, type, catalogId, CatalogQuery(), done);
}

void fetchCatalogAsync(const std::string& addonBase, const std::string& type,
                       const std::string& catalogId, const CatalogQuery& query,
                       std::function<void(LibraryResult)> done)
{
    brls::async([addonBase, type, catalogId, query, done]() {
        LibraryResult r;
        std::string url = catalogUrl(addonBase, type, catalogId, query);
        std::string resp, err;
        if (!http::get(url, resp, err))
        {
            r.error = err;
        }
        else
        {
            r.ok = true;
            for (const auto& o : json::objects(resp, "metas"))
            {
                LibItem it;
                it.id     = json::str(o, "id");
                it.name   = json::str(o, "name");
                it.type   = json::str(o, "type");
                if (it.type.empty()) it.type = type;
                it.poster = json::str(o, "poster");
                // Cinemeta says the year in "releaseInfo" ("2016", "2013-2019");
                // other addons may only carry "year".
                it.year   = json::str(o, "releaseInfo");
                if (it.year.empty()) it.year = json::str(o, "year");
                if (it.id.empty() || it.name.empty()) continue;
                r.items.push_back(it);
            }
            brls::Logger::info("[stremio] catalog {}/{} -> {} items", type,
                               catalogId, r.items.size());
        }
        brls::sync([done, r]() { done(r); });
    });
}

void fetchSearchAsync(const std::string& query,
                      std::function<void(LibraryResult)> done)
{
    brls::async([query, done]() {
        LibraryResult r;
        r.ok         = true;
        const char* types[] = { "movie", "series" };
        for (const char* type : types)
        {
            std::string url = std::string("https://v3-cinemeta.strem.io/catalog/") +
                              type + "/top/search=" + http::urlEncode(query) + ".json";
            std::string resp, err;
            if (!http::get(url, resp, err)) continue;  // one type may fail; keep the other
            for (const auto& o : json::objects(resp, "metas"))
            {
                LibItem it;
                it.id     = json::str(o, "id");
                it.name   = json::str(o, "name");
                it.type   = json::str(o, "type");
                if (it.type.empty()) it.type = type;
                it.poster = json::str(o, "poster");
                it.year   = json::str(o, "releaseInfo");
                if (it.year.empty()) it.year = json::str(o, "year");
                if (it.id.empty() || it.name.empty()) continue;
                r.items.push_back(it);
            }
        }
        brls::Logger::info("[stremio] search \"{}\" -> {} items", query,
                           r.items.size());
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
    NVGcolor hintColor = theme::textDim();

    // ---- sign-in form ----------------------------------------------------
    // A card with two rows that look like the fields they stand for. It used to
    // be three identical 360px buttons in a column -- "Email", "Password",
    // "Sign in" -- where the first two open a keyboard and the third submits,
    // which the shapes did nothing to tell apart. What had been typed showed as
    // a bare line floating above them, and the password's state was announced
    // in the same label as the errors.
    loginBox = new brls::Box();
    loginBox->setAxis(brls::Axis::COLUMN);
    loginBox->setJustifyContent(brls::JustifyContent::CENTER);
    loginBox->setAlignItems(brls::AlignItems::CENTER);
    loginBox->setGrow(1.0f);
    loginBox->setPadding(0, 60, 0, 60);

    constexpr float kFormW = 480.0f;

    // The mark, drawn rather than loaded so it follows the accent (see
    // theme::drawStremioMark).
    class Mark : public brls::Box
    {
      public:
        void draw(NVGcontext* vg, float x, float y, float w, float h,
                  brls::Style style, brls::FrameContext* ctx) override
        {
            theme::drawStremioMark(vg, x, y, w);
        }
    };
    auto* mark = new Mark();
    mark->setDimensions(72.0f, 72.0f);
    mark->setMarginBottom(18.0f);
    loginBox->addView(mark);

    auto* title = new brls::Label();
    title->setText(tr("Sign in to Stremio"));
    title->setFontSize(30.0f);
    title->setTextColor(theme::text());
    loginBox->addView(title);

    auto* hint = new brls::Label();
    hint->setText(tr("Your library, your addons and their sources."));
    hint->setFontSize(18.0f);
    hint->setTextColor(theme::textMuted());
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    hint->setMargins(8.0f, 0.0f, 26.0f, 0.0f);
    loginBox->addView(hint);

    auto* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setWidth(kFormW);
    card->setMarginBottom(22.0f);

    card->addView(loginField(tr("EMAIL"), tr("Not entered"),
                             [this]() { promptEmail(); }, &emailLabel));
    card->addView(loginField(tr("PASSWORD"), tr("Not entered"),
                             [this]() { promptPassword(); }, &passLabel));
    loginBox->addView(card);

    loginBtn = new brls::Button();
    loginBtn->setStyle(&brls::BUTTONSTYLE_PRIMARY);
    loginBtn->setText(tr("Sign in"));
    loginBtn->setWidth(kFormW);
    loginBtn->registerClickAction([this](brls::View*) { doLogin(); return true; });
    loginBox->addView(loginBtn);

    // The other way in: approve the console from a phone that is already signed
    // in, instead of typing a password on the on-screen keyboard.
    auto* qrBtn = new brls::Button();
    qrBtn->setStyle(&brls::BUTTONSTYLE_BORDERED);
    qrBtn->setText(tr("Sign in with a QR code"));
    qrBtn->setWidth(kFormW);
    qrBtn->setMarginTop(12.0f);
    qrBtn->registerClickAction([this](brls::View*) { promptQrLogin(); return true; });
    loginBox->addView(qrBtn);

    statusLabel = new brls::Label();
    statusLabel->setText("");
    statusLabel->setFontSize(17.0f);
    statusLabel->setTextColor(theme::textMuted());
    statusLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    // Fixed height: without it the column re-centres every time this text
    // changes, so the whole form jumped around on a failed sign-in.
    statusLabel->setHeight(28.0f);
    statusLabel->setMargins(16.0f, 0.0f, 0.0f, 0.0f);
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
    // No top padding: the item count that used to sit here moved to the header,
    // so the list starts right under it.
    libraryBox->setPadding(0.0f, 0.0f, 0.0f, 60.0f);
    libraryBox->setVisibility(brls::Visibility::GONE);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    // CENTERED, not the default NATURAL: NATURAL free-scrolls by the pixel, so
    // holding UP glides past rows with no per-row stop and never cleanly hands
    // focus to the header tab bar, and coming back down lands on whatever row is
    // topmost on screen (the 4th, after scrolling) instead of the first.
    // CENTERED moves focus one row at a time, scrolling to keep it in view.
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    libScroll = scroll;
    libList = new brls::Box();
    libList->setAxis(brls::Axis::COLUMN);
    // No margin here: ScrollingFrame::setContentView() detaches the content view
    // and forces setWidth(frameWidth), so anything set on this box is ignored.
    // The inset has to live on the rows themselves (see showItems).
    scroll->setContentView(libList);
    libraryBox->addView(scroll);

    // Centered loading/status overlay over the list: a message, and under it an
    // indeterminate bar (a segment sliding back and forth, animated in draw())
    // shown only while loading. Absolute + full-size so it centres on screen and
    // does not push the list.
    loadingBox = new brls::Box();
    loadingBox->setAxis(brls::Axis::COLUMN);
    loadingBox->setPositionType(brls::PositionType::ABSOLUTE);
    loadingBox->setPositionTop(0.0f);
    loadingBox->setPositionLeft(0.0f);
    loadingBox->setWidthPercentage(100.0f);
    loadingBox->setHeightPercentage(100.0f);
    loadingBox->setJustifyContent(brls::JustifyContent::CENTER);
    loadingBox->setAlignItems(brls::AlignItems::CENTER);
    loadingBox->setVisibility(brls::Visibility::GONE);

    libStatus = new brls::Label();
    libStatus->setText("");
    libStatus->setFontSize(22);
    libStatus->setTextColor(hintColor);
    libStatus->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    loadingBox->addView(libStatus);

    loadingBar = new brls::Box();
    loadingBar->setWidth(300.0f);
    loadingBar->setHeight(5.0f);
    loadingBar->setCornerRadius(2.5f);
    loadingBar->setMarginTop(22.0f);
    loadingBar->setClipsToBounds(true);   // keep the sliding fill inside the track
    loadingBar->setBackgroundColor(theme::scrim(35));
    loadingFill = new brls::Box();
    loadingFill->setWidth(90.0f);
    loadingFill->setHeight(5.0f);
    loadingFill->setCornerRadius(2.5f);
    loadingFill->setBackgroundColor(
        brls::Application::getTheme().getColor("brls/accent"));
    loadingBar->addView(loadingFill);
    loadingBox->addView(loadingBar);
    libraryBox->addView(loadingBox);

    this->addView(libraryBox);

    focusSub    = brls::Application::getGlobalFocusChangeEvent()->subscribe(
        [this](brls::View* v) { onGlobalFocus(v); });
    focusSubbed = true;

    // Y reloads the library on demand -- fires only while focus is on this tab.
    this->registerAction(
        tr("Reload"), brls::BUTTON_Y,
        [this](brls::View*) {
            if (!authKey.empty()) loadLibrary();
            return true;
        },
        false, false, brls::SOUND_NONE);

    // Left/Right (d-pad and the analog stick, which borealis maps to them) also
    // cycle the view -- but on the tab, not the frame: the header tab bar uses
    // Left/Right to move between Local and Stremio, and a list has no horizontal
    // neighbours so the directions are otherwise unused here. Hidden hints (R/L
    // already advertise "View").
    this->registerAction(
        tr("View"), brls::BUTTON_RIGHT,
        [this](brls::View*) {
            // The poster style lays the titles out horizontally, so Left/Right
            // belong to it entirely -- never consumed here, whether or not there
            // is a card that way (at the end of the strip the cursor should say
            // so, not jump to another category). L/R still cycle the views.
            if (posterStyle()) return false;
            // Two columns on screen (Search's Movies | Shows, or Popular next to
            // Featured): Left/Right moves between them when there IS a neighbour
            // that way (return false = not consumed, so borealis runs its focus
            // navigation); at the outer edge -- or on the search bar, which has no
            // neighbour -- it falls through to cycling the category instead.
            if (columnsShown)
            {
                brls::View* cur = brls::Application::getCurrentFocus();
                if (cur && cur->getNextFocus(brls::FocusDirection::RIGHT, cur))
                    return false;
            }
            if (!authKey.empty() && libLoaded) cycleView(+1);
            return true;
        },
        true, false, brls::SOUND_NONE);
    this->registerAction(
        tr("View"), brls::BUTTON_LEFT,
        [this](brls::View*) {
            if (posterStyle()) return false;  // see the RIGHT action above
            if (columnsShown)
            {
                brls::View* cur = brls::Application::getCurrentFocus();
                if (cur && cur->getNextFocus(brls::FocusDirection::LEFT, cur))
                    return false;
            }
            if (!authKey.empty() && libLoaded) cycleView(-1);
            return true;
        },
        true, false, brls::SOUND_NONE);

    // R/L cycle Continue Watching -> Popular Movies -> Popular Shows -> Library.
    // Registered on the frame (main.cpp) via this cycler, not on the tab, so R/L
    // also work while focus is on the header tab bar (outside this view tree).
    stremio::setViewCycler([this](int dir) {
        if (!authKey.empty() && libLoaded) cycleView(dir);
    });

    // A header view-bar button jumps straight to that view. Same guard as the
    // cycler: no-op until the library is up.
    stremio::setViewSelector([this](int idx) {
        if (!authKey.empty() && libLoaded)
            selectView(static_cast<View>(idx));
    });
    // Hidden until we render a view: a fresh tab shows the sign-in form, and the
    // bar has no place on it.
    stremio::reportView(-1);

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
    stremio::setViewCycler(nullptr);   // no live tab for the frame's R/L to reach
    stremio::setViewSelector(nullptr);
    stremio::reportView(-1);           // fold the header bar away with the tab
    if (focusSubbed)
        brls::Application::getGlobalFocusChangeEvent()->unsubscribe(focusSub);
}

void StremioTab::onGlobalFocus(brls::View* focused)
{
    // Focus landing back anywhere in OUR activity after something marked the
    // library stale: playback pushing new progress, or Options changing the list
    // style. Reload once so what is on screen is current. Tracking the
    // generation we last rendered (rather than a shared flag) means the reload
    // -- which re-fires focus events -- does not loop, and a deeper list
    // consuming the signal does not rob us of it.
    //
    // Our activity, not just our list: coming back from Options lands the cursor
    // on the header gear, which is outside the list but still on this screen --
    // waiting for the list itself to be focused meant the new style only
    // appeared after a manual reload. The test is the root of the tree we are
    // in: an activity stacked on top of us has a tree of its own, and rebuilding
    // the rows under one would free views its focus stack still points at.
    if (!libList || !focused) return;
    brls::View* root = this;
    while (root->getParent()) root = root->getParent();
    if (!isUnder(focused, root)) return;
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

brls::Box* StremioTab::loginField(const char* caption, const char* placeholder,
                                  std::function<void()> onPress,
                                  brls::Label** value)
{
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::COLUMN);
    row->setPadding(12.0f, 20.0f, 14.0f, 20.0f);
    row->setMarginBottom(10.0f);
    row->setCornerRadius(8.0f);
    row->setBackgroundColor(theme::surfaceSunken());
    row->setFocusable(true);
    row->setHighlightCornerRadius(8.0f);
    row->registerClickAction([onPress](brls::View*) {
        onPress();
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    auto* cap = new brls::Label();
    cap->setText(caption);
    cap->setFontSize(14.0f);
    cap->setTextColor(theme::textMuted());
    row->addView(cap);

    auto* val = new brls::Label();
    val->setText(placeholder);
    val->setFontSize(21.0f);
    // Faint until there is something in it: an empty field and a filled one
    // have to be distinguishable at a glance, which is the whole point of
    // showing the value in the row instead of above the form.
    val->setTextColor(theme::textFaint());
    val->setSingleLine(true);
    val->setMarginTop(2.0f);
    row->addView(val);

    *value = val;
    return row;
}

void StremioTab::promptEmail()
{
    brls::Application::getImeManager()->openForText(
        [this, live = alive](std::string out) {
            if (!*live) return;
            email = out;
            emailLabel->setText(email.empty() ? tr("Not entered") : email);
            emailLabel->setTextColor(email.empty() ? theme::textFaint()
                                                   : theme::text());
        },
        tr("Stremio email"), "", 128, email);
}

void StremioTab::promptPassword()
{
    brls::Application::getImeManager()->openForText(
        [this, live = alive](std::string out) {
            if (!*live) return;
            password = out;
            // Never echoed: the row shows one bullet per character, capped so a
            // long password does not report its own length across the card.
            size_t n = password.size() > 16 ? 16 : password.size();
            std::string dots;
            for (size_t i = 0; i < n; i++) dots += "\xE2\x80\xA2";
            passLabel->setText(password.empty() ? tr("Not entered") : dots);
            passLabel->setTextColor(password.empty() ? theme::textFaint()
                                                     : theme::text());
        },
        tr("Stremio password"), "", 128, "");
}

void StremioTab::doLogin()
{
    if (email.empty() || password.empty())
    {
        dialog(tr("Enter an email and a password."));
        return;
    }

    statusLabel->setText(tr("Signing in..."));
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
            dialog(tr("Sign-in failed: ") + r.error);
    });
}

namespace
{

// The QR dialog's content. The dialog deletes it however it goes away -- B,
// Cancel, or closed on success -- so its destructor is the one reliable "the
// dialog is gone" signal, and every pending request or timer checks `open`
// before touching the views below.
class QrPanel : public brls::Box
{
  public:
    std::shared_ptr<bool> open = std::make_shared<bool>(true);
    ~QrPanel() override { *open = false; }
};

struct QrSession
{
    std::shared_ptr<bool> open;
    brls::Dialog* dialog = nullptr;
    brls::Image* qr      = nullptr;
    brls::Label* code    = nullptr;
    brls::Label* status  = nullptr;
    std::string current;  // the code on screen
    std::chrono::steady_clock::time_point born;
    std::function<void(std::string)> onKey;  // approved: the account's authKey
};

// How often to ask whether the phone has approved the code, and when to swap it
// for a fresh one. The service answers "invalid or expired" both while a code
// waits and once it has expired, so its age is the only way to know. A code
// lives about 5 minutes (measured 2026-11-18: valid at 300s, expired by 315s);
// renewing at 4 leaves a minute's margin for a scan already under way.
constexpr long kQrPollMs      = 3000;
constexpr int  kQrLifetimeSec = 240;

void qrPoll(std::shared_ptr<QrSession> s);

void qrRequest(std::shared_ptr<QrSession> s)
{
    s->status->setText(tr("Getting a code..."));
    stremio::createLinkAsync([s](stremio::LinkCode c) {
        if (!*s->open) return;
        if (!c.ok)
        {
            // Offline, or the service is down: say so and try again shortly,
            // rather than leaving a dead QR up.
            s->status->setText(tr("Could not get a code: ") + c.error);
            brls::delay(5000, [s]() {
                if (*s->open) qrRequest(s);
            });
            return;
        }
        s->current = c.code;
        s->born    = std::chrono::steady_clock::now();
        s->qr->setImageFromMem((const unsigned char*)c.qrPng.data(),
                               (int)c.qrPng.size());
        s->code->setText(c.code);
        s->status->setText(tr("Waiting for your phone..."));
        brls::delay(kQrPollMs, [s]() { qrPoll(s); });
    });
}

void qrPoll(std::shared_ptr<QrSession> s)
{
    if (!*s->open) return;
    auto age = std::chrono::steady_clock::now() - s->born;
    if (age > std::chrono::seconds(kQrLifetimeSec))
    {
        qrRequest(s);
        return;
    }
    std::string code = s->current;
    stremio::readLinkAsync(code, [s, code](stremio::LoginResult r) {
        // A reply for a code we have since replaced is stale either way.
        if (!*s->open || code != s->current) return;
        if (r.ok)
        {
            auto cb  = s->onKey;
            auto key = r.authKey;
            s->dialog->close([cb, key]() { cb(key); });
            return;
        }
        brls::delay(kQrPollMs, [s]() { qrPoll(s); });
    });
}

} // namespace

void StremioTab::promptQrLogin()
{
    auto* panel = new QrPanel();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setAlignItems(brls::AlignItems::CENTER);
    panel->setPadding(26.0f, 50.0f, 22.0f, 50.0f);
    // A modal needs a focus target; this one is not meant to look focused.
    panel->setFocusable(true);
    panel->setHideHighlight(true);

    auto* title = new brls::Label();
    title->setText(tr("Scan with a phone signed in to Stremio"));
    title->setFontSize(22.0f);
    title->setTextColor(theme::text());
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    panel->addView(title);

    // NEAREST: the QR is a grid of hard-edged modules, and smoothing them while
    // scaling 396px down blurs the edges a phone camera locks onto.
    auto* qr = new brls::Image();
    qr->setDimensions(250.0f, 250.0f);
    qr->setScalingType(brls::ImageScalingType::FIT);
    qr->setInterpolation(brls::ImageInterpolation::NEAREST);
    qr->setMargins(18.0f, 0.0f, 14.0f, 0.0f);
    panel->addView(qr);

    auto* how = new brls::Label();
    how->setText(tr("Or open link.stremio.com and enter this code:"));
    how->setFontSize(17.0f);
    how->setTextColor(theme::textMuted());
    how->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    panel->addView(how);

    auto* code = new brls::Label();
    code->setText("...");
    code->setFontSize(34.0f);
    code->setTextColor(theme::text());
    code->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    code->setMarginTop(4.0f);
    panel->addView(code);

    auto* status = new brls::Label();
    status->setText("");
    status->setFontSize(16.0f);
    status->setTextColor(theme::textMuted());
    status->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    status->setHeight(24.0f);  // fixed, so a status change does not reflow
    status->setMarginTop(8.0f);
    panel->addView(status);

    auto* d = new brls::Dialog(panel);
    d->addButton(tr("Cancel"), []() {});
    d->open();

    auto s    = std::make_shared<QrSession>();
    s->open   = panel->open;
    s->dialog = d;
    s->qr     = qr;
    s->code   = code;
    s->status = status;
    s->onKey  = [this, live = alive](std::string key) {
        if (!*live) return;
        onAuthenticated(key, true);
        // Options shows who is signed in; nobody typed an email this time.
        stremio::fetchEmailAsync(key, [](std::string email) {
            if (!email.empty()) stremio::saveEmail(email);
        });
    };
    qrRequest(s);
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
    // until there are rows to move it to; showItems takes it from there.
    libraryBox->setFocusable(true);
    libraryBox->setHideHighlight(true);
    brls::Application::giveFocus(libraryBox);

    if (announce)
        dialog(tr("Signed in"));

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

// Centered message; with `loading`, the indeterminate bar under it too.
void StremioTab::showStatus(const std::string& msg, bool loading)
{
    libStatus->setText(msg);
    loadingBar->setVisibility(loading ? brls::Visibility::VISIBLE
                                      : brls::Visibility::GONE);
    loadingBox->setVisibility(brls::Visibility::VISIBLE);
}

// Slides the loading segment left<->right while the bar is shown. Time-based, so
// it is smooth regardless of frame rate; a no-op when the bar is hidden.
void StremioTab::draw(NVGcontext* vg, float x, float y, float width, float height,
                      brls::Style style, brls::FrameContext* ctx)
{
    double t = std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();

    // Analog stick flick cycles the view too (Left/Right buttons are actions,
    // but the stick is an axis borealis does not turn into button presses here).
    // Only while focus is in the list, one cycle per flick, and deferred to a
    // safe point (rebuilding the tree mid-draw crashes).
    if (libLoaded && !authKey.empty())
    {
        brls::View* f = brls::Application::getCurrentFocus();
        if (f && libList && isUnder(f, libList))
        {
            brls::ControllerState cs {};
            brls::Application::getPlatform()
                ->getInputManager()
                ->updateUnifiedControllerState(&cs);
            // Either stick flicks the view -- take whichever is pushed further,
            // so the right Joy-Con's stick works like the left one.
            float lx = cs.axes[brls::ControllerAxis::LEFT_X];
            float rx = cs.axes[brls::ControllerAxis::RIGHT_X];
            float sx = (lx < 0 ? -lx : lx) >= (rx < 0 ? -rx : rx) ? lx : rx;
            if (!stickLatched && (sx > 0.6f || sx < -0.6f))
            {
                stickLatched = true;
                int dir = sx > 0 ? 1 : -1;
                auto live = alive;
                brls::sync([this, live, dir]() {
                    if (!*live) return;
                    // Nothing to do in the poster style: Left/Right are no
                    // longer consumed there, so the push borealis turns into a
                    // navigation event already walks the strip -- moving focus
                    // here as well stepped two cards at a time.
                    if (posterStyle()) return;
                    // Same rule as the Left/Right actions when two columns are
                    // on screen (Popular | Featured, Search's two types): the
                    // flick belongs to the list while there is a column that
                    // way -- and borealis' own navigation is what moves it --
                    // and only cycles the category at the outer edge.
                    if (columnsShown)
                    {
                        brls::View* cur = brls::Application::getCurrentFocus();
                        if (cur &&
                            cur->getNextFocus(dir > 0 ? brls::FocusDirection::RIGHT
                                                      : brls::FocusDirection::LEFT,
                                              cur))
                            return;
                    }
                    if (libLoaded && !authKey.empty()) cycleView(dir);
                });
            }
            else if (sx > -0.3f && sx < 0.3f)
                stickLatched = false;
        }
        else
            stickLatched = false;
    }

    if (loadingBox && loadingBar && loadingFill &&
        loadingBox->getVisibility() == brls::Visibility::VISIBLE &&
        loadingBar->getVisibility() == brls::Visibility::VISIBLE)
    {
        const float track = 300.0f, seg = 90.0f;
        float phase = (float)((std::sin(t * 3.0) + 1.0) * 0.5);  // 0..1 ease
        loadingFill->setTranslationX(phase * (track - seg));
    }

    // View-change slide: ease the list from an off-centre start back to x=0.
    if (sliding && libList)
    {
        const double dur   = 0.22;
        const float  slide = 520.0f;  // how far off it starts
        double el = t - slideStart;
        if (el >= dur)
        {
            sliding = false;
            libList->setTranslationX(0.0f);
        }
        else
        {
            double p     = el / dur;
            double eased = 1.0 - std::pow(1.0 - p, 3.0);  // ease-out cubic
            libList->setTranslationX((float)((1.0 - eased) * slideSign * slide));
        }
    }

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void StremioTab::loadLibrary()
{
    showStatus(tr("Loading library..."), true);
    stremio::setLibraryCount("");  // header back to a plain title while loading
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
            showStatus(tr("Error"), false);
            dialog(tr("Library unavailable: ") + r.error);
            return;
        }
        libItems  = r.items;   // cached for Continue Watching + Library views
        libLoaded = true;
        renderView();          // lands on the default view (Continue Watching)
    });
}

// R (dir +1) / L (dir -1) cycle the view; wraps around.
void StremioTab::cycleView(int dir)
{
    int n = static_cast<int>(View::COUNT);
    view  = static_cast<View>(((static_cast<int>(view) + dir) % n + n) % n);
    resetOnShow  = true;              // land on the first row, scrolled to the top
    pendingSlide = dir > 0 ? 1 : -1;  // slide the new list in from that side
    renderView();
}

// A header tab-bar pick: jump straight to `v`. Like cycleView, and it hands the
// cursor to the first item of the new view rather than leaving it on the button
// -- picking a category is a step INTO it, and having to press Down afterwards
// made the header feel like a dead end. The slide follows the index delta.
void StremioTab::selectView(View v)
{
    if (v == view) return;
    pendingSlide = static_cast<int>(v) > static_cast<int>(view) ? 1 : -1;
    view = v;
    resetOnShow = true;
    renderView();
}

// Builds the list for the current view. Continue Watching and Library come from
// the cached library; the two Popular views pull Cinemeta's top catalog once and
// cache it, showing a loading line in between.
void StremioTab::renderView()
{
    stremio::reportView(static_cast<int>(view));  // light up the header tab bar
    // The list's left inset belongs to the rows in every style but the poster
    // strip, which runs to the screen edge instead: with the inset, a card being
    // scrolled out was cut short of the edge with a band of background beside
    // it, which reads as clipped rather than as sliding away. The strip carries
    // that inset on its first card, and Search on its bar and its headings.
    libraryBox->setPaddingLeft(posterStyle() ? 0.0f : kPosterInset);
    // No scroll indicator in the poster style: a page of strips is meant to be
    // read across, and the bar pinned to the right edge sat over the artwork.
    if (libScroll) libScroll->setScrollingIndicatorVisible(!posterStyle());
    switch (view)
    {
        case View::ContinueWatching:
        {
            // Library items with playback started but not finished, newest first
            // (the library is already sorted by mtime desc). Not every one is in
            // the library proper -- Stremio auto-adds watched titles as temp
            // entries, which is exactly Continue Watching.
            // The just-played position updates locally the instant playback ends,
            // before the server round-trip. Overlay it so a finished episode shows
            // as advanced to the next one here and now, rather than waiting for --
            // or racing -- the library refetch. The server catches up on reload.
            stremio::LocalWatch lw = stremio::lastWatch();
            std::vector<stremio::LibItem> cw;
            for (auto it : libItems)  // by value: overlaid below
            {
                if (lw.itemId == it.id && !lw.videoId.empty())
                {
                    it.videoId      = lw.videoId;
                    it.timeOffsetMs = lw.offsetMs;
                    it.durationMs   = lw.durationMs;
                }
                double p           = it.progress();
                bool   inProgress  = p > 0.005 && p < 0.95;
                // A watched series stays in Continue Watching even once the current
                // episode is finished (>=95%) or already advanced to the next at
                // offset 0 (p < 0): Stremio shows it on the next episode. We keep
                // any series with a watched episode -- opening it lands on the right
                // episode (buildEpisodeCards advances past a finished one). Movies
                // still drop when finished.
                bool watchedSeries = it.type == "series" && !it.videoId.empty();
                if (inProgress || watchedSeries) cw.push_back(it);
            }
            //  history -- Material glyph in the header title.
            showItems(cw, std::string("  ") + tr("Continue Watching"),
                      tr("Nothing in progress"));
            break;
        }
        case View::Library:
        {
            // The grid is the explicit library only -- drop the removed entries we
            // now keep for Continue Watching.
            std::vector<stremio::LibItem> lib;
            for (const auto& it : libItems)
                if (!it.removed) lib.push_back(it);
            showItems(lib,
                      std::string("  ") + tr("Library") + " \xC2\xB7 " +
                          std::to_string(lib.size()) + tr(" items"),
                      tr("Library is empty"));
            break;
        }
        case View::PopularMovies:  //  movie
            loadFeatured("movie");  // the second section, in every style
            if (popMoviesLoaded)
                showItems(popMovies, std::string("  ") + tr("Popular Movies"),
                          tr("No popular movies"));
            else
                loadCatalog("movie", popMovies, popMoviesLoaded,
                            std::string("  ") + tr("Popular Movies"));
            break;
        case View::PopularSeries:  //  tv (E02C renders as a TV here)
            loadFeatured("series");
            if (popSeriesLoaded)
                showItems(popSeries, std::string("  ") + tr("Popular Shows"),
                          tr("No popular shows"));
            else
                loadCatalog("series", popSeries, popSeriesLoaded,
                            std::string("  ") + tr("Popular Shows"));
            break;
        case View::Search:
            renderSearch();
            break;
        default:
            break;
    }
}

// The Search view: a focusable search bar at the top (A opens the keyboard),
// then the results below it. The bar stays so you can search again.
void StremioTab::renderSearch()
{
    parkFocusOffList();
    *rowsAlive = false;
    rowsAlive  = std::make_shared<bool>(true);
    libList->clearViews();
    loadingBox->setVisibility(brls::Visibility::GONE);
    stremio::setLibraryCount(std::string("  ") + tr("Search") +
                             (searchQuery.empty()
                                  ? ""
                                  : " \xC2\xB7 " + searchQuery));

    // In the poster style the list gives up its screen inset so the strips can
    // run to the edge (see renderView), so everything else on this page carries
    // it instead.
    const float inset = posterStyle() ? kPosterInset : 0.0f;

    // The search bar cell.
    auto* bar = new brls::Box();
    bar->setAxis(brls::Axis::ROW);
    bar->setAlignItems(brls::AlignItems::CENTER);
    bar->setHeight(72.0f);
    bar->setPaddingLeft(20.0f);
    bar->setPaddingRight(24.0f);
    bar->setMarginLeft(inset);
    bar->setMarginRight(40.0f + inset);
    bar->setCornerRadius(6.0f);
    bar->setBackgroundColor(theme::scrim(20));
    bar->setFocusable(true);
    bar->registerClickAction([this](brls::View*) { promptSearch(); return true; });
    bar->addGestureRecognizer(new brls::TapGestureRecognizer(bar));
    auto* icon = new brls::Label();
    icon->setText("");  // Material "search" glyph (borealis fallback font)
    icon->setFontSize(34.0f);
    // The Material glyph sits high in its box; nudge it down to sit level with
    // the placeholder text.
    icon->setMargins(10.0f, 16.0f, 0.0f, 0.0f);
    bar->addView(icon);
    auto* barText = new brls::Label();
    barText->setText(searchQuery.empty() ? tr("Search movies & shows...")
                                         : searchQuery);
    barText->setFontSize(24.0f);
    barText->setTextColor(searchQuery.empty() ? theme::textMuted()
                                              : theme::text());
    barText->setSingleLine(true);
    bar->addView(barText);
    // Gap before the first result (finishList overrides this to the bottom inset
    // when the bar is the last row, i.e. before any search).
    bar->setMarginBottom(18.0f);
    libList->addView(bar);

    brls::View* lastRow = bar;
    columnsShown        = false;  // set below, only by the two-column layout
    if (!searchQuery.empty())
    {
        if (!searchLoaded || searchResults.empty())
        {
            auto* l = new brls::Label();
            l->setText(!searchLoaded
                           ? tr("Searching...")
                           : tr("No results for \"") + searchQuery + "\"");
            l->setFontSize(20.0f);
            l->setTextColor(theme::textMuted());
            l->setMargins(24.0f, 0.0f, 8.0f, 20.0f + inset);
            libList->addView(l);
            lastRow = l;
        }
        else if (posterStyle())
        {
            // One strip per type, stacked: films first, then shows, and a type
            // with no hits gets no section at all rather than an empty strip.
            auto section = [&](const char* title, bool series) {
                std::vector<stremio::LibItem> sel;
                for (const auto& it : searchResults)
                    if ((it.type == "series") == series) sel.push_back(it);
                if (!sel.empty()) lastRow = addStripSection(title, sel, nullptr);
            };
            section(tr("Movies"), false);
            section(tr("Shows"), true);
        }
        else
        {
            // Split the results: movies on the left, shows on the right.
            columnsShown = true;
            auto* split  = new brls::Box();
            split->setAxis(brls::Axis::ROW);
            split->setAlignItems(brls::AlignItems::FLEX_START);

            auto makeCol = [](const char* title) {
                auto* col = new brls::Box();
                col->setAxis(brls::Axis::COLUMN);
                col->setWidthPercentage(50.0f);  // hard 50/50, not content-sized
                auto* h = new brls::Label();
                h->setText(title);
                h->setFontSize(20.0f);
                h->setTextColor(theme::textMuted());
                // 16, the rows' own left padding: the heading lines up with the
                // posters under it instead of sitting inside them.
                h->setMargins(0.0f, 0.0f, 10.0f, 16.0f);
                col->addView(h);
                return col;
            };
            auto* moviesCol = makeCol(tr("Movies"));
            auto* showsCol  = makeCol(tr("Shows"));

            int nMovies = 0, nShows = 0;
            for (const auto& it : searchResults)
            {
                bool isSeries = it.type == "series";
                (isSeries ? showsCol : moviesCol)->addView(buildItemRow(it, false));
                (isSeries ? nShows : nMovies)++;
            }
            auto emptyNote = [](brls::Box* col, int n) {
                if (n) return;
                auto* l = new brls::Label();
                l->setText(tr("None"));
                l->setFontSize(18.0f);
                l->setTextColor(theme::textFaint());
                l->setMarginLeft(20.0f);
                col->addView(l);
            };
            emptyNote(moviesCol, nMovies);
            emptyNote(showsCol, nShows);

            split->addView(moviesCol);
            split->addView(showsCol);
            libList->addView(split);
            lastRow = split;
        }
    }
    finishList(lastRow);
}

// Opens the on-screen keyboard, then runs the query.
void StremioTab::promptSearch()
{
    brls::Application::getImeManager()->openForText(
        [this, live = alive](std::string out) {
            if (!*live || out.empty()) return;
            searchQuery   = out;
            searchLoaded  = false;
            searchResults.clear();
            renderSearch();   // show the bar + "Searching..."
            stremio::fetchSearchAsync(
                out, [this, live, want = out](stremio::LibraryResult r) {
                    if (!*live || view != View::Search || searchQuery != want)
                        return;
                    searchResults = r.ok ? r.items : std::vector<stremio::LibItem>{};
                    searchLoaded  = true;
                    renderSearch();
                });
        },
        tr("Search Stremio"), "", 128, searchQuery);
}

// Fetches Cinemeta's "top" catalog for `type` into `cache`, then renders it if
// the view has not moved on since. Header shows a loading line meanwhile.
void StremioTab::loadCatalog(const char* type,
                             std::vector<stremio::LibItem>& cache, bool& loaded,
                             const std::string& header)
{
    parkFocusOffList();
    *rowsAlive = false;
    rowsAlive  = std::make_shared<bool>(true);
    libList->clearViews();
    showStatus(tr("Loading..."), true);
    stremio::setLibraryCount(header);

    View want = view;  // if R moves on before this lands, drop it
    std::string t = type;
    stremio::fetchCatalogAsync(
        "https://v3-cinemeta.strem.io", type, "top",
        [this, live = alive, want, t, &cache, &loaded, header](
            stremio::LibraryResult r) {
            if (!*live || view != want) return;
            if (!r.ok)
            {
                showStatus(tr("Error"), false);
                dialog(tr("This catalogue is unavailable: ") + r.error);
                return;
            }
            cache  = r.items;
            loaded = true;
            showItems(cache, header,
                      t == "series" ? tr("No popular shows")
                                    : tr("No popular movies"));
        });
}

// The heading over the current view's strip. Plain words, unlike the header
// line at the top of the screen, which carries a glyph and a count.
const char* StremioTab::sectionTitle() const
{
    switch (view)
    {
        case View::ContinueWatching: return tr("Continue watching");
        case View::PopularMovies:
        case View::PopularSeries:    return tr("Popular");
        case View::Library:          return tr("Library");
        default:                     return "";
    }
}

const std::vector<stremio::LibItem>* StremioTab::featuredCache()
{
    if (view == View::PopularMovies) return &featMovies;
    if (view == View::PopularSeries) return &featSeries;
    return nullptr;
}

// Cinemeta's second catalog ("year", its newest releases) for `type`, shown as
// the Featured strip. Fired alongside the popular one and folded in when it
// lands; a failure just means no second section.
void StremioTab::loadFeatured(const char* type)
{
    bool series   = std::string(type) == "series";
    auto& cache   = series ? featSeries : featMovies;
    bool& asked   = series ? featSeriesAsked : featMoviesAsked;
    if (asked) return;
    asked = true;

    View want = view;
    stremio::fetchCatalogAsync(
        "https://v3-cinemeta.strem.io", type, "year",
        [this, live = alive, want, &cache](stremio::LibraryResult r) {
            if (!*live || !r.ok || r.items.empty()) return;
            cache = r.items;
            // Only if the user is still on the view that asked for it: the
            // render folds the new section in.
            if (view == want) renderView();
        });
}

// A heading and the strip under it, both into libList. Returns the strip: the
// bottom inset finishList adds belongs on the last thing on the page.
brls::View* StremioTab::addStripSection(const std::string& title,
                                        const std::vector<stremio::LibItem>& items,
                                        brls::Box* seeMore)
{
    bool first = libList->getChildren().empty();
    if (!title.empty())
    {
        // Nothing under it: the strip carries its own slack for the cursor's
        // glow, and that slack is the gap.
        addHeading(title, first ? 0.0f : 22.0f, 0.0f, headingInset());
        first = false;  // ... the heading is on the page now
    }
    brls::View* strip =
        buildPosterStrip(items, libList->getChildren().size() <= 1, seeMore);
    libList->addView(strip);
    return strip;
}

// Whether the current view's items are a catalog we cap (see kSectionMax).
bool StremioTab::capped() const
{
    return view == View::PopularMovies || view == View::PopularSeries;
}

// The tile that ends a capped section, shaped like the items it follows so the
// row/strip keeps its rhythm: a poster-sized card in the poster style, a row in
// the others. Both open the whole section full-screen.
// The two Popular views are the only ones with a See More (see capped()), and
// each shows one type.
const char* StremioTab::catalogType() const
{
    return view == View::PopularSeries ? "series" : "movie";
}

brls::Box* StremioTab::buildSeeMoreCard(const std::string& title,
                                        std::vector<stremio::LibItem> all,
                                        std::string catType, std::string catId)
{
    auto* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setJustifyContent(brls::JustifyContent::CENTER);
    card->setAlignItems(brls::AlignItems::CENTER);
    card->setWidth(kPosterCardW);
    card->setHeight(kPosterCardH);
    card->setMargins(0.0f, 16.0f, 0.0f, 8.0f);
    card->setCornerRadius(kCardRadius);
    card->setBackgroundColor(theme::scrim(16));
    card->setFocusable(true);
    card->setHighlightCornerRadius(
        kCardRadius +
        brls::Application::getStyle()["brls/highlight/stroke_width"] / 2);
    card->registerClickAction([this, title, all, catType, catId](brls::View*) {
        openSection(title, all, catType, catId);
        return true;
    });
    card->addGestureRecognizer(new brls::TapGestureRecognizer(card));

    auto* icon = new brls::Label();
    icon->setText("");  // Material "chevron_right" (borealis fallback font)
    icon->setFontSize(52.0f);
    icon->setTextColor(theme::textDim());
    card->addView(icon);

    auto* label = new brls::Label();
    label->setText(tr("See More"));
    label->setFontSize(21.0f);
    label->setTextColor(theme::textDim());
    label->setMarginTop(6.0f);
    card->addView(label);
    return card;
}

brls::Box* StremioTab::buildSeeMoreRow(const std::string& title,
                                       std::vector<stremio::LibItem> all,
                                       std::string catType, std::string catId)
{
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setJustifyContent(brls::JustifyContent::CENTER);
    row->setHeight(72.0f);
    row->setMarginRight(40.0f);
    row->setMarginTop(6.0f);
    row->setCornerRadius(kCardRadius);
    row->setBackgroundColor(theme::scrim(16));
    row->setFocusable(true);
    row->setHighlightCornerRadius(
        kCardRadius +
        brls::Application::getStyle()["brls/highlight/stroke_width"] / 2);
    row->registerClickAction([this, title, all, catType, catId](brls::View*) {
        openSection(title, all, catType, catId);
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));

    auto* label = new brls::Label();
    label->setText(tr("See More"));
    label->setFontSize(21.0f);
    label->setTextColor(theme::textDim());
    row->addView(label);

    auto* icon = new brls::Label();
    icon->setText("");  // Material "chevron_right"
    icon->setFontSize(30.0f);
    icon->setTextColor(theme::textDim());
    icon->setMarginLeft(6.0f);
    row->addView(icon);
    return row;
}

// The whole of a section, full screen: title, a genre filter, then the items in
// the style the list is in -- a grid of cards under the poster style, the same
// rows under the others. Only the footer sits around it; the header would just
// repeat the title. Built here rather than by the activity because the card and
// row builders (and the account key they need) live on the tab.
//
// Unlike the strip it came from, this page is not a fixed set of items: it pages
// the catalog as you scroll (Stremio's "skip" extra) and re-queries it when the
// genre changes ("genre"). `items` is the first page, already in hand.
void StremioTab::openSection(std::string title,
                             std::vector<stremio::LibItem> items,
                             std::string catType, std::string catId)
{
    // Everything the page mutates. On the heap and shared, because it outlives
    // this function and belongs to the pushed page rather than to the tab --
    // which keeps its own state for the list behind it.
    struct State
    {
        std::vector<stremio::LibItem> items;
        std::set<std::string> ids;  // what is already on the page (see loadMore)
        std::string type, id;
        std::string genre;                   // "" = All
        std::vector<std::string> genresList;  // as the manifest spells them
        int nextSkip   = 0;     // what to ask the catalog for next
        bool loading   = false;
        bool exhausted = false;
        // Bumped on every genre change. A request carries the value it was
        // issued under and its answer is dropped if that no longer matches --
        // without it, switching All -> Drama -> Family fast lets Drama's reply
        // land after Family's clear and fill the page with the wrong genre.
        int gen = 0;
        // The line currently being filled, in the poster grid. Raw pointers
        // into the view tree, reset whenever the list is cleared.
        brls::Box* line = nullptr;
        int inLine      = 0;
        // Lifetime of the cards CURRENTLY in the grid, which is shorter than
        // the page's: a genre change frees every one of them while the page
        // lives on. Their poster fetches hold this, so retiring it is what
        // keeps a late download off a freed Image.
        std::shared_ptr<bool> gridAlive = std::make_shared<bool>(true);
    };
    // Retired by ~SectionActivity. Everything this page starts -- poster
    // downloads, catalog pages -- checks it, because B can pop the page while
    // they are in flight and they all hold raw pointers into its view tree.
    auto pageAlive = std::make_shared<bool>(true);

    auto st   = std::make_shared<State>();
    st->type  = catType;
    st->id    = catId;
    st->items = std::move(items);
    for (const auto& it : st->items) st->ids.insert(it.id);
    st->nextSkip = (int)st->items.size();

    auto* root = new GrowingSection();
    root->setAxis(brls::Axis::COLUMN);
    root->setGrow(1.0f);
    root->setPaddingTop(28.0f);

    // As many cards per line as the logical width takes -- which is not a
    // constant, it follows the UI-size setting and the dock state.
    //
    // A line is (kPosterInset - 8) of left inset, then N cards of 8 + 210 + 16.
    // The +16 below is the LAST card's own right margin, which already provides
    // part of the gap at the right edge: counting a full kPosterInset there on
    // top of it -- which "contentWidth - 2 * kPosterInset" did -- lost a whole
    // column at the 100% UI size (four cards and a card-wide hole on the right,
    // where five fit exactly).
    const float step     = kPosterCardW + 24.0f;
    const float lineLeft = kPosterInset - 8.0f;
    const float usable =
        brls::Application::contentWidth - lineLeft - kPosterInset + 16.0f;
    int perRow = (int)(usable / step);
    if (perRow < 1) perRow = 1;

    // The columns almost never divide the width exactly, and a card is 210 wide
    // whatever the screen -- so N columns at their natural spacing leave up to a
    // card's worth of nothing against the right edge (234px at the 89% UI size).
    // Spread that remainder across the gaps BETWEEN cards instead: the grid then
    // ends where the header does at every UI size, and the columns simply
    // breathe a little more on the wider ones.
    float gapExtra = 0.0f;
    if (perRow > 1)
    {
        float lastRight = lineLeft + perRow * step - 16.0f;
        float slack     = brls::Application::contentWidth - kPosterInset - lastRight;
        if (slack > 0.0f) gapExtra = slack / (perRow - 1);
    }

    // The page header: the title on the left, the filter pinned to the right.
    // One row, so the filter costs no vertical space of its own and the grid
    // starts higher.
    auto* head = new brls::Box();
    head->setAxis(brls::Axis::ROW);
    head->setAlignItems(brls::AlignItems::CENTER);
    // Left matches the grid's effective inset (the lines' kPosterInset - 8 plus
    // the cards' own 8); right matches where the grid now ends, which gapExtra
    // has just pinned to the same inset. The other styles have no grid: their
    // rows end at 40.
    head->setMargins(0.0f, posterStyle() ? kPosterInset : 40.0f, 0.0f,
                     kPosterInset);
    root->addView(head);

    // "Movies - Popular", "Shows - Featured". The strip this came from sat under
    // a view whose own name carried the type; this page stands alone, so it has
    // to say it. No item count: the page pages, so any number it showed would be
    // "how much has been scrolled so far", which is not worth a line.
    auto* h = new brls::Label();
    h->setText(std::string(catType == "series" ? tr("Shows") : tr("Movies")) + " - " +
               title);
    h->setFontSize(28.0f);
    h->setSingleLine(true);
    h->setGrow(1.0f);  // pushes the filter to the right edge
    head->addView(h);

    // Starts as the only option and grows once the addon's manifest is read.
    // Built now rather than after that read so the page appears immediately --
    // the manifest is one more request, and it is cached from then on. Fixed
    // width: a SelectorCell is a full-width list row by default, which is not
    // what it is being used as here.
    auto* genreCell = new brls::SelectorCell();
    genreCell->setWidth(340.0f);
    genreCell->setShrink(0.0f);
    // RecyclerCell's constructor gives every cell a 1px bottom separator, which
    // is right in a list and wrong here -- it drew a rule under the filter with
    // nothing below it to separate from.
    genreCell->setLineBottom(0.0f);
    head->addView(genreCell);

    // Shown while a query is out and the grid has nothing in it -- picking a
    // genre empties the page for as long as the round trip takes, and an empty
    // screen with no explanation reads as a bug.
    auto* busy = new brls::Label();
    busy->setText(tr("Loading..."));
    busy->setFontSize(20.0f);
    busy->setTextColor(theme::textMuted());
    busy->setMargins(24.0f, 0.0f, 0.0f, kPosterInset);
    busy->setVisibility(brls::Visibility::GONE);
    root->addView(busy);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    // CENTERED for the same reason the library list uses it: one step per press,
    // scrolled to keep the cursor in view.
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* list = new brls::Box();
    list->setAxis(brls::Axis::COLUMN);
    if (!posterStyle()) list->setPaddingLeft(kPosterInset);
    scroll->setContentView(list);
    root->addView(scroll);

    root->scroll = scroll;
    root->list   = list;

    // Adds st->items[from..] to the list, continuing the part-filled line the
    // last page left behind rather than starting a new one -- a page boundary
    // must not show up as a gap in the grid.
    auto appendFrom = [this, list, st, perRow, gapExtra, genreCell,
                       busy](size_t from) {
        // The cards built below belong to this grid, so their artwork fetches
        // must die with it -- not with the tab's list, which outlives the page,
        // and not with the page either, which outlives a genre change. See
        // attachPoster.
        artToken = st->gridAlive;
        for (size_t i = from; i < st->items.size(); i++)
        {
            // UP out of the top of the list has to be said explicitly: the
            // ScrollingFrame hands focus back to itself while it can still
            // scroll up, so traversal alone never reaches the genre row (the
            // same reason the library has setLibraryUpTarget).
            bool top = list->getChildren().empty();

            if (!posterStyle())
            {
                brls::Box* row = buildItemRow(st->items[i], false);
                if (top)
                    row->setCustomNavigationRoute(brls::FocusDirection::UP,
                                                  genreCell);
                list->addView(row);
                continue;
            }
            if (!st->line || st->inLine >= perRow)
            {
                st->line = new brls::Box();
                st->line->setAxis(brls::Axis::ROW);
                // The cards carry their own left margin.
                st->line->setMarginLeft(kPosterInset - 8.0f);
                st->line->setMarginBottom(16.0f);
                list->addView(st->line);
                st->inLine = 0;
            }
            brls::Box* card = buildPosterCard(st->items[i], false);
            // Everything but the last column carries a share of the slack, so
            // a part-filled final line still lines up with the ones above it.
            if (st->inLine < perRow - 1)
                card->setMarginRight(16.0f + gapExtra);
            // Every card of the first line, whichever page it came from.
            if (list->getChildren().size() == 1)
                card->setCustomNavigationRoute(brls::FocusDirection::UP,
                                               genreCell);
            st->line->addView(card);
            st->inLine++;
        }
        artToken = nullptr;
        if (!st->items.empty()) busy->setVisibility(brls::Visibility::GONE);
    };

    auto live = this->alive;

    // Pulls the next page. Called from the draw loop while the scroll sits near
    // the bottom, so the two flags are what stop it firing sixty times a second
    // -- and what stops it asking forever once the catalog runs out.
    auto loadMore = [live, pageAlive, st, appendFrom, busy]() {
        if (st->loading || st->exhausted) return;
        st->loading = true;
        if (st->items.empty())
        {
            busy->setText(tr("Loading..."));
            busy->setVisibility(brls::Visibility::VISIBLE);
        }

        stremio::CatalogQuery q;
        q.genre  = st->genre;
        q.skip   = st->nextSkip;
        int gen  = st->gen;
        stremio::fetchCatalogAsync(
            kCinemeta, st->type, st->id, q,
            [live, pageAlive, st, appendFrom, busy, gen](stremio::LibraryResult r) {
                if (!*live || !*pageAlive) return;
                // Answer to a question the page no longer asks.
                if (gen != st->gen) return;
                st->loading = false;
                busy->setVisibility(brls::Visibility::GONE);
                if (!r.ok || r.items.empty())
                {
                    st->exhausted = true;
                    // Nothing at all under this filter (or the addon refused).
                    // Leave the line up, saying which -- an empty page with no
                    // word on it is the same blank screen the busy label exists
                    // to avoid.
                    if (st->items.empty())
                    {
                        busy->setText(r.ok ? tr("Nothing here") : tr("Catalog unavailable"));
                        busy->setVisibility(brls::Visibility::VISIBLE);
                    }
                    return;
                }
                // Skip is counted in what the addon sent, not in what we kept:
                // an addon that pads the last page with items we already have
                // would otherwise make us ask for the same offset forever.
                st->nextSkip += (int)r.items.size();

                size_t from = st->items.size();
                for (const auto& it : r.items)
                    if (st->ids.insert(it.id).second) st->items.push_back(it);
                if (st->items.size() == from)
                {
                    st->exhausted = true;  // a whole page of things we had
                    return;
                }
                appendFrom(from);
            });
    };
    root->onNearBottom = loadMore;

    // Genre change: the page is a different query now, so it starts over.
    auto onGenre = [st, list, scroll, genreCell, loadMore](int sel) {
        // Index 0 is "All"; the rest are the manifest's list, offset by it.
        std::string want;
        if (sel > 0 && (size_t)(sel - 1) < st->genresList.size())
            want = st->genresList[(size_t)sel - 1];
        if (want == st->genre) return;
        st->genre = want;

        // clearViews() frees whatever it holds, including the focused view --
        // the same trap as the library list. Normally the cursor is up in the
        // dropdown that fired this, not in the grid, but park it if it is not.
        brls::View* cur = brls::Application::getCurrentFocus();
        if (cur && isUnder(cur, list)) brls::Application::giveFocus(genreCell);

        // The cards about to be freed each have a poster fetch that writes
        // into their Image when it lands. Retiring the grid's token is what
        // stops that writing into freed views. NOT the tab's rowsAlive: that
        // belongs to the list behind this page, which is not being touched.
        *st->gridAlive = false;
        st->gridAlive  = std::make_shared<bool>(true);
        list->clearViews();
        st->items.clear();
        st->ids.clear();
        st->line      = nullptr;
        st->inLine    = 0;
        st->nextSkip  = 0;
        st->exhausted = false;
        st->loading   = false;
        st->gen++;  // any reply still out belongs to the previous genre

        // The frame keeps the offset it was scrolled to, and the new query
        // starts empty -- leave it and the first page is drawn above the
        // viewport, which looks exactly like the list vanishing.
        scroll->setContentOffsetY(0.0f, false);

        loadMore();  // the first page of the new query
    };

    // Cinemeta's "year" catalog (the Featured strip) filters by YEAR through the
    // very same "genre" extra -- its options are 2026, 2025, ... So the row is
    // named after what it will actually hold, not after the Stremio prop.
    genreCell->init(catId == "year" ? tr("Year") : tr("Genre"), { tr("All") }, 0,
                    onGenre);

    stremio::fetchCatalogGenresAsync(
        kCinemeta, catType, catId,
        [live, pageAlive, st, genreCell](std::vector<std::string> g) {
            if (!*live || !*pageAlive) return;
            if (g.empty()) return;  // no genres: the row stays at All
            st->genresList = g;
            std::vector<std::string> labels{ tr("All") };
            for (const auto& x : g) labels.push_back(x);
            genreCell->setData(labels);
            genreCell->setSelection(0, true);  // silent: nothing has changed
        });

    appendFrom(0);
    brls::Application::pushActivity(
        new SectionActivity(root, [st, pageAlive]() {
            *pageAlive     = false;  // catalog pages, the genre list
            *st->gridAlive = false;  // the artwork of the cards on screen
        }));
}

// A section heading into libList. `left` is the inset the style needs -- see
// headingInset(); `bottom` is nothing over a poster strip, which carries slack
// of its own, and a real gap over rows, which start immediately.
void StremioTab::addHeading(const std::string& title, float top, float bottom,
                            float left)
{
    auto* h = new brls::Label();
    h->setText(title);
    h->setFontSize(20.0f);
    h->setTextColor(theme::textMuted());
    h->setMargins(top, 0.0f, bottom, left);
    libList->addView(h);
}

// Where a heading has to start to sit over the artwork rather than inside it.
// The three styles put their poster in three different places: the strips run
// to the screen edge and carry the inset themselves, a classic row keeps the
// shell's own left padding, and a card row has none at all -- the cursor hugs
// the card, so the poster starts at the list's edge.
float StremioTab::headingInset() const
{
    if (posterStyle()) return kPosterInset;
    return config::get().listStyle == kStyleClassic ? kRowInset : 0.0f;
}

void StremioTab::showItems(const std::vector<stremio::LibItem>& items,
                           const std::string& header, const char* emptyMsg)
{
    parkFocusOffList();                       // never clearViews a focused row
    *rowsAlive = false;                       // same: these rows are about to die
    rowsAlive  = std::make_shared<bool>(true);
    libList->clearViews();

    if (items.empty())
    {
        showStatus(emptyMsg, false);   // centered message, no bar
        stremio::setLibraryCount(header);
        pendingSlide = 0;              // nothing to slide
        libList->setTranslationX(0.0f);
        sliding = false;
        // No rows to land on, but still clear the one-shot flags so they do not
        // leak into the next render.
        resetOnShow       = false;
        suppressFocusMove = false;
        return;
    }
    // Rows to show: hide the centered overlay entirely.
    loadingBox->setVisibility(brls::Visibility::GONE);
    stremio::setLibraryCount(header);

    // A catalog is a hundred titles, and building a hundred cards (each with an
    // artwork request behind it) is what made these views take seconds to come
    // up. Show a screenful; the rest is one press away behind See More. Only the
    // catalogs are capped -- Continue Watching and Library are the user's own
    // lists, and they are as long as they are.
    const size_t cap = capped() ? kSectionMax : 0;
    auto head = [cap](const std::vector<stremio::LibItem>& v) {
        return cap && v.size() > cap
                   ? std::vector<stremio::LibItem>(v.begin(), v.begin() + cap)
                   : v;
    };
    auto overflows = [cap](const std::vector<stremio::LibItem>& v) {
        return cap && v.size() > cap;
    };

    columnsShown = false;  // the strips are one row of cards, not two columns
    if (posterStyle())
    {
        // Straight into the list, top-aligned like the rows: a wrapper sized to
        // the viewport to centre the strip crashed on a UI-size change (setting
        // a height from inside onLayout re-enters the layout) and left the list
        // one pixel too tall, which put a scrollbar on a page that does not
        // scroll.
        brls::View* last = addStripSection(
            sectionTitle(), head(items),
            overflows(items) ? buildSeeMoreCard(sectionTitle(), items,
                                              catalogType(), "top")
                             : nullptr);
        // Popular Movies / Shows carry a second strip under the first. It is
        // absent until Cinemeta's second catalog lands (loadFeatured re-renders
        // when it does), and stays absent if that request fails.
        const std::vector<stremio::LibItem>* feat = featuredCache();
        if (feat && !feat->empty())
            last = addStripSection(
                tr("Featured"), head(*feat),
                overflows(*feat) ? buildSeeMoreCard(tr("Featured"), *feat,
                                                   catalogType(), "year")
                                 : nullptr);
        finishList(last);
        return;
    }

    const float ins = headingInset();
    const std::vector<stremio::LibItem>* feat = featuredCache();
    columnsShown = feat && !feat->empty();

    // Two sections in the row styles sit SIDE BY SIDE -- Popular left, Featured
    // right -- the way Search lays its two types out. Stacked, the second one
    // was a scroll away and easy to miss.
    if (feat && !feat->empty())
    {
        auto* split = new brls::Box();
        split->setAxis(brls::Axis::ROW);
        split->setAlignItems(brls::AlignItems::FLEX_START);

        auto makeCol = [&](const std::string& title) {
            auto* col = new brls::Box();
            col->setAxis(brls::Axis::COLUMN);
            col->setWidthPercentage(50.0f);  // hard 50/50, not content-sized
            auto* h = new brls::Label();
            h->setText(title);
            h->setFontSize(20.0f);
            h->setTextColor(theme::textMuted());
            h->setMargins(0.0f, 0.0f, 10.0f, ins);
            col->addView(h);
            return col;
        };
        auto* popCol  = makeCol(sectionTitle());
        auto* featCol = makeCol(tr("Featured"));
        // No type tag on these rows: both columns are the one type this view is
        // about, and half a row is narrow enough already.
        for (const auto& it : head(items)) popCol->addView(buildItemRow(it, false));
        for (const auto& it : head(*feat)) featCol->addView(buildItemRow(it, false));
        if (overflows(items))
            popCol->addView(
                buildSeeMoreRow(sectionTitle(), items, catalogType(), "top"));
        if (overflows(*feat))
            featCol->addView(
                buildSeeMoreRow(tr("Featured"), *feat, catalogType(), "year"));

        split->addView(popCol);
        split->addView(featCol);
        libList->addView(split);
        finishList(split);
        return;
    }

    // One section: still titled, so Continue Watching and Library say what they
    // are here as well as in the poster style.
    addHeading(sectionTitle(), 0.0f, 10.0f, ins);
    brls::View* last = nullptr;
    for (const auto& it : head(items)) last = addItemRow(it);
    if (overflows(items))
    {
        last = buildSeeMoreRow(sectionTitle(), items, catalogType(), "top");
        libList->addView(last);
    }
    finishList(last);
}

// Builds one poster/title/progress row for `it`, adds it to libList, returns it.
brls::Box* StremioTab::addItemRow(const stremio::LibItem& it)
{
    auto* row = buildItemRow(it, true);
    libList->addView(row);
    return row;
}

namespace
{

// "1h 59m" / "47m" -- a runtime, or what is left of one. Empty under half a
// minute, which is what tells the progress line to fall back to a clock.
std::string humanRuntime(double ms)
{
    int mins = (int)(ms / 60000.0 + 0.5);
    if (mins <= 0) return "";
    char buf[32];
    if (mins >= 60)
        std::snprintf(buf, sizeof(buf), "%dh %02dm", mins / 60, mins % 60);
    else
        std::snprintf(buf, sizeof(buf), "%dm", mins);
    return buf;
}

// A position on the clock: "15:34", or "1:02:11" once it runs past an hour.
std::string clockTime(double ms)
{
    int secs = ms > 0 ? (int)(ms / 1000.0) : 0;
    char buf[32];
    if (secs >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", secs / 3600,
                      (secs / 60) % 60, secs % 60);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
    return buf;
}

// The year as a row should read it. A show still running comes back as an open
// range ("2013-"), which reads as a sentence someone forgot to finish -- say
// what it means instead. The trailing dash may be an en dash: some addons send
// that where Cinemeta sends a hyphen.
std::string yearLine(std::string y)
{
    static const std::string kEnDash = "–";
    while (!y.empty() && (y.back() == ' ' || y.back() == '\t')) y.pop_back();

    bool open = false;
    if (!y.empty() && y.back() == '-')
    {
        y.pop_back();
        open = true;
    }
    else if (y.size() > kEnDash.size() &&
             y.compare(y.size() - kEnDash.size(), kEnDash.size(), kEnDash) == 0)
    {
        y.erase(y.size() - kEnDash.size());
        open = true;
    }
    if (!open) return y;

    while (!y.empty() && y.back() == ' ') y.pop_back();
    return y.empty() ? y : y + tr(" – present");
}

// "Season 17 · Episode 7" from a state video_id ("tt123:17:7"), "" if it is not
// one -- a film's video_id is just the item id. The episode's own title would
// need a meta fetch per row, which the list does not do.
std::string episodeLine(const std::string& videoId)
{
    size_t p2 = videoId.rfind(':');
    if (p2 == std::string::npos || p2 == 0) return "";
    size_t p1 = videoId.rfind(':', p2 - 1);
    if (p1 == std::string::npos) return "";
    return tr("Season ") + videoId.substr(p1 + 1, p2 - p1 - 1) +
           " · " + tr("Episode ") +
           videoId.substr(p2 + 1);
}

} // namespace

// The poster style has no row of its own: Search lays its results out in two
// narrow columns, where an upright card does not fit. Those keep the card row.
brls::Box* StremioTab::buildItemRow(const stremio::LibItem& it, bool showType)
{
    return config::get().listStyle == kStyleClassic ? buildClassicRow(it, showType)
                                                   : buildCardRow(it, showType);
}

// The focusable row both styles are built on: its size, its click action and
// its tap gesture. Empty -- the caller fills it.
// X on a Continue Watching tile drops it from the row. Only there: everywhere
// else the same key means something else or nothing, and the footer hint --
// which borealis draws from the registered action -- would be a lie.
//
// The account has no "continue watching" list to delete from; the row is
// derived from each item's watch state, so clearing that state is the removal
// (see stremio::clearWatchStateAsync).
void StremioTab::bindRemoveFromContinue(brls::Box* card,
                                        const stremio::LibItem& it)
{
    if (view != View::ContinueWatching) return;

    auto live = alive;
    std::string key = authKey;
    // Just "Remove": the hint sits in the footer next to every other one, and
    // the full sentence pushed them all along.
    card->registerAction(
        tr("Remove"), brls::BUTTON_X,
        [this, live, key, id = it.id](brls::View*) {
            stremio::clearWatchStateAsync(key, id, [this, live, id](bool ok) {
                if (!*live) return;
                if (!ok)
                {
                    brls::Application::notify(tr("Could not remove it"));
                    return;
                }
                // Clear the same fields locally rather than dropping the item:
                // Continue Watching is filtered on them, so this takes it out
                // of the row while leaving it in the Library view where it may
                // well belong. Then re-render, which parks focus safely.
                for (auto& x : libItems)
                    if (x.id == id)
                    {
                        x.videoId.clear();
                        x.timeOffsetMs = 0;
                        x.durationMs   = 0;
                    }
                renderView();
            });
            return true;
        },
        false, false, brls::SOUND_CLICK);
}

brls::Box* StremioTab::newRowShell(const stremio::LibItem& it, float height)
{
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(height);  // sized off the poster, not the text
    row->setPaddingLeft(16.0f);
    row->setPaddingRight(24.0f);
    // Margin, not padding, and on the row rather than the list: the focus
    // highlight draws OUTSIDE the row's bounds and the frame scissors to its own
    // width, so a row reaching the edge has its glow clipped. Padding only moved
    // the text and left the box (and its glow) at the edge; the row itself has to
    // stop short. Also keeps the scrolling indicator, pinned to the frame's edge,
    // in a lane of its own.
    row->setMarginRight(40.0f);
    row->setCornerRadius(6.0f);
    row->setFocusable(true);
    // Series open a season/episode picker, films go straight to sources.
    std::string key = authKey;
    row->registerClickAction([key, it](brls::View*) {
        openLibraryItem(key, it);
        return true;
    });
    bindRemoveFromContinue(row, it);
    // Tap gesture so the touchscreen works too (A-only otherwise).
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
    return row;
}

// Posters are 2:3, so fit rather than stretch. The artwork arrives
// asynchronously: the row draws immediately with an empty slot and it fills in,
// instead of the whole list waiting on the network.
brls::Image* StremioTab::newPoster(const stremio::LibItem& it, float w, float h,
                                   float marginRight)
{
    auto* art = new brls::Image();
    art->setDimensions(w, h);
    art->setScalingType(brls::ImageScalingType::FIT);
    art->setCornerRadius(6.0f);  // rounded poster corners (borealis clips it)
    art->setMarginRight(marginRight);
    attachPoster(art, it);
    return art;
}

// Fills `art` in once the artwork lands: the row draws immediately with an
// empty slot instead of the whole list waiting on the network.
void StremioTab::attachPoster(brls::Image* art, const stremio::LibItem& it)
{
    // artToken is set while a pushed page builds cards of its own (see
    // openSection). Those Images die when that page is popped, not when the
    // tab's list is rebuilt, so the fetch has to be tied to whichever of the
    // two is the shorter-lived owner of this particular Image.
    auto alive = artToken ? artToken : rowsAlive;
    stremio::fetchPosterAsync(it.id, it.poster, [art, alive](std::string path) {
        if (!*alive || path.empty()) return;
        art->setImageFromFile(path);
    });
}

// The tab's background colour at a screen point. The field is one linear
// gradient across the whole frame, from the top-RIGHT corner to the bottom-left
// (see BrowserFrame::draw), so this reproduces nanovg's own parameter -- the
// projection of the point onto that axis -- and interpolates the same pair. The
// frame is borealis' logical size, which follows the UI-size setting and the
// dock state, so it is read live rather than cached.
NVGcolor tabBgAt(float px, float py)
{
    const float W = brls::Application::contentWidth,
                H = brls::Application::contentHeight;
    // Axis: (W, 0) -> (0, H).
    float dx = -W, dy = H;
    float len2 = dx * dx + dy * dy;
    float t    = len2 > 0.0f ? ((px - W) * dx + py * dy) / len2 : 0.0f;
    t          = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const NVGcolor a = theme::gradTop(), b = theme::gradBottom();
    return nvgRGBAf(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                    a.b + (b.b - a.b) * t, 1.0f);
}

// A strip that fades its cards out at whichever edge still has content beyond
// it, so they dissolve into the background instead of being cut off -- the same
// treatment the episode and addon scrollers get. The scrim is the background
// colour sampled AT the edge, fading to transparent inward; an edge with
// nothing past it is left alone, so the first card at rest is never dimmed.
class FadeStrip : public brls::HScrollingFrame
{
  public:
    void setContentView(brls::View* view)
    {
        content = view;
        brls::HScrollingFrame::setContentView(view);
    }

    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override
    {
        brls::HScrollingFrame::draw(vg, x, y, w, h, style, ctx);

        const float fade = 64.0f;
        float offset     = getContentOffsetX();
        float rightLimit = content ? content->getWidth() - w : 0.0f;
        float midY       = y + h * 0.5f;

        if (offset > 2.0f)  // scrolled: cards run off the left
        {
            NVGcolor edge = tabBgAt(x, midY);
            NVGcolor gone = edge;
            gone.a        = 0.0f;
            nvgBeginPath(vg);
            nvgRect(vg, x, y, fade, h);
            nvgFillPaint(vg, nvgLinearGradient(vg, x, y, x + fade, y, edge, gone));
            nvgFill(vg);
        }
        if (offset < rightLimit - 2.0f)  // more cards off the right
        {
            NVGcolor edge = tabBgAt(x + w, midY);
            NVGcolor gone = edge;
            gone.a        = 0.0f;
            nvgBeginPath(vg);
            nvgRect(vg, x + w - fade, y, fade, h);
            nvgFillPaint(vg,
                         nvgLinearGradient(vg, x + w - fade, y, x + w, y, gone, edge));
            nvgFill(vg);
        }
    }

  private:
    brls::View* content = nullptr;
};

// A poster with only its top corners rounded: it meets the card's footer flush,
// and a rounded bottom edge cut two notches of card background out of the
// artwork. brls::Image rounds all four corners, so this repeats its draw with a
// varying-radius rect -- everything it needs is protected, not private.
class TopRoundedImage : public brls::Image
{
  public:
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override
    {
        if (this->texture == 0) return;

        float ix = x + this->imageX;
        float iy = y + this->imageY;
        this->paint.xform[4] = ix;
        this->paint.xform[5] = iy;

        float r = this->getCornerRadius();
        nvgBeginPath(vg);
        if (this->getClipsToBounds())
            nvgRoundedRectVarying(vg, x, y, w, h, r, r, 0.0f, 0.0f);
        else
            nvgRoundedRectVarying(vg, ix, iy, this->imageWidth,
                                  this->imageHeight, r, r, 0.0f, 0.0f);
        nvgFillPaint(vg, a(this->paint));
        nvgFill(vg);
    }
};

// The card's panel: the surface the title, the meta line and the progress bar
// sit on. It drops its own fill while the row is focused -- the cursor draws a
// background of its own behind the whole row, and the two stacked surfaces read
// as a paler rectangle floating inside the highlight rather than as one lit
// card. Read as it draws rather than switched on a focus event: nothing has to
// find these rows to keep them in step.
class CardPanel : public brls::Box
{
  public:
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override
    {
        // Only while the cursor is actually drawn: View::drawHighlight bails out
        // on touch input, so under a finger there is no highlight to take over
        // the fill -- the card would just go hollow, and worse, the row keeps
        // its focus while a touch scroll carries it away, so the wrong card
        // stays hollow. Same test as drawHighlight's, so the two cannot drift.
        brls::View* row = this->getParent();
        bool cursor     = brls::Application::getInputType() != brls::InputType::TOUCH;
        this->setBackgroundColor(cursor && row && row->isFocused()
                                     ? nvgRGBA(0, 0, 0, 0)
                                     : theme::scrim(16));
        brls::Box::draw(vg, x, y, w, h, style, ctx);
    }
};

// The card style: the poster stands on the background, everything else sits on
// a raised panel next to it -- title, the episode or the runtime under it, how
// much is left, and the progress bar across the bottom, with the type as a pill
// and a chevron on the right.
brls::Box* StremioTab::buildCardRow(const stremio::LibItem& it, bool showType)
{
    // The highlight hugs the row's padding box: no padding at all, and a row
    // exactly as tall as its content, so the cursor sits ON the poster and the
    // card rather than around them. The gap between rows therefore has to be a
    // margin (finishList overrides the last row's with the bottom inset).
    auto* row = newRowShell(it, kCardHeight);
    row->setPadding(0.0f, 0.0f, 0.0f, 0.0f);
    row->setMarginBottom(14.0f);
    // One radius for the cursor, the card and the poster -- they are drawn on
    // top of each other, so three different corners read as sloppy. The cursor
    // needs saying separately: it is NOT the view's cornerRadius but its own
    // highlightCornerRadius, which defaults to the style's 6. And it is stroked
    // centred on a rect inflated by half the stroke width, so its radius has to
    // grow by the same to stay concentric with the card underneath.
    row->setCornerRadius(kCardRadius);
    row->setHighlightCornerRadius(
        kCardRadius + brls::Application::getStyle()["brls/highlight/stroke_width"] / 2);

    auto* art = newPoster(it, 88.0f, kCardHeight, 18.0f);
    art->setCornerRadius(kCardRadius);
    row->addView(art);

    auto* card = new CardPanel();
    card->setAxis(brls::Axis::ROW);
    card->setAlignItems(brls::AlignItems::CENTER);
    card->setGrow(1.0f);
    card->setHeight(kCardHeight);  // the poster's height, so they line up
    card->setCornerRadius(kCardRadius);
    card->setPaddingLeft(22.0f);
    card->setPaddingRight(18.0f);
    row->addView(card);

    auto* textCol = new brls::Box();
    textCol->setAxis(brls::Axis::COLUMN);
    textCol->setJustifyContent(brls::JustifyContent::CENTER);
    textCol->setGrow(1.0f);

    auto* name = new brls::Label();
    name->setText(it.name);
    name->setFontSize(26.0f);
    name->setSingleLine(true);
    textCol->addView(name);

    double prog    = it.progress();
    bool   watched = prog > 0.005;

    // Under the title: which episode the account is on for a show, and the year
    // for everything else. (A show that has not been started would rather say
    // how many seasons it has, but that is a meta request per row -- a whole
    // extra fetch per title for one number.) Last resort, the runtime, if
    // playing it ever told us: better than an empty line.
    std::string meta;
    if (watched && it.type == "series") meta = episodeLine(it.videoId);
    if (meta.empty()) meta = yearLine(it.year);
    if (meta.empty()) meta = humanRuntime(it.durationMs);
    if (!meta.empty())
    {
        auto* metaLbl = new brls::Label();
        metaLbl->setText(meta);
        metaLbl->setFontSize(18.0f);
        metaLbl->setTextColor(theme::textMuted());
        metaLbl->setSingleLine(true);
        metaLbl->setMarginTop(5.0f);
        textCol->addView(metaLbl);
    }

    if (watched)
    {
        // The same clock for films and episodes -- where you are over how long
        // it runs. What is left of a film is said on the meta line above.
        //
        // AccentLabel/AccentBox, not the accent copied in: these rows live on
        // the main screen for as long as the tab does, so a colour change in
        // Options has to reach them without the tab being rebuilt.
        auto* posLbl = new theme::AccentLabel();
        posLbl->setText(clockTime(it.timeOffsetMs) + " / " +
                        clockTime(it.durationMs));
        posLbl->setFontSize(18.0f);
        posLbl->setSingleLine(true);
        posLbl->setMarginTop(7.0f);
        textCol->addView(posLbl);

        auto* track = new brls::Box();
        track->setWidthPercentage(100.0f);
        track->setHeight(6.0f);
        track->setCornerRadius(3.0f);
        track->setBackgroundColor(theme::scrim(40));
        track->setMarginTop(9.0f);

        auto* fill = new theme::AccentBox();
        fill->setWidthPercentage((float)(prog * 100.0));
        fill->setHeight(6.0f);
        fill->setCornerRadius(3.0f);
        track->addView(fill);
        textCol->addView(track);
    }
    card->addView(textCol);

    if (showType)
    {
        auto* pill = new brls::Box();
        pill->setAxis(brls::Axis::ROW);
        pill->setAlignItems(brls::AlignItems::CENTER);
        pill->setCornerRadius(14.0f);
        pill->setBackgroundColor(theme::scrim(30));
        // setPadding(top, right, bottom, left). Not even top/bottom: a Label's
        // box sits lower than its ink (the line box carries the descender), so
        // equal padding leaves a visibly thinner band above the text.
        pill->setPadding(9.0f, 14.0f, 5.0f, 14.0f);
        pill->setMarginLeft(18.0f);
        // Hold its width: the full-width progress bar in textCol otherwise
        // squeezes it until the label wraps.
        pill->setShrink(0.0f);

        auto* type = new brls::Label();
        type->setText(it.type == "series" ? tr("Show") : tr("Movie"));
        type->setFontSize(18.0f);
        type->setTextColor(theme::textDim());
        type->setSingleLine(true);
        pill->addView(type);
        card->addView(pill);
    }

    // Material "chevron_right", U+E5CC (borealis' fallback font): says the row
    // opens something rather than toggling in place. Careful next to it: U+E5CB
    // is chevron_LEFT.
    auto* chevron = new brls::Label();
    chevron->setText("");
    chevron->setFontSize(32.0f);
    chevron->setTextColor(theme::textMuted());
    // The glyph sits high in its line box, so centring that box on the card
    // leaves the arrow above the pill beside it. setMargins(top, right, bottom,
    // left): the row centres the box *with* its margins, so the arrow moves down
    // by half of the top one.
    chevron->setMargins(12.0f, 0.0f, 0.0f, 14.0f);
    chevron->setShrink(0.0f);
    card->addView(chevron);

    return row;
}

// The poster style's strip: one horizontally scrolling row of cards. It is a
// single view in the (vertical) list, so the list itself has nothing left to
// scroll -- moving through the titles is the strip's job.
brls::View* StremioTab::buildPosterStrip(const std::vector<stremio::LibItem>& items,
                                         bool upToHeader, brls::Box* seeMore)
{
    // Taller than the cards: the frame scissors to its own bounds, and the
    // cursor's glow is drawn OUTSIDE the card it belongs to. Without the slack
    // it would be shaved off top and bottom. Kept tight -- it doubles as the gap
    // under the section heading.
    const float stripH = kPosterCardH + 16.0f;

    auto* strip = new FadeStrip();
    strip->setHeight(stripH);
    // CENTERED, not the default NATURAL: NATURAL free-scrolls by the pixel and
    // hands focus to the FRAME to do it, which slid the strip under its own left
    // edge and left half a card showing. CENTERED moves card by card and scrolls
    // to keep the focused one in view -- the same fix the vertical list uses.
    strip->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);  // the slack, split evenly
    for (const auto& it : items)
    {
        auto* card = buildPosterCard(it, upToHeader);
        // The screen inset the list gives up in this style (see renderView), so
        // the first card starts where the rows do.
        if (row->getChildren().empty()) card->setMarginLeft(kPosterInset);
        row->addView(card);
    }
    // The tail of a capped section: the rest of it, one press away.
    if (seeMore) row->addView(seeMore);
    // setContentView forces the content view's height and detaches it, so any
    // margin here would be dropped -- the insets live on the cards.
    strip->setContentView(row);
    return strip;
}

brls::Box* StremioTab::buildPosterCard(const stremio::LibItem& it, bool upToHeader)
{
    auto* card = new brls::Box();
    card->setAxis(brls::Axis::COLUMN);
    card->setWidth(kPosterCardW);
    card->setHeight(kPosterCardH);
    // setMargins(top, right, bottom, left): the gap between cards, and enough
    // on the left for the first card's glow to clear the frame's edge.
    card->setMargins(0.0f, 16.0f, 0.0f, 8.0f);
    card->setCornerRadius(kCardRadius);
    card->setBackgroundColor(theme::scrim(16));
    card->setFocusable(true);
    card->setHighlightCornerRadius(
        kCardRadius +
        brls::Application::getStyle()["brls/highlight/stroke_width"] / 2);

    std::string key = authKey;
    card->registerClickAction([key, it](brls::View*) {
        openLibraryItem(key, it);
        return true;
    });
    card->addGestureRecognizer(new brls::TapGestureRecognizer(card));
    bindRemoveFromContinue(card, it);
    // A card in the page's FIRST strip cannot walk focus out on its own (the
    // frames keep it inside while they can still scroll), so it is handed the
    // way back to the header. Any strip below one has somewhere to go on its
    // own -- the section above it, or the search bar -- and routing those to the
    // header too made UP skip the whole page.
    if (upToHeader && stremio::libraryUpTarget)
        card->setCustomNavigationRoute(brls::FocusDirection::UP,
                                       stremio::libraryUpTarget);

    // The artwork, with the type tag dropped into its bottom-right corner: on a
    // card this narrow the tag was taking half the title's line and truncating
    // most names.
    auto* artBox = new brls::Box();
    artBox->setWidth(kPosterCardW);
    artBox->setHeight(kPosterArtH);

    auto* art = new TopRoundedImage();
    art->setDimensions(kPosterCardW, kPosterArtH);
    art->setScalingType(brls::ImageScalingType::FILL);  // fill, crop the excess
    art->setClipsToBounds(true);   // ... which is what keeps the crop inside
    art->setCornerRadius(kCardRadius);
    attachPoster(art, it);
    artBox->addView(art);

    auto* pill = new brls::Box();
    pill->setAxis(brls::Axis::ROW);
    pill->setAlignItems(brls::AlignItems::CENTER);
    pill->setCornerRadius(11.0f);
    // Fixed translucent black with light text, not the theme's scrim: this one
    // sits on artwork rather than on a surface, so it has to hold up over a
    // bright poster as well as a dark one, in either variant.
    pill->setBackgroundColor(nvgRGBA(0, 0, 0, 170));
    pill->setPadding(7.0f, 10.0f, 3.0f, 10.0f);  // see buildCardRow: not even
    pill->setPositionType(brls::PositionType::ABSOLUTE);
    pill->setPositionBottom(10.0f);
    pill->setPositionRight(10.0f);

    auto* type = new brls::Label();
    type->setText(it.type == "series" ? tr("Show") : tr("Movie"));
    type->setFontSize(15.0f);
    type->setTextColor(nvgRGBA(255, 255, 255, 235));
    type->setSingleLine(true);
    pill->addView(type);
    artBox->addView(pill);

    card->addView(artBox);

    auto* foot = new brls::Box();
    foot->setAxis(brls::Axis::COLUMN);
    foot->setJustifyContent(brls::JustifyContent::CENTER);
    foot->setGrow(1.0f);
    foot->setPaddingLeft(14.0f);
    foot->setPaddingRight(14.0f);

    // The title now has the line to itself -- the type tag lives on the poster.
    auto* name = new brls::Label();
    name->setText(it.name);
    name->setFontSize(21.0f);
    name->setSingleLine(true);
    foot->addView(name);

    double prog    = it.progress();
    bool   watched = prog > 0.005;

    // The same line as the row style: the episode for a show being watched, the
    // year otherwise.
    std::string meta;
    if (watched && it.type == "series") meta = episodeLine(it.videoId);
    if (meta.empty()) meta = yearLine(it.year);
    if (meta.empty()) meta = humanRuntime(it.durationMs);
    if (!meta.empty())
    {
        auto* metaLbl = new brls::Label();
        metaLbl->setText(meta);
        metaLbl->setFontSize(16.0f);
        metaLbl->setTextColor(theme::textMuted());
        metaLbl->setSingleLine(true);
        metaLbl->setMarginTop(7.0f);
        foot->addView(metaLbl);
    }

    if (watched)
    {
        auto* posLbl = new theme::AccentLabel();
        posLbl->setText(clockTime(it.timeOffsetMs) + " / " +
                        clockTime(it.durationMs));
        posLbl->setFontSize(16.0f);
        posLbl->setSingleLine(true);
        posLbl->setMarginTop(7.0f);
        foot->addView(posLbl);

        auto* track = new brls::Box();
        track->setWidthPercentage(100.0f);
        track->setHeight(5.0f);
        track->setCornerRadius(2.5f);
        track->setBackgroundColor(theme::scrim(40));
        track->setMarginTop(9.0f);

        auto* fill = new theme::AccentBox();
        fill->setWidthPercentage((float)(prog * 100.0));
        fill->setHeight(5.0f);
        fill->setCornerRadius(2.5f);
        track->addView(fill);
        foot->addView(track);
    }
    card->addView(foot);

    return card;
}

// The flat style the app shipped with: poster, title, progress bar, type.
brls::Box* StremioTab::buildClassicRow(const stremio::LibItem& it, bool showType)
{
    auto* row = newRowShell(it, 140.0f);
    row->addView(newPoster(it, 80.0f, 120.0f, 22.0f));

    // Name over an optional watch-progress bar (where the account is in this
    // film / the show's last watched episode, from the library state).
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

    // For a show mid-episode, name the episode under the title.
    std::string ep = prog > 0.005 && it.type == "series" ? episodeLine(it.videoId)
                                                         : std::string();
    if (!ep.empty())
    {
        auto* epl = new brls::Label();
        epl->setText(ep);
        epl->setFontSize(18.0f);
        epl->setTextColor(theme::textMuted());
        epl->setSingleLine(true);
        epl->setMarginTop(4.0f);
        textCol->addView(epl);
    }

    if (prog > 0.005)
    {
        auto* track = new brls::Box();
        track->setWidthPercentage(100.0f);
        track->setHeight(5.0f);
        track->setCornerRadius(2.5f);
        track->setBackgroundColor(theme::scrim(40));
        track->setMarginTop(10.0f);

        // AccentBox, not a Box with the accent copied in: these rows live on the
        // main screen for as long as the tab does, so a colour change in Options
        // has to reach them without the tab being rebuilt.
        auto* fill = new theme::AccentBox();
        fill->setWidthPercentage((float)(prog * 100.0));
        fill->setHeight(5.0f);
        fill->setCornerRadius(2.5f);
        track->addView(fill);
        textCol->addView(track);
    }
    row->addView(textCol);

    if (showType)
    {
        auto* type = new brls::Label();
        type->setText(it.type == "series" ? tr("Show") : tr("Movie"));
        type->setFontSize(19.0f);
        type->setTextColor(theme::textMuted());
        type->setMarginLeft(16.0f);
        // Keep it on one line and let it hold its width: the full-width progress
        // bar in textCol otherwise squeezed this label until it wrapped.
        type->setSingleLine(true);
        type->setShrink(0.0f);
        row->addView(type);
    }

    return row;
}

// Common tail after (re)building libList: first-row inset, focus/scroll reset on
// a view change, the up-route to the tab bar, and the bottom inset.
void StremioTab::finishList(brls::View* lastRow)
{
    if (!libList->getChildren().empty())
    {
        // Top inset on the first row, mirroring the bottom one below: it starts
        // right under the header otherwise.
        libList->getChildren()[0]->setMarginTop(20.0f);

        // The first thing on the page that can actually take the cursor -- NOT
        // simply the first child: with a section heading above it, that child is
        // a Label, giveFocus() on a view that resolves to nothing is a silent
        // no-op, and Application::currentFocus was left parked on libraryBox,
        // which this function then makes unfocusable. Everything after that is
        // navigating from a view that is not in the running, and the boxes it
        // walks still hold lastFocusedView pointers into the list we just freed.
        brls::View* first = nullptr;
        for (brls::View* child : libList->getChildren())
            if ((first = child->getDefaultFocus())) break;

        brls::View* focus = brls::Application::getCurrentFocus();
        bool parked = !focus || focus == libraryBox || isUnder(focus, loginBox);
        libraryBox->setFocusable(false);
        // resetOnShow (a view change) forces the cursor to the first row and the
        // list back to the top, regardless of where focus was in the old list --
        // unless suppressFocusMove (a header tab-bar pick) asked to leave focus
        // where it is, up on the bar.
        if (first && (parked || resetOnShow) && !suppressFocusMove)
            brls::Application::giveFocus(first);
        if (resetOnShow && libScroll)
            libScroll->setContentOffsetY(0.0f, false);

        if (first && stremio::libraryUpTarget)
            first->setCustomNavigationRoute(brls::FocusDirection::UP,
                                            stremio::libraryUpTarget);
    }
    resetOnShow       = false;
    suppressFocusMove = false;

    // Kick off the horizontal slide-in for a view change.
    if (pendingSlide != 0)
    {
        slideSign  = pendingSlide;
        slideStart = std::chrono::duration<double>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
        sliding      = true;
        pendingSlide = 0;
    }

    // Breathing room under the last row, so scrolled to the end it sits above the
    // screen edge instead of against it.
    if (lastRow) lastRow->setMarginBottom(32.0f);
}
