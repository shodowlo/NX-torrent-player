#include "settings.hpp"

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/button.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>
#include <borealis/views/cells/cell_bool.hpp>
#include <borealis/views/cells/cell_selector.hpp>

#include <cstdio>

#include "config.hpp"
#include "update.hpp"
#include "stremio.hpp"

extern "C" {
#include "torrentfs.h"
}

namespace
{

void note(const std::string& msg)
{
    auto* d = new brls::Dialog(msg);
    d->addButton("OK", []() {});
    d->open();
}

} // namespace

brls::View* SettingsActivity::createContentView()
{
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setGrow(1.0f);
    // No side padding on the box: the ScrollingFrame pins its indicator to its
    // own right edge, so any right padding here dragged the bar inward over the
    // cells. The frame reaches the screen edge; the screen inset lives on the
    // list's padding instead (which survives setContentView, unlike a margin).
    box->setPadding(10.0f, 0.0f, 0.0f, 0.0f);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    auto* list = new brls::Box();
    list->setAxis(brls::Axis::COLUMN);
    list->setPaddingLeft(60.0f);
    list->setPaddingRight(40.0f);
    // Breathing room so the last cell does not sit against the footer bar.
    list->setPaddingBottom(40.0f);

    config::Config& cfg = config::get();

    // ---- general ---------------------------------------------------------
    auto* general = new brls::Header();
    general->setTitle("General");
    list->addView(general);

    auto* startup = new brls::SelectorCell();
    startup->init("Category on startup", { "Local", "Stremio" },
                  cfg.startupTab == config::Tab::STREMIO ? 1 : 0, [](int sel) {
                      config::get().startupTab =
                          sel == 1 ? config::Tab::STREMIO : config::Tab::LOCAL;
                      config::save();
                  });
    list->addView(startup);

    auto* tabBar = new brls::SelectorCell();
    tabBar->init("Category bar", { "Left", "Top" },
                 cfg.tabBar == config::TabBar::TOP ? 1 : 0, [](int sel) {
                     config::get().tabBar = sel == 1 ? config::TabBar::TOP
                                                     : config::TabBar::LEFT;
                     config::save();
                 });
    list->addView(tabBar);

    auto* barHint = new brls::Label();
    barHint->setText("Restart the app to apply.");
    barHint->setFontSize(15.0f);
    barHint->setTextColor(nvgRGB(150, 150, 155));
    barHint->setMargins(12.0f, 20.0f, 18.0f, 20.0f);
    list->addView(barHint);

    auto* logging = new brls::BooleanCell();
    logging->init("Log file", cfg.logging, [](bool on) {
        config::get().logging = on;
        config::save();
    });
    list->addView(logging);

    auto* logHint = new brls::Label();
    logHint->setText(
        "The log is written to the SD card continuously. Turn it on to diagnose a "
        "problem, then restart the app.");
    logHint->setFontSize(15.0f);
    logHint->setTextColor(nvgRGB(150, 150, 155));
    // Belongs to the toggle above, but needs room to read as a caption rather
    // than a squashed second line of it.
    logHint->setMargins(12.0f, 20.0f, 18.0f, 20.0f);
    logHint->setLineHeight(1.4f);
    list->addView(logHint);

    // ---- playback --------------------------------------------------------
    auto* playHdr = new brls::Header();
    playHdr->setTitle("Playback");
    list->addView(playHdr);

    // Index of a stored code in the offered list; "auto" (0) if we do not know
    // it -- a config.json edited by hand can say anything.
    auto langIndex = [](const std::string& code) {
        const auto& codes = config::langCodes();
        for (size_t i = 0; i < codes.size(); i++)
            if (codes[i] == code) return (int)i;
        return 0;
    };

    auto* alang = new brls::SelectorCell();
    alang->init("Audio language", config::langLabels(),
                langIndex(cfg.audioLang), [](int sel) {
                    config::get().audioLang = config::langCodes()[sel];
                    config::save();
                });
    list->addView(alang);

    auto* slang = new brls::SelectorCell();
    slang->init("Subtitle language", config::langLabels(),
                langIndex(cfg.subLang), [](int sel) {
                    config::get().subLang = config::langCodes()[sel];
                    config::save();
                });
    list->addView(slang);

    auto* subs = new brls::BooleanCell();
    subs->init("Subtitles", cfg.subtitles, [](bool on) {
        config::get().subtitles = on;
        config::save();
    });
    list->addView(subs);

    auto* langHint = new brls::Label();
    langHint->setText(
        "Applies to the next video. A track in that language is picked when the "
        "file has one; otherwise it falls back to the file's default. Console "
        "language is currently " + config::consoleLang() + ".");
    langHint->setFontSize(15.0f);
    langHint->setTextColor(nvgRGB(150, 150, 155));
    langHint->setMargins(12.0f, 20.0f, 18.0f, 20.0f);
    langHint->setLineHeight(1.4f);
    list->addView(langHint);

    auto* governor = new brls::BooleanCell();
    governor->init("Limit download rate", cfg.rateGovernor, [](bool on) {
        config::get().rateGovernor = on;
        config::save();
        // Takes effect immediately, even for a stream already playing.
        torrentfs_set_governor(on ? 1 : 0);
    });
    list->addView(governor);

    auto* governorHint = new brls::Label();
    governorHint->setText(
        "Once the playback buffer is comfortably ahead, cap the download speed "
        "instead of bursting at full speed — the bursts overload the console's "
        "network core and can stutter the system. Off by default, so downloads "
        "run full speed. Streams with less than 10 s of buffer are never "
        "limited.");
    governorHint->setFontSize(15.0f);
    governorHint->setTextColor(nvgRGB(150, 150, 155));
    governorHint->setMargins(12.0f, 20.0f, 18.0f, 20.0f);
    governorHint->setLineHeight(1.4f);
    list->addView(governorHint);

    auto* ramStream = new brls::BooleanCell();
    ramStream->init("Stream to RAM (no SD cache)", cfg.ramStream, [](bool on) {
        config::get().ramStream = on;
        config::save();
        // Latched when the engine opens, so it takes effect on the next video.
    });
    list->addView(ramStream);

    auto* ramHint = new brls::Label();
    ramHint->setText(
        "Keep downloaded pieces in memory instead of writing them to the SD "
        "card. Removes the brief stutter every time a piece finishes (the SD "
        "write hammers the system core, worse for bigger pieces), at the cost "
        "of no resume and a limited seek-back range. Applies to the next video.");
    ramHint->setFontSize(15.0f);
    ramHint->setTextColor(nvgRGB(150, 150, 155));
    ramHint->setMargins(12.0f, 20.0f, 18.0f, 20.0f);
    ramHint->setLineHeight(1.4f);
    list->addView(ramHint);

    // ---- stremio ---------------------------------------------------------
    auto* stremioHdr = new brls::Header();
    stremioHdr->setTitle("Stremio");
    list->addView(stremioHdr);

    auto* hide4k = new brls::BooleanCell();
    hide4k->init("Hide 4K sources", cfg.hide4k, [](bool on) {
        config::get().hide4k = on;
        config::save();
    });
    list->addView(hide4k);

    // Poster cache: size + a Clear button. Clearing marks the library stale so
    // the artwork is re-fetched next time the tab is focused.
    auto humanMB = [](int64_t b) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
        return std::string(buf);
    };
    auto* cacheLbl = new brls::Label();
    cacheLbl->setText("Poster cache: " + humanMB(stremio::posterCacheBytes()));
    cacheLbl->setFontSize(16.0f);
    cacheLbl->setTextColor(nvgRGB(190, 190, 195));
    cacheLbl->setMargins(12.0f, 20.0f, 4.0f, 20.0f);
    list->addView(cacheLbl);

    auto* clearCache = new brls::Button();
    clearCache->setText("Clear poster cache");
    clearCache->setMargins(4.0f, 20.0f, 18.0f, 20.0f);
    clearCache->registerClickAction([cacheLbl, humanMB](brls::View*) {
        stremio::clearPosterCache();
        stremio::markLibraryStale();  // library reloads on return
        cacheLbl->setText("Poster cache: " + humanMB(stremio::posterCacheBytes()));
        note("Poster cache cleared. The library reloads when you go back to it.");
        return true;
    });
    list->addView(clearCache);

    // Account (belongs in the Stremio section).
    std::string key = stremio::loadAuthKey();
    if (!key.empty())
    {
        std::string who = stremio::loadEmail();
        auto* account   = new brls::Label();
        account->setText(who.empty() ? "Logged in" : "Logged in as: " + who);
        account->setFontSize(17.0f);
        account->setTextColor(nvgRGB(190, 190, 195));
        account->setMargins(12.0f, 20.0f, 4.0f, 20.0f);
        list->addView(account);

        auto* logout = new brls::Button();
        logout->setText("Sign out of Stremio");
        logout->setMargins(16.0f, 20.0f, 0.0f, 20.0f);
        logout->registerClickAction([logout](brls::View*) {
            stremio::clearAuthKey();
            // The addon collection belongs to that account.
            stremio::clearAddonCache();
            // The tab holds the key in memory and is only rebuilt when it is
            // re-entered, so say what actually has to happen.
            note("Signed out. Restart the app to get back to the sign-in "
                 "screen.");
            logout->setState(brls::ButtonState::DISABLED);
            return true;
        });
        list->addView(logout);
    }
    else
    {
        auto* signedOut = new brls::Label();
        signedOut->setText("No account signed in.");
        signedOut->setFontSize(16.0f);
        signedOut->setTextColor(nvgRGB(150, 150, 155));
        signedOut->setMargins(10.0f, 20.0f, 0.0f, 20.0f);
        list->addView(signedOut);
    }

    // ---- updates ---------------------------------------------------------
    auto* updHdr = new brls::Header();
    updHdr->setTitle("Updates");
    list->addView(updHdr);

    auto* checkUpd = new brls::BooleanCell();
    checkUpd->init("Check on startup", cfg.checkUpdates, [](bool on) {
        config::get().checkUpdates = on;
        config::save();
    });
    list->addView(checkUpd);

    auto* version = new brls::Label();
    version->setText(update::hasPending()
                         ? "Version " APP_VERSION
                           " — an update is installed, restart to use it"
                         : "Version " APP_VERSION);
    version->setFontSize(15.0f);
    version->setTextColor(nvgRGB(150, 150, 155));
    version->setMargins(12.0f, 20.0f, 8.0f, 20.0f);
    list->addView(version);

    auto* checkNow = new brls::Button();
    checkNow->setText("Check now");
    checkNow->setMargins(4.0f, 20.0f, 18.0f, 20.0f);
    checkNow->registerClickAction([checkNow](brls::View*) {
        checkNow->setState(brls::ButtonState::DISABLED);
        checkNow->setText("Checking...");
        update::checkAsync([checkNow](update::Release r) {
            checkNow->setState(brls::ButtonState::ENABLED);
            checkNow->setText("Check now");
            // Asked for explicitly, so unlike the startup check this one says
            // something either way.
            if (!r.ok)
                note("Could not check for updates: " + r.error);
            else if (!r.newer)
                note("You are on the latest version (" APP_VERSION ").");
            else if (r.url.empty())
                // Newer, but nothing we can install: say so rather than claim
                // this is the latest.
                note("Version " + r.version +
                     " is out, but that release has no .nro to install. "
                     "Get it from GitHub.");
            else
                update::promptInstall(r);
        });
        return true;
    });
    list->addView(checkNow);

    scroll->setContentView(list);
    box->addView(scroll);

    auto* frame = new brls::AppletFrame();
    frame->pushContentView(box);
    // After pushContentView: it overwrites the title with the content view's.
    frame->setTitle("Options");
    return frame;
}
