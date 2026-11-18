#include "settings.hpp"

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/button.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/header.hpp>
#include <borealis/views/label.hpp>
#include <borealis/views/scrolling_frame.hpp>
#include <borealis/views/tab_frame.hpp>
#include <borealis/views/cells/cell_bool.hpp>
#include <borealis/views/cells/cell_selector.hpp>

#include <cctype>
#include <cstdio>

#include <switch.h>  // appletGetOperationMode (isDocked)

#include "config.hpp"
#include "theme.hpp"
#include "update.hpp"
#include "stremio.hpp"

extern "C" {
#include "torrentfs.h"
}

#include "i18n.hpp"

namespace
{

void note(const std::string& msg)
{
    auto* d = new brls::Dialog(msg);
    d->addButton("OK", []() {});
    d->open();
}

std::function<void()> uiScaleHook;

// A Label an outstanding background job can safely give up on: the Options
// screen is popped long before a slow SD-card scan of the poster cache is
// necessarily over, and the callback would write into a freed view.
class AsyncLabel : public brls::Label
{
  public:
    ~AsyncLabel() override { *alive = false; }
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

// The same guard for a Box that a background job fills with rows -- the Account
// screen's addon list, whose fetch can land after B has popped it.
class AsyncBox : public brls::Box
{
  public:
    ~AsyncBox() override { *alive = false; }
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

brls::View* generalPane();
brls::View* playbackPane();
brls::View* streamingPane();
brls::View* stremioPane();
brls::View* aboutPane();

// ---------------------------------------------------------------------------
// The panes. One per sidebar tab, each built fresh when its tab is entered
// (TabFrame frees the previous one), so they read config at build time and
// share nothing.

// The scrolling column a pane is. Returns the frame to hand to the tab; `list`
// comes back as the box to fill.
brls::ScrollingFrame* newPane(brls::Box** list)
{
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    // CENTERED, not the default NATURAL: NATURAL's pixel free-scroll hands focus
    // to the frame and re-grabs the topmost visible cell as you cross a page,
    // which intermittently wedged the cursor and refused to go further.
    // CENTERED moves focus one cell at a time and scrolls to keep it in view.
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    // No margin: setContentView detaches the box and forces its width, so only
    // padding survives. Right padding stays clear of the scrolling indicator,
    // which the frame pins to its own edge.
    box->setPadding(16.0f, 40.0f, 40.0f, 40.0f);
    scroll->setContentView(box);

    *list = box;
    return scroll;
}

// A caption under the cell it belongs to.
brls::Label* caption(const std::string& text)
{
    auto* l = new brls::Label();
    l->setText(text);
    l->setFontSize(15.0f);
    l->setTextColor(theme::textMuted());
    l->setLineHeight(1.4f);
    l->setMargins(10.0f, 20.0f, 18.0f, 20.0f);
    return l;
}

// Index of the stored width in the offered list. load() rejects anything it
// does not offer, so the fallback only matters if the table is ever changed
// without the defaults following it.
int uiIndex(int width)
{
    const auto& ws = config::uiWidths();
    for (size_t i = 0; i < ws.size(); i++)
        if (ws[i] == width) return (int)i;
    return 0;
}

// The list is shared between the two modes, but they do not default to the same
// size -- so mark the default in each cell rather than in the table.
std::vector<std::string> uiLabels(int dflt)
{
    std::vector<std::string> v = config::uiWidthLabels();
    const auto& ws             = config::uiWidths();
    for (size_t i = 0; i < ws.size(); i++)
        if (ws[i] == dflt) v[i] += tr(" (default)");
    return v;
}

// Index of a stored language code in the offered list; "auto" (0) if we do not
// know it -- a config.json edited by hand can say anything.
int langIndex(const std::string& code)
{
    const auto& codes = config::langCodes();
    for (size_t i = 0; i < codes.size(); i++)
        if (codes[i] == code) return (int)i;
    return 0;
}

brls::View* generalPane()
{
    brls::Box* list = nullptr;
    auto* pane      = newPane(&list);
    config::Config& cfg = config::get();

    auto* startup = new brls::SelectorCell();
    startup->init(tr("Category on startup"), { tr("Local"), tr("Stremio") },
                  cfg.startupTab == config::Tab::STREMIO ? 1 : 0, [](int sel) {
                      config::get().startupTab =
                          sel == 1 ? config::Tab::STREMIO : config::Tab::LOCAL;
                      config::save();
                  });
    list->addView(startup);

    // Same startup-only story as the theme below, for the same reason plus one
    // of its own: a view reads its strings when it is built, and the header,
    // the tab bar and the browser behind this screen were built once at launch.
    auto* language = new brls::SelectorCell();
    language->init(
        tr("Language"), i18n::langLabels(),
        [] {
            const auto& ids = i18n::langIds();
            for (size_t i = 0; i < ids.size(); i++)
                if (ids[i] == config::get().language) return (int)i;
            return 0;
        }(),
        [](int sel) {
            config::get().language = i18n::langIds()[(size_t)sel];
            config::save();
        });
    list->addView(language);
    list->addView(caption(tr("The language applies when you restart the app.")));

    // Startup-only, unlike every other cell here: borealis states the variant is
    // not expected to change while the app runs, and the views already on screen
    // copied their colours out of the theme when they were built. Applying it
    // live would leave the UI half converted, so say so instead.
    static const std::vector<std::string> kVariantIds = { "dark", "light",
                                                          "system" };
    auto* variant = new brls::SelectorCell();
    variant->init(
        tr("Theme"), { tr("Dark (default)"), tr("Light"), tr("Follow the console") },
        [] {
            for (size_t i = 0; i < kVariantIds.size(); i++)
                if (kVariantIds[i] == config::get().themeVariant) return (int)i;
            return 0;
        }(),
        [](int sel) {
            config::get().themeVariant = kVariantIds[sel];
            config::save();
            // Nothing to apply: theme::applyVariant() latched the variant at
            // startup and is the only reader of this setting.
        });
    list->addView(variant);
    list->addView(caption(tr("The theme applies when you restart the app.")));

    auto* accent = new brls::SelectorCell();
    accent->init(
        tr("Accent colour"),
        [] {
            std::vector<std::string> v = theme::schemeLabels();
            if (!v.empty()) v[0] += tr(" (default)");  // schemes() is default-first
            return v;
        }(),
        [] {
            const auto& ids = theme::schemeIds();
            for (size_t i = 0; i < ids.size(); i++)
                if (ids[i] == config::get().accent) return (int)i;
            return 0;
        }(),
        [](int sel) {
            config::get().accent = theme::schemeIds()[sel];
            config::save();
            // The backgrounds follow on the next frame by themselves (they read
            // the scheme as they draw); this repaints the theme colours that
            // views copy out of it.
            theme::applyAccent();
        });
    list->addView(accent);

    // Index-matched with the cell's labels. The library rows are built once and
    // kept for the life of the tab, so marking it stale is what re-renders them
    // -- on the next return to the list.
    static const std::vector<std::string> kListStyles = { "posters", "cards",
                                                          "classic" };
    auto* listStyle = new brls::SelectorCell();
    listStyle->init(tr("List style"), { tr("Posters (default)"), tr("Cards"), tr("Classic") },
                    [&] {
                        for (size_t i = 0; i < kListStyles.size(); i++)
                            if (kListStyles[i] == cfg.listStyle) return (int)i;
                        return 0;
                    }(),
                    [](int sel) {
                        config::get().listStyle = kListStyles[sel];
                        config::save();
                        stremio::markLibraryStale();
                    });
    list->addView(listStyle);

    auto* sizeHdr = new brls::Header();
    sizeHdr->setTitle(tr("UI size"));
    list->addView(sizeHdr);

    auto* dockedUi = new brls::SelectorCell();
    dockedUi->init(tr("Docked"), uiLabels(config::kDefaultDockedUiWidth),
                   uiIndex(config::get().dockedUiWidth), [](int sel) {
                       config::get().dockedUiWidth = config::uiWidths()[sel];
                       config::save();
                       // Next frame, not now: the selector's dropdown is still
                       // closing, and relaying out the activity stack from under
                       // it is asking for trouble.
                       brls::sync([] { applyUiScale(); });
                   });
    list->addView(dockedUi);

    auto* handheldUi = new brls::SelectorCell();
    handheldUi->init(tr("Handheld"), uiLabels(config::kDefaultHandheldUiWidth),
                     uiIndex(config::get().handheldUiWidth), [](int sel) {
                         config::get().handheldUiWidth = config::uiWidths()[sel];
                         config::save();
                         brls::sync([] { applyUiScale(); });
                     });
    list->addView(handheldUi);

    return pane;
}

brls::View* playbackPane()
{
    brls::Box* list = nullptr;
    auto* pane      = newPane(&list);
    config::Config& cfg = config::get();

    auto* alang = new brls::SelectorCell();
    alang->init(tr("Audio language"), config::langLabels(),
                langIndex(cfg.audioLang), [](int sel) {
                    config::get().audioLang = config::langCodes()[sel];
                    config::save();
                });
    list->addView(alang);

    auto* slang = new brls::SelectorCell();
    slang->init(tr("Subtitle language"), config::langLabels(),
                langIndex(cfg.subLang), [](int sel) {
                    config::get().subLang = config::langCodes()[sel];
                    config::save();
                });
    list->addView(slang);

    auto* subs = new brls::BooleanCell();
    subs->init(tr("Subtitles"), cfg.subtitles, [](bool on) {
        config::get().subtitles = on;
        config::save();
    });
    list->addView(subs);

    auto* audioBoost = new brls::BooleanCell();
    audioBoost->init(tr("Boost quiet audio in handheld"), cfg.audioBoost, [](bool on) {
        config::get().audioBoost = on;
        config::save();
        // The player re-reads this every frame, so it reaches a video that is
        // already playing as well as the next one.
    });
    list->addView(audioBoost);
    list->addView(caption(
        tr("On by default. Lifts a 5.1 mix folded down to stereo, which plays "
        "much quieter.")));

    auto* hwdec = new brls::BooleanCell();
    hwdec->init(tr("Hardware decoding"), cfg.hwDecode, [](bool on) {
        config::get().hwDecode = on;
        config::save();
    });
    list->addView(hwdec);
    list->addView(caption(
        tr("On by default. Off decodes in software: slower, may stutter on 1080p.")));

    return pane;
}

brls::View* streamingPane()
{
    brls::Box* list = nullptr;
    auto* pane      = newPane(&list);
    config::Config& cfg = config::get();

    // A SelectorCell rather than a BooleanCell for the two settings whose value
    // needs saying more than "On"/"Off": BooleanCell hardcodes those two strings
    // in a private, non-virtual updateUI() that also re-runs 200ms after every
    // toggle (the end of its scale animation), so its text cannot be overridden
    // from here.
    auto* ramStream = new brls::SelectorCell();
    ramStream->init(tr("Stream to RAM (no SD cache)"),
                    { tr("Off (Not recommended)"), tr("On (Recommended)") },
                    cfg.ramStream ? 1 : 0, [](int sel) {
                        config::get().ramStream = sel == 1;
                        config::save();
                        // Latched when the engine opens, so it takes effect on
                        // the next video.
                    });
    list->addView(ramStream);
    list->addView(caption(
        tr("Keep pieces in memory instead of writing them to the SD card. Removes "
        "the stutter on each finished piece, at the cost of no resume and a "
        "limited seek-back range.")));

    auto* governor = new brls::SelectorCell();
    governor->init(tr("Limit download rate"), { tr("Off (default)"), tr("On") },
                   cfg.rateGovernor ? 1 : 0, [](int sel) {
                       config::get().rateGovernor = sel == 1;
                       config::save();
                       // Takes effect immediately, even for a stream already
                       // playing.
                       torrentfs_set_governor(sel == 1 ? 1 : 0);
                   });
    list->addView(governor);
    list->addView(caption(
        tr("Once the buffer is comfortably ahead, cap the download speed instead "
        "of bursting \xE2\x80\x94 the bursts can stutter the system. Off by default.")));

    return pane;
}

brls::View* stremioPane()
{
    brls::Box* list = nullptr;
    auto* pane      = newPane(&list);
    config::Config& cfg = config::get();

    auto* hide4k = new brls::BooleanCell();
    hide4k->init(tr("Hide 4K sources"), cfg.hide4k, [](bool on) {
        config::get().hide4k = on;
        config::save();
    });
    list->addView(hide4k);

    // The account itself is not here: it has its own screen, off the header's
    // profile button (see AccountActivity).

    auto* cacheHdr = new brls::Header();
    cacheHdr->setTitle(tr("Poster cache"));
    list->addView(cacheHdr);

    auto humanMB = [](int64_t b) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
        return std::string(buf);
    };
    auto* cacheLbl = new AsyncLabel();
    cacheLbl->setText(tr("Reading..."));
    cacheLbl->setFontSize(16.0f);
    cacheLbl->setTextColor(theme::textDim());
    cacheLbl->setMargins(10.0f, 20.0f, 6.0f, 20.0f);
    list->addView(cacheLbl);

    // Off the UI thread: the size is a stat() per cached poster, and on a full
    // cache that SD-card walk is most of a second -- long enough that the pane
    // took visibly too long to appear, since nothing is drawn until this whole
    // function returns. The label fills itself in a moment later instead.
    auto readCacheSize = [cacheLbl, humanMB]() {
        auto live = cacheLbl->alive;
        brls::async([cacheLbl, live, humanMB]() {
            int64_t bytes = stremio::posterCacheBytes();
            brls::sync([cacheLbl, live, humanMB, bytes]() {
                if (*live) cacheLbl->setText(humanMB(bytes) + tr(" on the SD card"));
            });
        });
    };
    readCacheSize();

    auto* clearCache = new brls::Button();
    clearCache->setText(tr("Clear poster cache"));
    clearCache->setMargins(4.0f, 20.0f, 18.0f, 20.0f);
    clearCache->registerClickAction([cacheLbl, readCacheSize](brls::View*) {
        auto live = cacheLbl->alive;
        // Deleting them is the same walk, so it goes off the thread too --
        // otherwise the whole UI stops until the card is done.
        cacheLbl->setText(tr("Clearing..."));
        brls::async([live, readCacheSize]() {
            stremio::clearPosterCache();
            brls::sync([live, readCacheSize]() {
                stremio::markLibraryStale();  // library reloads on return
                note(tr("Poster cache cleared. The library reloads when you go "
                     "back to it."));
                if (*live) readCacheSize();
            });
        });
        return true;
    });
    list->addView(clearCache);

    return pane;
}

brls::View* aboutPane()
{
    brls::Box* list = nullptr;
    auto* pane      = newPane(&list);
    config::Config& cfg = config::get();

    auto* version = new brls::Label();
    version->setText(
        update::hasPending()
            ? tr("Version ") + std::string(APP_VERSION) +
                  tr(" \xE2\x80\x94 an update is installed, restart to use it")
            : tr("Version ") + std::string(APP_VERSION));
    version->setFontSize(20.0f);
    version->setTextColor(theme::text());
    version->setMargins(4.0f, 20.0f, 16.0f, 20.0f);
    list->addView(version);

    auto* checkUpd = new brls::BooleanCell();
    checkUpd->init(tr("Check for updates on startup"), cfg.checkUpdates, [](bool on) {
        config::get().checkUpdates = on;
        config::save();
    });
    list->addView(checkUpd);

    auto* changelog = new brls::Button();
    changelog->setText(tr("View changelog"));
    changelog->setMargins(14.0f, 20.0f, 8.0f, 20.0f);
    changelog->registerClickAction([](brls::View*) {
        // showChangelog raises its own blocking spinner while it loads, so the
        // user cannot re-press this or leave Options mid-fetch.
        update::showChangelog(APP_VERSION);
        return true;
    });
    list->addView(changelog);

    auto* checkNow = new brls::Button();
    checkNow->setText(tr("Check now"));
    checkNow->setMargins(4.0f, 20.0f, 18.0f, 20.0f);
    checkNow->registerClickAction([checkNow](brls::View*) {
        checkNow->setState(brls::ButtonState::DISABLED);
        checkNow->setText(tr("Checking..."));
        update::checkAsync([checkNow](update::Release r) {
            checkNow->setState(brls::ButtonState::ENABLED);
            checkNow->setText(tr("Check now"));
            // Asked for explicitly, so unlike the startup check this one says
            // something either way.
            if (!r.ok)
                note(tr("Could not check for updates: ") + r.error);
            else if (!r.newer)
                note(tr("You are on the latest version (") +
                     std::string(APP_VERSION) + ").");
            else if (r.url.empty())
                // Newer, but nothing we can install: say so rather than claim
                // this is the latest.
                note(tr("Version ") + r.version +
                     tr(" is out, but that release has no .nro to install. "
                        "Get it from GitHub."));
            else
                update::promptInstall(r);
        });
        return true;
    });
    list->addView(checkNow);

    auto* diagHdr = new brls::Header();
    diagHdr->setTitle(tr("Diagnostics"));
    list->addView(diagHdr);

    auto* logging = new brls::BooleanCell();
    logging->init(tr("Log file"), cfg.logging, [](bool on) {
        config::get().logging = on;
        config::save();
    });
    list->addView(logging);
    list->addView(caption(
        tr("The log is written to the SD card continuously. Turn it on to diagnose "
        "a problem, then restart the app.")));

    return pane;
}

}  // namespace

void setUiScaleHook(std::function<void()> fn) { uiScaleHook = std::move(fn); }

bool isDocked()
{
    return appletGetOperationMode() == AppletOperationMode_Console;
}

void applyUiScale()
{
    // borealis lays out in a fixed 1280x720 logical space and multiplies it by
    // windowScale to fill the output, so the UI keeps the same relative size
    // whatever it is drawn to -- docked it is simply 1.5x bigger, with no more
    // content on screen. Widening the logical space shrinks the UI instead; how
    // far is the user's call, separately per mode, since a 1080p TV metres away
    // and a 6" panel at arm's length do not want the same thing.
    bool docked = isDocked();
    uint32_t w  = (uint32_t)(docked ? config::get().dockedUiWidth
                                    : config::get().handheldUiWidth);
    uint32_t h  = w * 9 / 16;  // every offered width is 16:9, so this is exact

    if (brls::Application::ORIGINAL_WINDOW_WIDTH == w) return;

    brls::Application::ORIGINAL_WINDOW_WIDTH  = w;
    brls::Application::ORIGINAL_WINDOW_HEIGHT = h;

    // Recomputes windowScale/contentWidth and relayouts the activity stack.
    // Safe to call from the window-size-changed listener: that event is fired by
    // onWindowResized *after* its own setWindowSize call, and setWindowSize does
    // not fire it again -- so this is one extra layout pass, not a loop, and it
    // lands before the next frame is drawn.
    brls::Application::setWindowSize(brls::Application::windowWidth,
                                     brls::Application::windowHeight);

    // ... and because it does not fire it again, anything that keys off the UI
    // size has to be told here. Editing the setting in Options reaches the
    // header this way and no other.
    if (uiScaleHook) uiScaleHook();
}

brls::View* SettingsActivity::createContentView()
{
    // A sidebar with a pane per category, rather than the one long scroll this
    // used to be. Twenty-odd cells with a prose caption under half of them ran
    // to several screens, and the section headers went past too fast to serve
    // as landmarks -- you had to remember roughly how far down a setting was.
    // The sidebar names the five groups up front and each pane now fits in a
    // screen or close to it.
    //
    // TabFrame keeps only the visible tab alive: every pane below is rebuilt
    // from config each time it is entered, which is also why none of them can
    // hold a pointer into another.
    auto* tabs = new brls::TabFrame();

    tabs->addTab(tr("General"), [] { return generalPane(); });
    tabs->addTab(tr("Playback"), [] { return playbackPane(); });
    tabs->addTab(tr("Streaming"), [] { return streamingPane(); });
    tabs->addTab("Stremio", [] { return stremioPane(); });
    tabs->addTab(tr("About"), [] { return aboutPane(); });

    auto* frame = new brls::AppletFrame();
    frame->pushContentView(tabs);
    // After pushContentView: it overwrites the title with the content view's.
    frame->setTitle(tr("Options"));
    return frame;
}

brls::View* AccountActivity::createContentView()
{
    // Was a header, a line of text and a button. It is the screen behind the
    // header's profile button, so it should answer the questions you press that
    // button to ask: who is signed in, how much is on the account, and what the
    // addons -- which everything the Stremio side does depends on -- actually
    // provide.
    auto* root = new brls::Box();
    root->setAxis(brls::Axis::COLUMN);
    root->setGrow(1.0f);
    root->setPadding(24.0f, 0.0f, 0.0f, 0.0f);

    std::string key   = stremio::loadAuthKey();
    std::string email = stremio::loadEmail();

    if (key.empty())
    {
        auto* box = new brls::Box();
        box->setAxis(brls::Axis::COLUMN);
        box->setGrow(1.0f);
        box->setJustifyContent(brls::JustifyContent::CENTER);
        box->setAlignItems(brls::AlignItems::CENTER);

        auto* title = new brls::Label();
        title->setText(tr("Not signed in"));
        title->setFontSize(30.0f);
        title->setTextColor(theme::text());
        box->addView(title);

        auto* sub = new brls::Label();
        sub->setText(tr("The sign-in form is on the Stremio tab."));
        sub->setFontSize(18.0f);
        sub->setTextColor(theme::textMuted());
        sub->setMarginTop(10.0f);
        box->addView(sub);

        root->addView(box);

        auto* frame = new brls::AppletFrame();
        frame->pushContentView(root);
        frame->setTitle(tr("Account"));
        return frame;
    }

    // ---- identity ---------------------------------------------------------
    auto* head = new brls::Box();
    head->setAxis(brls::Axis::ROW);
    head->setAlignItems(brls::AlignItems::CENTER);
    head->setMargins(0.0f, 60.0f, 28.0f, 60.0f);

    // A monogram rather than an avatar: the API gives us no picture, and a
    // letter in the accent reads as an account at a glance where a generic
    // person icon would just be furniture.
    auto* mono = new brls::Box();
    mono->setWidth(84.0f);
    mono->setHeight(84.0f);
    mono->setCornerRadius(42.0f);
    mono->setBackgroundColor(theme::accent());
    mono->setJustifyContent(brls::JustifyContent::CENTER);
    mono->setAlignItems(brls::AlignItems::CENTER);
    mono->setShrink(0.0f);
    {
        auto* l = new brls::Label();
        std::string c = email.empty() ? "?" : std::string(1, email[0]);
        if (!c.empty()) c[0] = (char)std::toupper((unsigned char)c[0]);
        l->setText(c);
        l->setFontSize(40.0f);
        l->setTextColor(nvgRGB(255, 255, 255));
        mono->addView(l);
    }
    head->addView(mono);

    auto* who = new brls::Box();
    who->setAxis(brls::Axis::COLUMN);
    who->setGrow(1.0f);
    who->setMarginLeft(24.0f);
    {
        auto* addr = new brls::Label();
        // The address is only kept so this screen can name the account -- the
        // API has no use for it once there is an authKey.
        addr->setText(email.empty() ? tr("this console") : email);
        addr->setFontSize(28.0f);
        addr->setTextColor(theme::text());
        addr->setSingleLine(true);
        who->addView(addr);

        auto* sub = new brls::Label();
        sub->setText(tr("Signed in to Stremio"));
        sub->setFontSize(17.0f);
        sub->setTextColor(theme::textMuted());
        sub->setMarginTop(4.0f);
        who->addView(sub);
    }
    head->addView(who);
    root->addView(head);

    // ---- the two figures --------------------------------------------------
    auto* stats = new brls::Box();
    stats->setAxis(brls::Axis::ROW);
    stats->setMargins(0.0f, 60.0f, 26.0f, 60.0f);

    // Returns the value label, so a count that is not known yet can be filled
    // in when it lands.
    auto stat = [&](const std::string& name, const std::string& value) {
        auto* card = new brls::Box();
        card->setAxis(brls::Axis::COLUMN);
        card->setWidth(200.0f);
        card->setPadding(18.0f, 20.0f, 18.0f, 22.0f);
        card->setCornerRadius(10.0f);
        card->setBackgroundColor(theme::surface());
        card->setMarginRight(20.0f);

        auto* v = new brls::Label();
        v->setText(value);
        v->setFontSize(34.0f);
        v->setTextColor(theme::text());
        card->addView(v);

        auto* n = new brls::Label();
        n->setText(name);
        n->setFontSize(15.0f);
        n->setTextColor(theme::textMuted());
        n->setMarginTop(2.0f);
        card->addView(n);

        stats->addView(card);
        return v;
    };

    int libN = stremio::libraryCount();
    stat(tr("In library"), libN < 0 ? "\xE2\x80\x94" : std::to_string(libN));
    brls::Label* addonCount = stat(tr("Addons"), "\xE2\x80\x94");
    root->addView(stats);

    // ---- the addons -------------------------------------------------------
    auto* addonHdr = new brls::Header();
    addonHdr->setTitle(tr("Installed addons"));
    addonHdr->setMargins(0.0f, 60.0f, 0.0f, 60.0f);
    root->addView(addonHdr);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* addonList = new AsyncBox();
    addonList->setAxis(brls::Axis::COLUMN);
    addonList->setPadding(6.0f, 40.0f, 20.0f, 60.0f);
    scroll->setContentView(addonList);
    root->addView(scroll);

    auto* pending = new brls::Label();
    pending->setText(tr("Reading the account's addons..."));
    pending->setFontSize(16.0f);
    pending->setTextColor(theme::textMuted());
    pending->setMarginTop(8.0f);
    addonList->addView(pending);

    // Free after the first call of the session, so this is normally instant.
    auto live = addonList->alive;
    stremio::fetchAddonsAsync(key, [addonList, live, addonCount,
                                    pending](stremio::AddonsResult r) {
        if (!*live) return;
        pending->setVisibility(brls::Visibility::GONE);
        if (!r.ok)
        {
            auto* err = new brls::Label();
            err->setText(tr("Could not read them: ") + r.error);
            err->setFontSize(16.0f);
            err->setTextColor(theme::textWarn());
            addonList->addView(err);
            return;
        }
        addonCount->setText(std::to_string(r.addons.size()));

        for (const auto& a : r.addons)
        {
            // Focusable, with nothing to activate: this is a list to read,
            // and a ScrollingFrame scrolls by moving focus through its children
            // -- with none of them focusable there was nothing for the stick to
            // move to and the list could only be dragged by touch.
            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setPadding(14.0f, 20.0f, 14.0f, 20.0f);
            row->setMarginBottom(8.0f);
            row->setCornerRadius(8.0f);
            row->setBackgroundColor(theme::scrim(14));
            row->setFocusable(true);
            row->setHighlightCornerRadius(8.0f);

            auto* name = new brls::Label();
            name->setText(a.name);
            name->setFontSize(19.0f);
            name->setTextColor(theme::text());
            name->setSingleLine(true);
            name->setGrow(1.0f);
            name->setMarginRight(20.0f);
            row->addView(name);

            // What it actually serves, which is the only thing about an addon
            // that changes what the app can do with it. An addon on the
            // blocklist keeps whatever else it provides -- only its streams are
            // ignored -- so that is said of the streams, not of the addon.
            bool streamsOff = a.hasStream && stremio::isStreamAddonHidden(a.name);
            std::string what;
            auto add = [&](const char* s) {
                if (!what.empty()) what += "  \xC2\xB7  ";
                what += s;
            };
            if (a.hasMeta) add(tr("Metadata"));
            if (a.hasStream) add(streamsOff ? tr("Streams (disabled)") : tr("Streams"));
            if (a.hasSubtitles) add(tr("Subtitles"));
            if (what.empty()) what = tr("Nothing this app uses");

            bool anyUsable =
                a.hasMeta || a.hasSubtitles || (a.hasStream && !streamsOff);
            if (!anyUsable) name->setTextColor(theme::textDim());

            auto* kind = new brls::Label();
            kind->setText(what);
            kind->setFontSize(16.0f);
            kind->setTextColor(anyUsable ? theme::textDim()
                                         : theme::textFaint());
            kind->setSingleLine(true);
            kind->setShrink(0.0f);
            row->addView(kind);

            addonList->addView(row);
        }

        if (r.addons.empty())
        {
            auto* none = new brls::Label();
            none->setText(tr("None. Install them from Stremio on another device."));
            none->setFontSize(16.0f);
            none->setTextColor(theme::textMuted());
            addonList->addView(none);
        }
    });

    // ---- sign out ---------------------------------------------------------
    auto* logout = new brls::Button();
    logout->setText(tr("Sign out of Stremio"));
    logout->setMargins(4.0f, 60.0f, 24.0f, 60.0f);
    logout->registerClickAction([logout](brls::View*) {
        stremio::clearAuthKey();
        // The addon collection -- and every meta answer cached behind it --
        // belongs to that account.
        stremio::clearAddonCache();
        // The tab holds the key in memory and is only rebuilt when it is
        // re-entered, so say what actually has to happen.
        note(tr("Signed out. Restart the app to get back to the sign-in screen."));
        logout->setState(brls::ButtonState::DISABLED);
        return true;
    });
    root->addView(logout);

    auto* frame = new brls::AppletFrame();
    frame->pushContentView(root);
    frame->setTitle(tr("Account"));
    return frame;
}
