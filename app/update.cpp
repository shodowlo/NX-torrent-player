#include "update.hpp"

#include <borealis.hpp>

#include <switch.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "http.hpp"
#include "json.hpp"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
#ifndef UPDATE_API_URL
#define UPDATE_API_URL "https://api.github.com/repos/shodowlo/NX-torrent-player/releases/latest"
#endif

namespace update
{
namespace
{

std::string selfNro;  // set by init(), "" if we could not tell

std::string pendingPath()
{
    return selfNro.empty() ? "" : selfNro + ".new";
}

// "v1.2.3" / "1.2" / "1.2.3-beta" -> {1,2,3}. Anything unparsable reads as 0,
// which makes a malformed tag look older than us rather than newer -- a bad tag
// should not push an update at everyone.
void parseVersion(const std::string& s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
    for (int part = 0; part < 3 && i < s.size(); part++)
    {
        int n = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
        {
            n = n * 10 + (s[i++] - '0');
            any = true;
        }
        if (!any) break;
        out[part] = n;
        if (i >= s.size() || s[i] != '.') break;  // "-beta" and friends end it
        i++;
    }
}

bool isNewer(const std::string& candidate, const std::string& current)
{
    int a[3], b[3];
    parseVersion(candidate, a);
    parseVersion(current, b);
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

} // namespace

void init(int argc, char** argv)
{
    // argv[0] is the .nro's own path when launched from the Homebrew Menu. If it
    // is missing (some loaders, or a debugger) we leave it empty: refusing to
    // update beats guessing a path and overwriting the wrong file.
    if (argc > 0 && argv && argv[0] && argv[0][0])
        selfNro = argv[0];
    brls::Logger::info("[update] running from {}",
                       selfNro.empty() ? "(unknown)" : selfNro);
}

std::string selfPath() { return selfNro; }

bool hasPending()
{
    std::string p = pendingPath();
    if (p.empty()) return false;
    // The file, not a flag: a download that survived a crash is still good, and
    // gets applied on the next clean exit.
    if (FILE* f = std::fopen(p.c_str(), "rb"))
    {
        std::fclose(f);
        return true;
    }
    return false;
}

void checkAsync(std::function<void(Release)> done)
{
    if (selfNro.empty())
    {
        Release r;
        r.error = "unknown install path";
        brls::sync([done, r]() { done(r); });
        return;
    }

    brls::async([done]() {
        Release r;
        std::string resp, err;
        if (!http::get(UPDATE_API_URL, resp, err))
        {
            r.error = err;
            brls::Logger::info("[update] check failed: {}", err);
            brls::sync([done, r]() { done(r); });
            return;
        }

        std::string tag = json::str(resp, "tag_name");
        if (tag.empty())
        {
            // No releases yet, or a rate-limit / error body.
            r.error = json::str(resp, "message");
            if (r.error.empty()) r.error = "no release found";
            brls::Logger::info("[update] no tag_name: {}", r.error);
            brls::sync([done, r]() { done(r); });
            return;
        }

        r.ok      = true;
        r.version = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
        r.notes   = json::str(resp, "body");

        // The .nro asset. The release also carries the zip for manual installs;
        // downloading that would mean pulling the same bytes to then unpack one
        // file out of them.
        for (const auto& a : json::objects(resp, "assets"))
        {
            std::string name = json::str(a, "name");
            if (name.size() < 4 || name.compare(name.size() - 4, 4, ".nro") != 0)
                continue;
            r.url  = json::str(a, "browser_download_url");
            r.size = json::integer(a, "size");
            break;
        }

        r.newer = isNewer(r.version, APP_VERSION);
        brls::Logger::info("[update] latest={} running={} newer={} asset={}",
                           r.version, APP_VERSION, r.newer,
                           r.url.empty() ? "(none)" : r.url);
        if (r.newer && r.url.empty())
            brls::Logger::warning(
                "[update] release {} is newer but ships no .nro asset", tag);

        brls::sync([done, r]() { done(r); });
    });
}

void downloadAsync(const Release& r, std::function<void(float)> progress,
                   std::function<void(std::string)> done)
{
    std::string url  = r.url;
    std::string dest = pendingPath();
    if (dest.empty() || url.empty())
    {
        brls::sync([done]() { done("nothing to download"); });
        return;
    }

    brls::async([url, dest, progress, done]() {
        // The callback runs on this thread, per curl chunk. Marshal to the UI
        // thread, but only when the whole percent changes -- a sync() per chunk
        // would queue thousands of no-op UI updates.
        auto last = std::make_shared<int>(-1);
        std::string err;
        bool ok = http::download(url, dest, err,
                                 [progress, last](int64_t now, int64_t total) {
                                     float f = total > 0 ? (float)now / total : -1.0f;
                                     int pct = total > 0 ? (int)(f * 100) : -1;
                                     if (pct != *last)
                                     {
                                         *last = pct;
                                         brls::sync([progress, f]() { progress(f); });
                                     }
                                     return true;
                                 });
        std::string e = ok ? "" : err;
        if (!ok) brls::Logger::error("[update] download failed: {}", err);
        brls::sync([done, e]() { done(e); });
    });
}

namespace
{

// A trimmed release body. GitHub notes can be pages of markdown; a modal is not
// the place to read them.
std::string shortNotes(const std::string& notes)
{
    std::string t;
    int lines = 0;
    for (size_t i = 0; i < notes.size() && lines < 6; i++)
    {
        if (notes[i] == '\r') continue;
        if (notes[i] == '\n') lines++;
        t += notes[i];
        if (t.size() > 300) { t += "..."; break; }
    }
    while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
    return t;
}

void note(const std::string& msg)
{
    auto* d = new brls::Dialog(msg);
    d->addButton("OK", []() {});
    d->open();
}

std::string humanMB(int64_t bytes)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

} // namespace

void promptInstall(const Release& r)
{
    std::string msg = "Version " + r.version + " is available.";
    if (r.size > 0) msg += "  (" + humanMB(r.size) + ")";
    std::string n = shortNotes(r.notes);
    if (!n.empty()) msg += "\n\n" + n;

    auto* ask = new brls::Dialog(msg);
    ask->addButton("Later", []() {});
    ask->addButton("Update", [r]() {
        // Its own dialog, with a label we keep writing into. Not cancelable:
        // there is no partial file to leave behind (http::download cleans up),
        // but B-ing out mid-transfer would leave the callbacks writing to a
        // freed label.
        auto* box = new brls::Box();
        box->setAxis(brls::Axis::COLUMN);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->setPadding(24.0f, 30.0f, 24.0f, 30.0f);

        auto* label = new brls::Label();
        label->setText("Downloading... 0%");
        label->setFontSize(20.0f);
        label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        box->addView(label);

        auto* track = new brls::Box();
        track->setWidth(320.0f);
        track->setHeight(8.0f);
        // Without this the track is just a suggestion: it is a flex item, so a
        // dialog narrower than 320 shrinks it -- while the fill's percentage
        // keeps resolving against the width we asked for. The fill then runs
        // past the track's edge and "100%" lands off it. Same divergence the
        // player's seek bar had.
        track->setShrink(0.0f);
        track->setCornerRadius(4.0f);
        track->setMarginTop(18.0f);
        track->setBackgroundColor(nvgRGBA(255, 255, 255, 38));
        auto* fill = new brls::Box();
        fill->setHeight(8.0f);
        fill->setWidth(0.0f);
        fill->setCornerRadius(4.0f);
        fill->setBackgroundColor(brls::Application::getTheme().getColor("brls/accent"));
        track->addView(fill);
        box->addView(track);

        auto* prog = new brls::Dialog(box);
        prog->setCancelable(false);
        prog->open();

        downloadAsync(
            r,
            [label, track, fill](float f) {
                if (f < 0)
                {
                    label->setText("Downloading...");  // server gave no size
                    return;
                }
                if (f > 1.0f) f = 1.0f;  // a server lying about its size
                int pct = (int)(f * 100);
                label->setText("Downloading... " + std::to_string(pct) + "%");
                // Pixels off the track as laid out, not a percentage of what we
                // asked for: the two are not the same number (see setShrink).
                float w = track->getWidth();
                if (w > 0.0f) fill->setWidth(w * f);
            },
            [prog](std::string err) {
                prog->close([err]() {
                    if (!err.empty())
                    {
                        note("Update failed: " + err);
                        return;
                    }
                    // The .nro on disk is only swapped once we let go of it, so
                    // the new version cannot start until this one stops. Restart
                    // rather than leave the user on a build that is already
                    // obsolete, with an update they will forget they installed.
                    auto* d = new brls::Dialog(
                        "Update installed. The app will restart to use it.");
                    d->setCancelable(false);
                    d->addButton("Restart", []() {
                        if (!restartNow())
                            note("Close and reopen the app to use the new "
                                 "version.");
                    });
                    d->open();
                });
            });
    });
    ask->open();
}

bool restartNow()
{
    if (selfNro.empty() || !envHasNextLoad())
    {
        brls::Logger::warning("[update] loader cannot relaunch us");
        return false;
    }
    // Queue OURSELVES as the next thing to run. main() swaps the .nro in on the
    // way out (applyPending), after which hbloader loads this same path -- and
    // gets the new build.
    envSetNextLoad(selfNro.c_str(), selfNro.c_str());
    brls::Application::quit();
    return true;
}

void applyPending()
{
    std::string src = pendingPath();
    if (src.empty() || !hasPending()) return;

    // The .nro is still open at this point, and that is the whole difficulty:
    // userAppInit() mounts our romfs FROM the running .nro and userAppExit()
    // only unmounts it after main() returns -- so a rename here failed silently
    // and left the download sitting next to an unchanged app. Close it first.
    // The second romfsExit() in userAppExit is a no-op that returns an error.
    // Nothing may read romfs after this line; applyPending is the last thing
    // main does.
    romfsExit();

    // Keep the old one until the new is in place: a failed rename mid-way would
    // otherwise leave no .nro at all, and the user with nothing to launch.
    std::string bak = selfNro + ".old";
    std::remove(bak.c_str());

    bool moved = std::rename(selfNro.c_str(), bak.c_str()) == 0;
    if (!moved)
        brls::Logger::warning("[update] could not move the old .nro aside: {}",
                              std::strerror(errno));

    if (std::rename(src.c_str(), selfNro.c_str()) == 0)
    {
        std::remove(bak.c_str());
        brls::Logger::info("[update] installed, active on next launch");
        return;
    }

    brls::Logger::error("[update] could not install {}: {}", src,
                        std::strerror(errno));
    if (moved) std::rename(bak.c_str(), selfNro.c_str());  // put it back
}

} // namespace update
