#include "update.hpp"

#include "theme.hpp"

#include <borealis.hpp>

#include <switch.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "http.hpp"
#include "json.hpp"
#include "i18n.hpp"

#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
#ifndef UPDATE_API_URL
#define UPDATE_API_URL "https://api.github.com/repos/shodowlo/NX-torrent-player/releases/latest"
#endif
// Base for a specific tag's release; the tag (e.g. "v0.2.0") is appended.
#ifndef UPDATE_TAG_URL
#define UPDATE_TAG_URL "https://api.github.com/repos/shodowlo/NX-torrent-player/releases/tags/"
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
        r.error = tr("unknown install path");
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
            if (r.error.empty()) r.error = tr("no release found");
            brls::Logger::info("[update] no tag_name: {}", r.error);
            brls::sync([done, r]() { done(r); });
            return;
        }

        r.ok      = true;
        r.version = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
        r.notes   = json::str(resp, "body");
        // The release's own name heads the changelog; fall back to the tag when
        // GitHub left it unnamed.
        r.title = json::str(resp, "name");
        if (r.title.empty()) r.title = tag;

        // The .nro asset. The release also carries the zip for manual installs;
        // downloading that would mean pulling the same bytes to then unpack one
        // file out of them. A release ships one .nro per console, so match this
        // build's exact name (UPDATE_ASSET, from CMake) -- "any .nro" would hand
        // a Switch 2 the Switch 1 binary, which does not run there. No match
        // means no update, never a guess.
        for (const auto& a : json::objects(resp, "assets"))
        {
            if (json::str(a, "name") != UPDATE_ASSET) continue;
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
                "[update] release {} is newer but ships no {} asset", tag,
                UPDATE_ASSET);

        brls::sync([done, r]() { done(r); });
    });
}

void fetchNotesAsync(
    const std::string& version,
    std::function<void(bool, std::string, std::string, std::string)> done)
{
    // A point release's changelog reads better with the earlier patches of the
    // same minor under it: 0.1.2 shows 0.1.2, then 0.1.1, then 0.1.0. So fetch
    // every tag from this patch down to x.y.0 and stitch them into one document.
    // A x.y.0 version is just itself.
    int v[3];
    parseVersion(version, v);

    brls::async([v0 = v[0], v1 = v[1], top = v[2], done]() {
        std::string doc, title, firstErr;
        bool any = false;
        for (int p = top; p >= 0; p--)
        {
            char tagbuf[32];
            std::snprintf(tagbuf, sizeof(tagbuf), "v%d.%d.%d", v0, v1, p);
            std::string tag = tagbuf;
            std::string url = std::string(UPDATE_TAG_URL) + tag;

            std::string resp, err;
            if (!http::get(url, resp, err))
            {
                if (firstErr.empty()) firstErr = err;
                brls::Logger::info("[update] changelog {} fetch failed: {}", tag,
                                   err);
                continue;
            }
            std::string body = json::str(resp, "body");
            if (body.empty())
                continue;  // no release published for this patch -- skip it

            std::string name = json::str(resp, "name");
            if (name.empty()) name = tag;

            if (!any)
            {
                // The newest one heads the page (its name becomes the title) and
                // needs no in-body header of its own.
                title = name;
                doc   = body;
                any   = true;
            }
            else
            {
                // Separate each older patch with its own name as a section head.
                doc += "\n\n\n" + name + "\n\n" + body;
            }
        }

        if (!any)
        {
            std::string m =
                firstErr.empty() ? tr("no changelog for this version") : firstErr;
            brls::sync([done, m]() { done(false, "", "", m); });
            return;
        }
        brls::sync([done, title, doc]() { done(true, title, doc, ""); });
    });
}

namespace
{

// A full-screen, scrollable page for the release body. A modal Dialog cannot hold
// a long changelog (it neither scrolls nor bounds its height), so this is its own
// pushed activity: the ScrollingFrame is focusable and, in NATURAL mode, scrolls
// plain text with the stick or a swipe -- no focusable rows needed.
class ChangelogActivity : public brls::Activity
{
  public:
    ChangelogActivity(std::string title, std::string notes)
        : title(std::move(title)), notes(std::move(notes))
    {
    }

    brls::View* createContentView() override
    {
        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1.0f);
        scroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);

        // setContentView forces the content's width and drops its margins, so the
        // reading inset lives on this padded wrapper rather than on the label.
        auto* pad = new brls::Box();
        pad->setAxis(brls::Axis::COLUMN);
        pad->setPadding(30.0f, 60.0f, 50.0f, 60.0f);  // top,right,bottom,left

        auto* label = new brls::Label();
        label->setSingleLine(false);  // honour the body's newlines and wrap
        label->setText(notes);
        label->setFontSize(18.0f);
        label->setLineHeight(1.4f);
        pad->addView(label);

        scroll->setContentView(pad);

        auto* frame = new brls::AppletFrame();
        frame->pushContentView(scroll);
        frame->setTitle(tr("Changelog — ") + title);
        return frame;
    }

  private:
    std::string title;
    std::string notes;
};

} // namespace

void showNotes(const std::string& title, const std::string& notes)
{
    brls::Application::pushActivity(new ChangelogActivity(title, notes));
}

void downloadAsync(const Release& r, std::function<void(float)> progress,
                   std::function<void(std::string)> done)
{
    std::string url  = r.url;
    std::string dest = pendingPath();
    if (dest.empty() || url.empty())
    {
        brls::sync([done]() { done(tr("nothing to download")); });
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

void showChangelog(const std::string& version)
{
    // Block everything while it loads. A GitHub round trip can take a second or
    // two, and without this the button underneath stays live: the user could tap
    // it again (stacking changelog pages), act on the update prompt, or leave the
    // screen out from under the pending callback. A non-cancelable, button-less
    // dialog swallows all input -- B included -- until the fetch resolves.
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setPadding(28.0f, 44.0f, 28.0f, 44.0f);
    // A modal needs a focus target, but this one is not meant to look focused.
    box->setFocusable(true);
    box->setHideHighlight(true);

    auto* sp = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
    sp->setDimensions(52.0f, 52.0f);
    sp->animate(true);
    box->addView(sp);

    auto* label = new brls::Label();
    label->setText(tr("Loading changelog..."));
    label->setFontSize(19.0f);
    label->setMarginTop(20.0f);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    box->addView(label);

    auto* loading = new brls::Dialog(box);
    loading->setCancelable(false);
    loading->open();

    fetchNotesAsync(version, [loading](bool ok, std::string title,
                                       std::string notes, std::string err) {
        // Tear the blocker down first, then show the result on top of whatever
        // it was covering (the Options list, or the update prompt).
        loading->close([ok, title, notes, err]() {
            if (!ok)
                note(tr("Could not load the changelog: ") + err);
            else
                showNotes(title, notes);
        });
    });
}

void promptInstall(const Release& r)
{
    // Just the release's own title -- it already reads "0.2.0 — <overview>". The
    // full body lives one tap away under "View changelog"; a modal is the wrong
    // place for pages of markdown (and it does not scroll).
    std::string msg = tr("Version ") + r.version + tr(" is available.");
    if (r.size > 0) msg += "  (" + humanMB(r.size) + ")";
    if (!r.title.empty()) msg += "\n\n" + r.title;

    auto* ask = new brls::Dialog(msg);
    ask->addButton(tr("Later"), []() {});
    ask->addButton(tr("View changelog"), [r]() {
        // A dialog button dismisses the prompt before its callback runs, so
        // re-open the prompt first: it then waits UNDER the changelog, and backing
        // out returns the user to it to choose Update / Later rather than dropping
        // them back in the app. showChangelog puts its own blocking spinner over
        // this re-opened prompt while it fetches.
        promptInstall(r);
        showChangelog(r.version);
    });
    ask->addButton(tr("Update"), [r]() {
        // Its own dialog, with a label we keep writing into. Not cancelable:
        // there is no partial file to leave behind (http::download cleans up),
        // but B-ing out mid-transfer would leave the callbacks writing to a
        // freed label.
        auto* box = new brls::Box();
        box->setAxis(brls::Axis::COLUMN);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->setPadding(24.0f, 30.0f, 24.0f, 30.0f);

        auto* label = new brls::Label();
        label->setText(tr("Downloading... 0%"));
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
        track->setBackgroundColor(theme::scrim(38));
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
                    label->setText(tr("Downloading..."));  // server gave no size
                    return;
                }
                if (f > 1.0f) f = 1.0f;  // a server lying about its size
                int pct = (int)(f * 100);
                label->setText(tr("Downloading... ") + std::to_string(pct) + "%");
                // Pixels off the track as laid out, not a percentage of what we
                // asked for: the two are not the same number (see setShrink).
                float w = track->getWidth();
                if (w > 0.0f) fill->setWidth(w * f);
            },
            [prog](std::string err) {
                prog->close([err]() {
                    if (!err.empty())
                    {
                        note(tr("Update failed: ") + err);
                        return;
                    }
                    // The .nro on disk is only swapped once we let go of it, so
                    // the new version cannot start until this one stops. Restart
                    // rather than leave the user on a build that is already
                    // obsolete, with an update they will forget they installed.
                    auto* d = new brls::Dialog(
                        tr("Update installed. The app will restart to use it."));
                    d->setCancelable(false);
                    d->addButton(tr("Restart"), []() {
                        if (!restartNow())
                            note(tr("Close and reopen the app to use the new "
                                 "version."));
                    });
                    d->open();
                });
            });
    });

    // Default the highlight to Update, not Later: the dialog parks focus on the
    // first button (button1 = Later) as each one is added, so override it once
    // all three are in. Update is button3 (added last).
    if (brls::View* up = ask->getView("brls/dialog/button3"))
        ask->setLastFocusedView(up);

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
