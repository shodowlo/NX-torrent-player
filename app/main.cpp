/*
    SwitchTorrent — borealis front-end (official Horizon-style UI).
    Stage 1: file browser only. mpv playback + torrent engine wired in next.
*/

#include <borealis.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/button.hpp>
#include <borealis/views/tab_frame.hpp>
#include <borealis/views/recycler.hpp>
#include <borealis/views/cells/cell_detail.hpp>

#include <curl/curl.h>

#include <switch.h>  // Thread/Mutex/CondVar for the async log sink

#include "appdata.hpp"
#include "browse.hpp"
#include "config.hpp"
#include "player.hpp"
#include "settings.hpp"
#include "update.hpp"
#include "stremio.hpp"

extern "C" {
#include "torrent.h"
#include "torrentfs.h"
}

namespace
{

// --- Async log sink ---------------------------------------------------------
// The log used to be an UNBUFFERED FILE handed to the Logger: every line
// (each mpv "v" message, the 2 s [stats] line) was a synchronous fs IPC from
// whichever thread logged it -- including the render thread, from draw(). One
// SD-card internal-GC stall blocked rendering for as long as the card pleased
// (measured: a 941 ms draw(), audio underrun included, with every engine
// probe healthy). Logger lines now land in a RAM ring through funopen(); a
// dedicated low-priority thread drains the ring to the SD file and flushes.
// The drain wakes on every line, so a crash still loses at most the few
// milliseconds of lines the stall itself would have eaten.
constexpr size_t kLogRingCap = 256 * 1024;

struct LogRing
{
    FILE* out = nullptr;   // the real SD file
    Mutex lock;
    CondVar cv;
    Thread thread;
    volatile bool stop = false;
    size_t head = 0, len = 0;
    bool overflowed = false;
    char buf[kLogRingCap];
};
LogRing* g_logRing = nullptr;

void logDrainThread(void*)
{
    LogRing* r = g_logRing;
    static char tmp[8 * 1024];  // one drainer; static keeps the stack tiny
    for (;;)
    {
        mutexLock(&r->lock);
        while (r->len == 0 && !r->stop)
            condvarWaitTimeout(&r->cv, &r->lock, 1000000000ULL);  // 1 s
        if (r->len == 0 && r->stop)
        {
            mutexUnlock(&r->lock);
            break;
        }
        size_t n = r->len < sizeof(tmp) ? r->len : sizeof(tmp);
        size_t first = kLogRingCap - r->head;
        if (first > n) first = n;
        memcpy(tmp, r->buf + r->head, first);
        memcpy(tmp + first, r->buf, n - first);
        r->head = (r->head + n) % kLogRingCap;
        r->len -= n;
        bool note = r->overflowed;
        r->overflowed = false;
        mutexUnlock(&r->lock);

        // SD I/O happens here, outside the ring lock: a stalling card blocks
        // only this thread, never a logger.
        std::fwrite(tmp, 1, n, r->out);
        if (note)
        {
            static const char msg[] = "[log] ring overflow: lines dropped\n";
            std::fwrite(msg, 1, sizeof(msg) - 1, r->out);
        }
        std::fflush(r->out);
    }
    std::fflush(r->out);
}

// funopen() write callback: memcpy into the ring and return. Never touches
// the fs. On overflow (SD slower than the log rate for a long stretch) lines
// are dropped and the drain thread notes it in the file.
int logRingWrite(void* cookie, const char* p, size_t n)
{
    LogRing* r = static_cast<LogRing*>(cookie);
    if (n == 0) return 0;
    mutexLock(&r->lock);
    size_t space = kLogRingCap - r->len;
    size_t take = n <= space ? n : space;
    if (take < n) r->overflowed = true;
    size_t tail = (r->head + r->len) % kLogRingCap;
    size_t first = kLogRingCap - tail;
    if (first > take) first = take;
    memcpy(r->buf + tail, p, first);
    memcpy(r->buf, p + first, take - first);
    r->len += take;
    mutexUnlock(&r->lock);
    condvarWakeOne(&r->cv);
    return (int)n;  // claim everything: a short write would make stdio error out
}

// Everything the app writes (log, piece cache) lives here so it doesn't clutter
// the SD card root. Created at startup. Paths in appdata.hpp.
void ensureAppDataDir()
{
    mkdir("sdmc:/switch", 0777);   // usually already there
    mkdir(APPDATA_DIR, 0777);      // ignore EEXIST
    mkdir(APPDATA_TORRENTS, 0777); // where the user drops .torrent files
    mkdir(APPDATA_POSTERS, 0777);  // cached Stremio artwork
}


struct TorrentEntry
{
    std::string name;      // display name (file name)
    std::string path;      // full path on the SD card
    std::string sizeText;  // human-readable size of the whole torrent
};

// Human-readable byte size (e.g. "1.4 GB").
std::string humanSize(int64_t bytes)
{
    if (bytes <= 0)
        return "";
    const char* unit[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int i    = 0;
    while (v >= 1024.0 && i < 4)
    {
        v /= 1024.0;
        i++;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), (v < 10.0 && i > 0) ? "%.1f %s" : "%.0f %s",
                  v, unit[i]);
    return buf;
}

// True for the file extensions we can actually play. A torrent carries subs,
// artwork and .nfo next to the video; listing those as playable choices would
// just be noise.
bool isVideoFile(const std::string& path)
{
    static const char* kExt[] = { ".mkv", ".mp4", ".avi", ".m4v",  ".mov",
                                  ".webm", ".ts", ".m2ts", ".mpg", ".mpeg",
                                  ".wmv", ".flv", ".ogv" };
    for (const char* e : kExt)
    {
        size_t n = std::strlen(e);
        if (path.size() > n &&
            strcasecmp(path.c_str() + path.size() - n, e) == 0)
            return true;
    }
    return false;
}

// Reads a .torrent's metadata for the browser: its total size, and whether it
// holds anything we can play. The sizes are in the metadata, so no download is
// needed. torrent_meta is ~70 KB, so it goes on the heap rather than the
// (startup-thread) stack. False = don't list this torrent.
bool torrentScanInfo(const std::string& path, std::string& sizeText)
{
    torrent_meta* m = (torrent_meta*)std::calloc(1, sizeof(torrent_meta));
    if (!m)
        return false;
    char err[128] = { 0 };
    bool hasVideo = false;
    if (torrent_load(m, path.c_str(), err, sizeof(err)) == 0)
    {
        for (int i = 0; i < m->file_count && !hasVideo; i++)
            hasVideo = isVideoFile(m->files[i].path);
        // The whole torrent, not just its video: that is the number the user
        // compares against the free space on their SD card, and it is what
        // every other client shows.
        sizeText = humanSize(m->total_len);
        brls::Logger::info("[scan] {} -> {} file(s), total {} ({}), video={}",
                           path, m->file_count, (long long)m->total_len,
                           sizeText, hasVideo);
        torrent_unload(m);
    }
    else
    {
        brls::Logger::warning("[scan] torrent_load failed for {}: {}", path,
                              err);
    }
    std::free(m);
    return hasVideo;
}

// Scans a directory for .torrent files, recursing into sub-folders. `dir` must
// end with a slash. Directory detection is done by trying to opendir the entry
// (libnx's dirent d_type / stat are unreliable), so anything that isn't a
// .torrent and can be opened as a directory is recursed into.
void scanTorrentsRec(const std::string& dir, std::vector<TorrentEntry>& out)
{
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr)
    {
        std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        std::string full = dir + name;
        if (name.size() > 8 &&
            name.compare(name.size() - 8, 8, ".torrent") == 0)
        {
            // Skip torrents with nothing playable in them (a music album, a
            // game): the app can only stream video, so listing them only leads
            // to a dead end.
            std::string sizeText;
            if (torrentScanInfo(full, sizeText))
                out.push_back({ name, full, sizeText });
        }
        else
        {
            // Not a .torrent: recurse if it's a directory (opendir returns null
            // for regular files, so this is a no-op for them).
            scanTorrentsRec(full + "/", out);
        }
    }
    closedir(d);
}

std::vector<TorrentEntry> scanTorrents(const std::string& dir)
{
    std::vector<TorrentEntry> out;
    scanTorrentsRec(dir, out);
    std::sort(out.begin(), out.end(),
              [](const TorrentEntry& a, const TorrentEntry& b) {
                  return a.name < b.name;
              });
    return out;
}

// One playable file inside a torrent.
struct VideoFile
{
    int index;         // into torrent_meta.files
    std::string name;  // last path component
    int64_t length;
};

// The video files a .torrent holds, largest first. Empty if it cannot be read.
std::vector<VideoFile> torrentVideoFiles(const std::string& path)
{
    std::vector<VideoFile> out;
    // ~70 KB: too big for the stack of whatever thread we are called on.
    torrent_meta* m = (torrent_meta*)std::calloc(1, sizeof(torrent_meta));
    if (!m) return out;
    char err[128] = { 0 };
    if (torrent_load(m, path.c_str(), err, sizeof(err)) == 0)
    {
        for (int i = 0; i < m->file_count; i++)
        {
            std::string p = m->files[i].path;
            if (!isVideoFile(p)) continue;
            size_t slash = p.find_last_of("/\\");
            out.push_back({ i, slash == std::string::npos ? p : p.substr(slash + 1),
                            m->files[i].length });
        }
        torrent_unload(m);
    }
    else
    {
        brls::Logger::warning("[files] torrent_load failed for {}: {}", path, err);
    }
    std::free(m);
    std::sort(out.begin(), out.end(), [](const VideoFile& a, const VideoFile& b) {
        return a.length > b.length;
    });
    return out;
}

// List cell: a video icon on the left, the torrent name, and its size on the
// right. Built in code (RecyclerCell's ctor wires the click -> didSelectRowAt).
class TorrentCell : public brls::RecyclerCell
{
  public:
    brls::Label* name = nullptr;
    brls::Label* size = nullptr;

    TorrentCell()
    {
        this->setFocusable(true);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        this->setHeight(64.0f);
        this->setPaddingLeft(24.0f);
        this->setPaddingRight(24.0f);
        this->setCornerRadius(6.0f);

        auto* icon = new brls::Image();
        icon->setImageFromRes("video-icon.png");
        icon->setScalingType(brls::ImageScalingType::FIT);
        icon->setDimensions(38.0f, 38.0f);
        icon->setMarginRight(20.0f);
        this->addView(icon);

        name = new brls::Label();
        name->setFontSize(22.0f);
        name->setGrow(1.0f);
        // Torrent names are long enough to wrap to two lines, which made the
        // rows ragged. Keep one line: borealis then truncates it, and scrolls
        // the full name past on focus (autoAnimate, on by default).
        name->setSingleLine(true);
        this->addView(name);

        size = new brls::Label();
        size->setFontSize(19.0f);
        size->setTextColor(brls::Application::getTheme().getColor(
            "brls/list/listItem_value_color"));
        size->setMarginLeft(16.0f);
        // "6.3 GB" was being squeezed by the growing name until it wrapped.
        // It is short and fixed-shape, so give it a lane of its own and let the
        // name take whatever is left.
        size->setSingleLine(true);
        size->setShrink(0.0f);
        size->setWidth(86.0f);
        size->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        this->addView(size);
    }

    static brls::RecyclerCell* create() { return new TorrentCell(); }
};

class TorrentDataSource : public brls::RecyclerDataSource
{
  public:
    explicit TorrentDataSource(std::vector<TorrentEntry> items)
        : items(std::move(items))
    {
    }

    int numberOfRows(brls::RecyclerFrame* recycler, int section) override
    {
        return (int)items.size();
    }

    brls::RecyclerCell* cellForRow(brls::RecyclerFrame* recycler,
                                   brls::IndexPath index) override
    {
        auto* cell = (TorrentCell*)recycler->dequeueReusableCell("Cell");
        cell->name->setText(items[index.row].name);
        cell->size->setText(items[index.row].sizeText);
        return cell;
    }

    void didSelectRowAt(brls::RecyclerFrame* recycler,
                        brls::IndexPath index) override
    {
        const std::string& path = items[index.row].path;

        // A season pack holds one video per episode, and the engine's default
        // (stream the largest file) would pick an arbitrary one. Ask.
        std::vector<VideoFile> vids = torrentVideoFiles(path);
        if (vids.size() > 1)
        {
            std::vector<Row> rows;
            for (const auto& v : vids)
            {
                Row row;
                row.label  = v.name;
                row.detail = humanSize(v.length);
                rows.push_back(row);
            }
            brls::Application::pushActivity(new ListActivity(
                items[index.row].name, "Pick a file", rows,
                [path, vids](int i) {
                    brls::Logger::info("Playing torrent: {} file {} ({})", path,
                                       vids[i].index, vids[i].name);
                    brls::Application::pushActivity(
                        new PlayerActivity(path, {}, vids[i].name,
                                           vids[i].index));
                }));
            return;
        }

        brls::Logger::info("Playing torrent: {}", path);
        brls::Application::pushActivity(new PlayerActivity(path));
    }

  private:
    std::vector<TorrentEntry> items;
};

// Centered message shown when the torrents folder is empty, telling the user
// where to drop .torrent files.
brls::View* buildEmptyState()
{
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setGrow(1.0f);
    box->setPadding(0, 80, 0, 80);

    auto* title = new brls::Label();
    title->setText("No torrents found");
    title->setFontSize(26);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMargins(0, 0, 18, 0);
    box->addView(title);

    auto* hint = new brls::Label();
    hint->setText("Add .torrent files to this folder on your SD card, "
                  "then relaunch:");
    hint->setFontSize(20);
    hint->setTextColor(nvgRGB(190, 190, 195));  // light gray (text_disabled is
                                                // too dark on the dark theme)
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    hint->setMargins(0, 0, 10, 0);
    box->addView(hint);

    auto* path = new brls::Label();
    path->setText("/switch/NX-torrent-player/torrents/");
    path->setFontSize(21);
    path->setTextColor(brls::Application::getTheme().getColor("brls/accent"));
    path->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    box->addView(path);

    return box;
}

// Shown on the libnx text console (NOT borealis) when the app was launched in
// applet mode -- that mode caps the heap far below what mpv and the RAM
// streaming window need. Done before borealis/GL is ever set up: bringing the
// whole UI stack up only to tear it back down on exit is what crashed the
// console in applet mode. Blocks on + then returns, so main() exits cleanly.
static void appletModeBlock()
{
    consoleInit(NULL);
    printf("\n  NX Torrent Player - full memory required\n\n");
    printf("  This app was launched in applet mode, which caps\n");
    printf("  memory well below what the player and its\n");
    printf("  streaming buffer need.\n\n");
    printf("  Launch it with full memory instead: start it over\n");
    printf("  a game (hold R while opening the game), or use a\n");
    printf("  forwarder.\n\n");
    printf("  Press + to exit.\n");
    consoleUpdate(NULL);

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
}

// The local ".torrent files on the SD card" list -- now one tab among several.
brls::View* buildLocalTab()
{
    auto items = scanTorrents(APPDATA_TORRENTS "/");

    brls::View* content;
    if (items.empty())
    {
        content = buildEmptyState();
    }
    else
    {
        auto* recycler = new brls::RecyclerFrame();
        recycler->registerCell("Cell", []() { return TorrentCell::create(); });
        recycler->setDataSource(new TorrentDataSource(std::move(items)));
        recycler->setGrow(1.0f);
        // Inset the list so the focus highlight (which draws a glow outside the
        // cell) isn't clipped by the screen edges.
        // setPadding(top,right,bottom,left).
        recycler->setPadding(32.0f, 60.0f, 47.0f, 60.0f);
        content = recycler;
    }
    return content;
}

// The Stremio view chip sits next to the header title: "· <glyph> <name>". The
// glyph is its OWN centred Label (a Material glyph inside the title text floats
// high off the text baseline); these are wired up in buildBrowser and updated by
// the library-count sink. File scope so applyTabIdentity can hide them off-tab.
brls::Box*   g_viewChip  = nullptr;
brls::Label* g_viewSep   = nullptr;
brls::Label* g_viewGlyph = nullptr;
brls::Label* g_viewName  = nullptr;

// Title + icon identifying the category on screen: the app itself for Local,
// Stremio's own mark for Stremio. Called on every tab switch, in both layouts.
void applyTabIdentity(brls::AppletFrame* frame, config::Tab tab)
{
    if (tab == config::Tab::STREMIO)
    {
        frame->setTitle("Stremio");
        frame->setIcon("romfs:/stremio-icon.png");
    }
    else
    {
        frame->setTitle("NX Torrent Player");
        frame->setIcon("romfs:/local-icon.png");
        // The view chip belongs to Stremio only -- drop it on the Local tab.
        if (g_viewChip) g_viewChip->setVisibility(brls::Visibility::GONE);
    }
    // The header icon is sized "auto", i.e. from the texture, and these two are
    // not the same pixel size -- pin both to the same box.
    if (auto* ic = dynamic_cast<brls::Image*>(
            frame->getView("brls/applet_frame/title_icon")))
    {
        ic->setDimensions(54.0f, 54.0f);
        ic->setScalingType(brls::ImageScalingType::FIT);
    }
}

brls::View* buildTab(config::Tab tab)
{
    return tab == config::Tab::STREMIO ? (brls::View*)new StremioTab()
                                       : buildLocalTab();
}

// Categories down the left, in borealis' sidebar. The Horizon convention.
brls::View* buildBrowserSidebar(brls::AppletFrame* frame)
{
    // borealis' default sidebar is 410px, which eats a third of a 1280-wide
    // screen for two short labels. The space goes to the list instead.
    //
    // The width alone is not enough: the sidebar's own padding is 80px left /
    // 40px right, scaled for those 410. Left as-is it holds the items at the
    // same offset and leaves ~50px of text lane, so the labels come out
    // crushed. Shrink the padding to match, keeping ~120px for the longest
    // label ("Stremio", ~80px at font size 22).
    // (getStyle() returns a by-value wrapper around the shared metric table,
    // so writing through a copy is fine.)
    brls::Style style = brls::Application::getStyle();
    style.addMetric("brls/tab_frame/sidebar_width", 170.0f);
    style.addMetric("brls/sidebar/padding_left", 28.0f);
    style.addMetric("brls/sidebar/padding_right", 20.0f);

    auto* tabs = new brls::TabFrame();
    tabs->addTab("Local", [frame]() {
        applyTabIdentity(frame, config::Tab::LOCAL);
        return buildTab(config::Tab::LOCAL);
    });
    tabs->addTab("Stremio", [frame]() {
        applyTabIdentity(frame, config::Tab::STREMIO);
        return buildTab(config::Tab::STREMIO);
    });
    return tabs;
}

// Categories as buttons in the header, top-right. Hand-built: borealis' TabFrame
// is sidebar-only. `content` must already be the frame's content view -- see the
// hint_box note below.
void attachTopTabBar(brls::AppletFrame* frame, brls::Box* content)
{
    auto* bar = new brls::Box();
    bar->setAxis(brls::Axis::ROW);
    bar->setAlignItems(brls::AlignItems::CENTER);

    // Rebuilt on every switch: TabFrame frees the old tab before making the new
    // one, and so must we -- two live StremioTabs would each hold their own
    // library and in-flight requests.
    auto* localBtn   = new brls::Button();
    auto* stremioBtn = new brls::Button();
    auto select      = [content, localBtn, stremioBtn, frame](config::Tab tab) {
        localBtn->setStyle(tab == config::Tab::LOCAL
                               ? &brls::BUTTONSTYLE_PRIMARY
                               : &brls::BUTTONSTYLE_BORDERLESS);
        stremioBtn->setStyle(tab == config::Tab::STREMIO
                                 ? &brls::BUTTONSTYLE_PRIMARY
                                 : &brls::BUTTONSTYLE_BORDERLESS);
        content->clearViews();  // deletes the previous tab
        brls::View* v = buildTab(tab);
        v->setGrow(1.0f);
        content->addView(v);
        applyTabIdentity(frame, tab);

        // B returns to the tab bar, which is what TabFrame does for its sidebar
        // -- and the only reliable way back up here. Navigating UP out of a list
        // does not do it: ScrollingFrame::getNextFocus keeps the focus inside
        // while the list can still scroll up, so a deep tab (Stremio's library
        // sits under two more boxes than the local list) never hands it over.
        brls::Button* active = tab == config::Tab::STREMIO ? stremioBtn : localBtn;
        v->registerAction(
            "Back", brls::BUTTON_B,
            [active](brls::View*) {
                brls::Application::giveFocus(active);
                return true;
            },
            false, false, brls::SOUND_BACK);
    };

    localBtn->setText("Local");
    localBtn->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    localBtn->registerClickAction([select](brls::View*) {
        select(config::Tab::LOCAL);
        return true;
    });
    bar->addView(localBtn);

    stremioBtn->setText("Stremio");
    stremioBtn->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    stremioBtn->setMarginLeft(8.0f);
    stremioBtn->registerClickAction([select](brls::View*) {
        select(config::Tab::STREMIO);
        return true;
    });
    bar->addView(stremioBtn);

    // hint_box is the header's right-hand slot. Nothing else claims it here (the
    // browser's content views carry no hint view), but pushContentView ->
    // updateAppletFrameItem() CLEARS it and resets title/icon -- so this, and
    // the applyTabIdentity inside select(), only work once the content view is
    // already in place.
    if (auto* hints = dynamic_cast<brls::Box*>(
            frame->getView("brls/applet_frame/hint_box")))
        hints->addView(bar);

    // The library list cannot walk focus back out to the header on its own; hand
    // it the button to jump to (see stremio::setLibraryUpTarget).
    stremio::setLibraryUpTarget(stremioBtn);

    select(config::get().startupTab);
}

brls::View* buildBrowser()
{
    auto* frame = new brls::AppletFrame();
    // TabFrame / our content box are plain Boxes with no header of their own,
    // so they go inside the AppletFrame that carries the title/icon.
    bool top = config::get().tabBar == config::TabBar::TOP;

    // IMPORTANT: everything touching the header has to happen AFTER
    // pushContentView -- it calls updateAppletFrameItem(), which empties
    // hint_box and resets title/icon to the content view's (blank) ones.
    if (top)
    {
        auto* content = new brls::Box();
        content->setAxis(brls::Axis::COLUMN);
        content->setGrow(1.0f);
        frame->pushContentView(content);
        attachTopTabBar(frame, content);
    }
    else
    {
        frame->pushContentView(buildBrowserSidebar(frame));
        // The sidebar's tab creators set the identity, but focusTab only fires
        // one when it actually moves the focus -- Local is already focused at 0.
        applyTabIdentity(frame, config::Tab::LOCAL);
        if (config::get().startupTab == config::Tab::STREMIO)
            ((brls::TabFrame*)frame->getContentView())->focusTab(1);
    }

    // The Stremio library reports its current view through the header, next to
    // the "Stremio" title, as "· <glyph> <name>". Rather than fold the glyph into
    // the title text (where a Material glyph rides high off the baseline), we
    // render it as a separate, vertically-centred Label in a chip appended to the
    // header's title box. We reach that box via the title label's parent, so no
    // borealis change is needed.
    {
        float fs     = brls::Application::getStyle()["brls/applet_frame/header_title_font_size"];
        float topOff = brls::Application::getStyle()["brls/applet_frame/header_title_top_offset"];
        auto* titleLabel = dynamic_cast<brls::Label*>(
            frame->getView("brls/applet_frame/title_label"));
        auto* titleBox = titleLabel
            ? dynamic_cast<brls::Box*>(titleLabel->getParent()) : nullptr;
        if (titleBox)
        {
            g_viewChip = new brls::Box();
            g_viewChip->setAxis(brls::Axis::ROW);
            g_viewChip->setAlignItems(brls::AlignItems::CENTER);
            g_viewChip->setMarginTop(topOff);
            g_viewChip->setMarginLeft(14.0f);
            g_viewChip->setVisibility(brls::Visibility::GONE);

            g_viewSep = new brls::Label();
            g_viewSep->setFontSize(fs);
            g_viewSep->setText("·");
            g_viewChip->addView(g_viewSep);

            g_viewGlyph = new brls::Label();
            g_viewGlyph->setFontSize(fs);
            g_viewGlyph->setMarginLeft(12.0f);
            g_viewGlyph->setMarginRight(9.0f);
            // A Material glyph sits high in its line box (ink fills toward the
            // ascender, none below the baseline). translationY drops just the
            // rendered glyph, without growing its layout box, so the separator and
            // name keep their place.
            g_viewGlyph->setTranslationY(fs * 0.11f);
            g_viewChip->addView(g_viewGlyph);

            g_viewName = new brls::Label();
            g_viewName->setFontSize(fs);
            g_viewChip->addView(g_viewName);

            titleBox->addView(g_viewChip);
        }
    }

    // Each header arrives as "<glyph 3 bytes>  <name>" (or "" to clear). Split the
    // leading PUA glyph off so it lands in its own centred Label; the name follows.
    stremio::setLibraryCountSink([frame](const std::string& count) {
        frame->setTitle("Stremio");
        if (!g_viewChip) return;
        if (count.empty())
        {
            g_viewChip->setVisibility(brls::Visibility::GONE);
            return;
        }
        std::string glyph, name = count;
        unsigned char lead = (unsigned char)count[0];
        if (lead >= 0xEE && count.size() >= 3)  // 3-byte UTF-8 PUA glyph
        {
            glyph = count.substr(0, 3);
            name  = count.substr(3);
            size_t s = name.find_first_not_of(' ');
            name = (s == std::string::npos) ? "" : name.substr(s);
        }
        g_viewGlyph->setText(glyph);
        g_viewGlyph->setVisibility(glyph.empty() ? brls::Visibility::GONE
                                                 : brls::Visibility::VISIBLE);
        g_viewName->setText(name);
        g_viewChip->setVisibility(brls::Visibility::VISIBLE);
    });

    frame->registerAction("Options", brls::BUTTON_X, [](brls::View*) {
        brls::Application::pushActivity(new SettingsActivity());
        return true;
    });

    // R/L cycle the Stremio view. On the frame (not the tab) so they work with
    // focus on the header tab bar too; a no-op when the Stremio tab is not live.
    // One chip "L R  View": L is the auto icon, R is a glyph in the hint text.
    // (The text glyph renders a touch smaller than the icon -- a borealis hint
    // quirk -- but this keeps them side by side; the R action is hidden so it
    // does not add a second chip.)
    frame->registerAction("  View", brls::BUTTON_LB, [](brls::View*) {
        stremio::cycleActiveView(-1);
        return true;
    }, false, false, brls::SOUND_NONE);
    frame->registerAction("View", brls::BUTTON_RB, [](brls::View*) {
        stremio::cycleActiveView(+1);
        return true;
    }, true, false, brls::SOUND_NONE);

    return frame;
}

} // namespace

int main(int argc, char* argv[])
{
    // Applet mode caps the heap far below what mpv + the RAM streaming window
    // need. Bail before anything is initialised (a full-RAM launch runs as an
    // Application or SystemApplication; everything else is applet mode) -- and
    // via the plain text console, since standing borealis/GL up just to shut it
    // down on exit is what crashed the console here.
    AppletType appletType = appletGetAppletType();
    if (appletType != AppletType_Application &&
        appletType != AppletType_SystemApplication)
    {
        appletModeBlock();
        return EXIT_SUCCESS;
    }

    ensureAppDataDir();

    config::load();
    // The engine default already matches the config default, but the config
    // may say otherwise: hand it over before any torrentfs can be opened.
    torrentfs_set_governor(config::get().rateGovernor ? 1 : 0);
    // argv[0] tells us which .nro to replace; nothing else can.
    update::init(argc, argv);

    // Off by default: unbuffered writes plus a [stats] line every 2s means the
    // SD card is written to for the whole session. Opt in from Options when
    // something needs diagnosing.
    brls::Logger::setLogLevel(config::get().logging ? brls::LogLevel::LOG_DEBUG
                                                    : brls::LogLevel::LOG_ERROR);
    if (config::get().logging)
    {
        if (FILE* lf = std::fopen(APPDATA_LOG, "w+"))
        {
            // Async sink (see LogRing above): loggers memcpy into a RAM ring,
            // a dedicated thread does the actual SD writes. Priority 0x3B
            // (well below everything): losing a beat only delays the file,
            // never a frame.
            g_logRing = new LogRing();
            g_logRing->out = lf;
            mutexInit(&g_logRing->lock);
            condvarInit(&g_logRing->cv);
            FILE* rf = nullptr;
            if (threadCreate(&g_logRing->thread, logDrainThread, nullptr,
                             nullptr, 0x8000, 0x3B, -2) == 0)
            {
                threadStart(&g_logRing->thread);
                rf = funopen(g_logRing, nullptr, logRingWrite, nullptr, nullptr);
            }
            if (rf)
            {
                // Unbuffered so every Logger line reaches the ring (and its
                // wake) immediately -- the "write" is a memcpy, so unbuffered
                // costs nothing here.
                std::setvbuf(rf, nullptr, _IONBF, 0);
                brls::Logger::setLogOutput(rf);
            }
            else
            {
                // No thread/funopen: fall back to the old direct unbuffered
                // file rather than losing the log entirely.
                std::setvbuf(lf, nullptr, _IONBF, 0);
                brls::Logger::setLogOutput(lf);
            }
        }
    }

    // Must happen once, before any thread touches curl: the old source/main.c
    // did this, but that front-end is no longer compiled, so curl was being used
    // (Stremio login) without ever being initialised.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!brls::Application::init())
    {
        brls::Logger::error("Unable to init borealis");
        return EXIT_FAILURE;
    }

    brls::Application::createWindow("NX Torrent Player");
    brls::Application::getPlatform()->setThemeVariant(brls::ThemeVariant::DARK);
    brls::Application::setGlobalQuit(true);

    brls::Application::pushActivity(new brls::Activity(buildBrowser()));

    // A quiet check: it only ever surfaces when there IS a newer release, so a
    // missing network or an unreachable GitHub costs nothing but a log line.
    if (config::get().checkUpdates)
        update::checkAsync([](update::Release r) {
            if (r.ok && r.installable()) update::promptInstall(r);
        });

    while (brls::Application::mainLoop())
        ;

    // Only now: hbloader keeps the running .nro open and libnx reads our romfs
    // out of it, so the file cannot be replaced until the app is done with it.
    // The new version is picked up on the next launch.
    update::applyPending();

    // Nothing to wait for here: ~MpvView closes the engine synchronously, and
    // Application::exit() destroys the activity stack before this returns. That
    // matters because as soon as main() returns libnx runs __appExit ->
    // userAppExit -> socketExit(), and any engine thread still alive past that
    // point would dereference a NULL socket devoptab and data abort.
    return EXIT_SUCCESS;
}
