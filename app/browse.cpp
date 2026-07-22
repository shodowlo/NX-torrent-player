#include "browse.hpp"

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

#include "config.hpp"
#include "player.hpp"

namespace
{

// True if `v` is `ancestor` or sits under it.
bool isUnder(brls::View* v, brls::View* ancestor)
{
    for (; v; v = v->getParent())
        if (v == ancestor) return true;
    return false;
}

// Trackers appended to the bare infoHash an addon gives us. These are the
// long-running public UDP trackers most public torrents announce to anyway.
constexpr const char* kPublicTrackers =
    "&tr=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce"
    "&tr=udp%3A%2F%2Fopen.demonii.com%3A1337%2Fannounce"
    "&tr=udp%3A%2F%2Ftracker.openbittorrent.com%3A6969%2Fannounce"
    "&tr=udp%3A%2F%2Fexodus.desync.com%3A6969%2Fannounce"
    "&tr=udp%3A%2F%2Ftracker.torrent.eu.org%3A451%2Fannounce"
    "&tr=udp%3A%2F%2Fopen.stealth.si%3A80%2Fannounce"
    "&tr=udp%3A%2F%2Ftracker.moeking.me%3A6969%2Fannounce"
    "&tr=udp%3A%2F%2Fexplodie.org%3A6969%2Fannounce";

// The Switch's shared system font has no colour emoji, so the pictographs
// addons use as field markers render as blanks/boxes. Swap the common ones for
// short labels rather than dropping the numbers they introduce.
std::string deEmoji(const std::string& s)
{
    struct { const char* from; const char* to; } map[] = {
        { "👤", "seeds:" }, { "💾", "taille:" }, { "⚙️", "src:" },
        { "⚙", "src:" },    { "🌐", "lang:" },   { "🎬", "" },
        { "📺", "" },        { "⭐", "" },        { "🔊", "audio:" },
        { "🇬🇧", "EN" },     { "🇫🇷", "FR" },
    };
    std::string out = s;
    for (const auto& m : map)
    {
        size_t p;
        while ((p = out.find(m.from)) != std::string::npos)
            out.replace(p, std::strlen(m.from), m.to);
    }
    // Anything still non-ASCII would draw as a blank box; strip it.
    std::string clean;
    for (size_t i = 0; i < out.size();)
    {
        unsigned char c = out[i];
        if (c < 0x80) { clean += (char)c; i++; continue; }
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        // Keep Latin-1 range accents, drop pictographs.
        if (len == 2) { clean += out.substr(i, 2); }
        i += len;
    }
    // Collapse the runs of spaces the removals leave behind.
    std::string tight;
    bool sp = false;
    for (char c : clean)
    {
        if (c == ' ') { if (!sp) tight += c; sp = true; }
        else { tight += c; sp = false; }
    }
    while (!tight.empty() && tight.back() == ' ') tight.pop_back();
    return tight;
}

void dialog(const std::string& msg)
{
    std::string s = msg;
    for (char& c : s)
        if (c == '\n' || c == '\r') c = ' ';
    if (s.size() > 120) s = s.substr(0, 119) + "…";
    auto* d = new brls::Dialog(s);
    d->addButton("OK", []() {});
    d->open();
}

// Addons pack several lines into one field, e.g. Torrentio sends
//   name  = "Torrentio\n4k"
//   title = "The.Movie.2024.2160p.WEB-DL\n👤 45 💾 15.2 GB ⚙️ ThePirateBay"
// so both need splitting to be usable.
std::vector<std::string> splitLines(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s)
    {
        if (c == '\n' || c == '\r')
        {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        }
        else
            cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string joinSpace(const std::vector<std::string>& v, size_t from = 0)
{
    std::string out;
    for (size_t i = from; i < v.size(); i++)
    {
        if (!out.empty()) out += "  ";
        out += v[i];
    }
    return out;
}

// A row of the list: label (+ optional sub-line) on the left, detail on the
// right.
class RowCell : public brls::Box
{
  public:
    RowCell(const Row& r, std::function<void()> cb)
    {
        this->setFocusable(true);
        this->setAxis(brls::Axis::ROW);
        this->setAlignItems(brls::AlignItems::CENTER);
        // An episode still needs the height to be worth showing at all.
        this->setHeight(!r.artId.empty() ? 84.0f : (r.sub.empty() ? 60.0f : 76.0f));
        this->setPaddingLeft(20.0f);
        this->setPaddingRight(24.0f);
        // Margin, not padding, and on the row rather than the list: the focus
        // highlight draws OUTSIDE the row's bounds and the frame scissors to
        // its own width, so a row reaching the edge has its glow clipped.
        // Padding would only move the text and leave the box (and its glow) at
        // the edge. It also keeps the scrolling indicator, pinned to the
        // frame's edge, in a lane of its own.
        this->setMarginRight(40.0f);
        this->setCornerRadius(6.0f);
        this->registerClickAction([cb](brls::View*) {
            cb();
            return true;
        });
        // registerClickAction only binds the A button; a tap gesture makes the
        // row respond to the touchscreen too (it fires the same A action).
        this->addGestureRecognizer(new brls::TapGestureRecognizer(this));

        // Episode still, as Stremio shows it. Fetched asynchronously so the
        // list draws now and the images fill in, rather than the screen waiting
        // on a dozen downloads.
        if (!r.artId.empty())
        {
            auto* art = new brls::Image();
            art->setDimensions(114.0f, 64.0f);  // 16:9, the still's aspect
            art->setScalingType(brls::ImageScalingType::FIT);
            art->setCornerRadius(6.0f);  // rounded corners, like the library posters
            art->setMarginRight(18.0f);
            art->setShrink(0.0f);
            this->addView(art);

            stremio::fetchPosterAsync(r.artId, r.artUrl,
                                      [art, live = alive](std::string path) {
                                          if (!*live || path.empty()) return;
                                          art->setImageFromFile(path);
                                      });
        }

        // Label over sub-label, so a source can show what it is *and* which
        // file it is without either being truncated into uselessness.
        auto* text = new brls::Box();
        text->setAxis(brls::Axis::COLUMN);
        text->setJustifyContent(brls::JustifyContent::CENTER);
        text->setGrow(1.0f);

        auto* l = new brls::Label();
        l->setText(r.label);
        l->setFontSize(21.0f);
        l->setSingleLine(true);
        text->addView(l);

        if (!r.sub.empty())
        {
            auto* s = new brls::Label();
            s->setText(r.sub);
            s->setFontSize(16.0f);
            s->setTextColor(nvgRGB(150, 150, 155));
            s->setSingleLine(true);
            s->setMarginTop(3.0f);
            text->addView(s);
        }

        // Watch-progress bar (Stremio library state): where the account is in
        // this episode/film.
        if (r.progress > 0.005)
        {
            auto* track = new brls::Box();
            track->setWidthPercentage(100.0f);
            track->setHeight(4.0f);
            track->setCornerRadius(2.0f);
            track->setBackgroundColor(nvgRGBA(255, 255, 255, 40));
            track->setMarginTop(7.0f);

            auto* fill = new brls::Box();
            fill->setWidthPercentage(
                (float)((r.progress > 1.0 ? 1.0 : r.progress) * 100.0));
            fill->setHeight(4.0f);
            fill->setCornerRadius(2.0f);
            fill->setBackgroundColor(
                brls::Application::getTheme().getColor("brls/accent"));
            track->addView(fill);
            text->addView(track);
        }
        this->addView(text);

        if (!r.detail.empty())
        {
            auto* d = new brls::Label();
            d->setText(r.detail);
            d->setFontSize(18.0f);
            d->setTextColor(nvgRGB(200, 200, 205));
            d->setSingleLine(true);
            d->setMarginLeft(16.0f);
            this->addView(d);
        }
    }

    // B can pop this list while a still is still downloading, taking the Image
    // with it. The fetch holds a raw pointer to it, so it has to be told.
    ~RowCell() override { *alive = false; }

  private:
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

// Spinner shown while a level is loading; pushed as its own activity so B
// cancels the wait instead of freezing on a blank screen.
class LoadingActivity : public brls::Activity
{
  public:
    // `title` / `iconPath` name what is being loaded (the film/show), so the
    // header does not fall back to a generic "Stremio" for the second or two
    // this is up, and matches the screen it leads to.
    LoadingActivity(std::string msg, std::string title = "Stremio",
                    std::string iconPath = "")
        : msg(std::move(msg))
        , title(std::move(title))
        , iconPath(std::move(iconPath))
    {
    }

    brls::View* createContentView() override
    {
        auto* box = new brls::Box();
        box->setAxis(brls::Axis::COLUMN);
        box->setJustifyContent(brls::JustifyContent::CENTER);
        box->setAlignItems(brls::AlignItems::CENTER);
        box->setGrow(1.0f);
        // Takes the focus off the row that was just tapped, but shows no
        // highlight of its own. Without something focusable here borealis has
        // nowhere to move the focus to, so it stays on that row and keeps
        // drawing its highlight over the spinner.
        box->setFocusable(true);
        box->setHideHighlight(true);

        auto* sp = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
        sp->setDimensions(52.0f, 52.0f);
        sp->animate(true);
        box->addView(sp);

        auto* l = new brls::Label();
        l->setText(msg);
        l->setFontSize(20);
        l->setTextColor(nvgRGB(190, 190, 195));
        l->setMargins(24, 0, 0, 0);
        box->addView(l);

        auto* frame = new brls::AppletFrame();
        frame->pushContentView(box);
        // After pushContentView: it overwrites the title with the content
        // view's.
        frame->setTitle(title);
        if (!iconPath.empty())
        {
            frame->setIcon(iconPath);
            // Sized "auto", i.e. from the texture -- a poster is several hundred
            // pixels tall and would blow the header open. Pin it to 2:3 inside
            // the 88px header, same as ListActivity.
            if (auto* ic = dynamic_cast<brls::Image*>(
                    frame->getView("brls/applet_frame/title_icon")))
            {
                ic->setDimensions(40.0f, 60.0f);
                ic->setScalingType(brls::ImageScalingType::FIT);
            }
        }
        return frame;
    }

  private:
    std::string msg, title, iconPath;
};

// Replaces the loading screen with `next`, so B from `next` lands on the level
// the user came from rather than back on a spinner.
void swapLoading(brls::Activity* next)
{
    brls::Application::popActivity(brls::TransitionAnimation::NONE, [next]() {
        brls::Application::pushActivity(next);
    });
}

void closeLoading()
{
    brls::Application::popActivity(brls::TransitionAnimation::NONE);
}

// The season number of a Stremio episode id ("tt1234567:2:5" -> 2), or -1 when
// the id doesn't carry one (films, empty, or an odd catalog id).
int seasonOfVideoId(const std::string& videoId)
{
    size_t c1 = videoId.find(':');
    if (c1 == std::string::npos) return -1;
    size_t c2 = videoId.find(':', c1 + 1);
    if (c2 == std::string::npos) return -1;
    std::string seg = videoId.substr(c1 + 1, c2 - c1 - 1);
    if (seg.empty() ||
        seg.find_first_not_of("0123456789") != std::string::npos)
        return -1;
    return std::atoi(seg.c_str());
}

// Where playback should resume from a stored offset (ms): far enough in to be
// worth it, but not within the last few percent -- a finished video restarts
// from the top, like Stremio itself. Rewinds a few seconds for context.
double resumeFrom(double offMs, double durMs)
{
    if (offMs <= 0) return 0.0;
    double pos = offMs / 1000.0;
    if (pos < 60.0) return 0.0;
    if (durMs > 0 && pos >= durMs / 1000.0 * 0.96) return 0.0;
    return pos - 5.0;
}

void showStreams(const stremio::Addon& addon, const std::string& type,
                 const std::string& videoId, const std::string& label,
                 const PlayerArt& art, const WatchInfo& watch);

// Addons that can serve a stream for this type. One list, one tap.
void showAddons(const std::string& authKey, const std::string& type,
                const std::string& videoId, const std::string& label,
                const PlayerArt& art, const WatchInfo& watch)
{
    brls::Application::pushActivity(
        new LoadingActivity("Addons...", label, art.posterPath));

    stremio::fetchAddonsAsync(authKey, [type, videoId, label, art, watch](
                                           stremio::AddonsResult r) {
        if (!r.ok)
        {
            closeLoading();
            dialog("Addons unavailable: " + r.error);
            return;
        }

        // Addons that declare "stream" but can never give us anything playable:
        // we only support BitTorrent sources (infoHash), and these serve local
        // files or their own hosted/P2P transports instead. Listing them just
        // leads to an empty screen after a tap.
        static const char* kUnsupported[] = { "WatchHub", "Local Files", "Peario" };

        auto usable = std::make_shared<std::vector<stremio::Addon>>();
        for (const auto& a : r.addons)
        {
            if (!a.hasStream || !a.supportsType(type)) continue;
            bool skip = false;
            for (const char* bad : kUnsupported)
                if (a.name.find(bad) != std::string::npos) { skip = true; break; }
            if (skip) continue;
            usable->push_back(a);
        }

        if (usable->empty())
        {
            closeLoading();
            dialog("No addon provides a source for this title.");
            return;
        }

        std::vector<Row> rows;
        for (const auto& a : *usable) { Row row; row.label = a.name; rows.push_back(row); }

        swapLoading(new ListActivity(
            label, "Pick an addon", rows,
            [usable, type, videoId, label, art, watch](int i) {
                showStreams((*usable)[i], type, videoId, label, art, watch);
            },
            art.posterPath));
    });
}

// A 4K source, as far as an addon label goes. Addons have no structured
// quality field -- it is whatever they wrote in the name/title -- so this
// matches on the tags they all use in practice.
bool isFourK(const stremio::Stream& s)
{
    std::string hay = s.name + " " + s.title;
    for (char& c : hay) c = (char)tolower((unsigned char)c);
    return hay.find("2160") != std::string::npos ||
           hay.find("4k") != std::string::npos ||
           hay.find("uhd") != std::string::npos;
}

// The sources one addon offers for this video.
void showStreams(const stremio::Addon& addon, const std::string& type,
                 const std::string& videoId, const std::string& label,
                 const PlayerArt& art, const WatchInfo& watch)
{
    brls::Application::pushActivity(
        new LoadingActivity("Sources...", label, art.posterPath));

    stremio::fetchStreamsAsync(
        addon.base, type, videoId, [label, art, watch](stremio::StreamsResult r) {
            if (!r.ok)
            {
                closeLoading();
                dialog("Sources unavailable: " + r.error);
                return;
            }
            if (r.streams.empty())
            {
                closeLoading();
                dialog("This addon has no source for this title.");
                return;
            }

            // The Switch outputs 1080p docked, so a 4K stream costs bandwidth
            // for pixels it cannot show -- and they are the heaviest files in
            // the swarm. Hidden by default, see Options.
            auto streams = std::make_shared<std::vector<stremio::Stream>>();
            int hidden4k = 0;
            for (const auto& s : r.streams)
            {
                if (config::get().hide4k && isFourK(s)) { hidden4k++; continue; }
                streams->push_back(s);
            }
            if (streams->empty())
            {
                closeLoading();
                dialog(hidden4k > 0
                           ? "This addon only offers 4K sources, which are "
                             "hidden in Options."
                           : "This addon has no source for this title.");
                return;
            }

            std::vector<Row> rows;
            for (const auto& s : *streams)
            {
                // name  -> "Torrentio" + quality tag
                // title -> file name, then the useful line: seeders, size,
                //          provider. That line is what tells a source that will
                //          actually stream from one that will crawl, so it goes
                //          on the right where it can be compared at a glance.
                auto nameLines  = splitLines(s.name);
                auto titleLines = splitLines(s.title);

                Row row;
                row.label = deEmoji(joinSpace(nameLines));
                if (row.label.empty() && !titleLines.empty()) row.label = titleLines[0];
                if (row.label.empty()) row.label = "Source";

                // The whole title the addon gives (filename plus seeders, size,
                // provider) rather than only the seeders/size line: that line
                // alone dropped the filename, which is the actual "which release
                // is this" information. Single-line, so it scrolls on focus.
                row.sub = deEmoji(joinSpace(titleLines));

                // Only BitTorrent sources are playable by our engine; say so up
                // front rather than failing after the tap.
                if (s.infoHash.empty())
                    row.detail = "unsupported";

                rows.push_back(row);
            }

            swapLoading(new ListActivity(
                label, "Pick a source", rows,
                [streams, art, label, watch](int i) {
                    const auto& s = (*streams)[i];
                    if (s.infoHash.empty())
                    {
                        dialog("Unsupported source: only torrents (infoHash) "
                               "can be played for now.");
                        return;
                    }
                    // Addons hand back a bare infoHash. Our magnet loader
                    // needs trackers -- it announces to them to find the peers
                    // that serve the metadata, and rejects a trackerless magnet
                    // outright (no DHT bootstrap for metadata yet). So attach
                    // the usual public trackers, which is what a bare infoHash
                    // relies on everywhere else too.
                    std::string magnet = "magnet:?xt=urn:btih:" + s.infoHash +
                                         kPublicTrackers;
                    brls::Logger::info("[stremio] play {}", magnet);
                    // At EOF, pop the player + this source list + the addon list
                    // to land back on the library (film) or the episode list
                    // (series).
                    WatchInfo w = watch;
                    w.endPop    = 3;
                    // The addon's file index picks the right episode inside a
                    // season pack (or all-seasons torrent); -1 = largest file.
                    brls::Application::pushActivity(
                        new PlayerActivity(magnet, art, label, s.fileIdx, w));
                },
                art.posterPath));
        });
}

// Episodes of one season. `item` is the library entry: its watch state says
// which episode was last watched and where in it the account stopped.
void showEpisodes(const std::string& authKey, const std::string& type,
                  std::shared_ptr<std::vector<stremio::Video>> vids, int season,
                  const stremio::LibItem& item, const PlayerArt& art)
{
    auto eps = std::make_shared<std::vector<stremio::Video>>();
    for (const auto& v : *vids)
        if (v.season == season) eps->push_back(v);
    std::sort(eps->begin(), eps->end(),
              [](const stremio::Video& a, const stremio::Video& b) {
                  return a.episode < b.episode;
              });

    // Rows from the episodes, with the watch bar on whichever episode the show's
    // position currently points at. A local record of the last thing we played
    // (stremio::lastWatch) overrides the library snapshot, so returning from an
    // episode moves the bar without another fetch.
    std::string itemId  = item.id;
    std::string libVid  = item.videoId;
    double      libProg = item.progress();
    auto buildRows = [eps, itemId, libVid, libProg]() {
        stremio::LocalWatch lw = stremio::lastWatch();
        bool haveLocal = lw.itemId == itemId && lw.progress() >= 0.0;
        std::string vid  = haveLocal ? lw.videoId : libVid;
        double      prog = haveLocal ? lw.progress() : libProg;
        std::vector<Row> out;
        for (const auto& v : *eps)
        {
            Row row;
            row.label  = "Episode " + std::to_string(v.episode);
            row.sub    = v.title;
            row.artId  = v.thumbnail.empty() ? "" : v.id;
            row.artUrl = v.thumbnail;
            if (!vid.empty() && v.id == vid) row.progress = prog;
            out.push_back(row);
        }
        return out;
    };
    std::vector<Row> rows = buildRows();

    std::string title = item.name + " - Season " + std::to_string(season);
    auto* epList = new ListActivity(
        title, "Pick an episode", rows,
        [authKey, type, eps, title, item, art](int i) {
            // The show's poster stays in front (it identifies the series at a
            // glance); the background becomes this episode's own still, which is
            // what Stremio shows for an episode.
            PlayerArt epArt = art;
            if (!(*eps)[i].thumbnail.empty())
            {
                epArt.bgId   = (*eps)[i].id;
                epArt.bgUrl  = (*eps)[i].thumbnail;
                epArt.blurBg = false;  // a still is already busy and low-contrast
            }
            WatchInfo watch;
            watch.authKey = authKey;
            watch.itemId  = item.id;
            watch.videoId = (*eps)[i].id;
            // Shown top-left on pause: episode title, then "season · episode".
            watch.displayTitle =
                (*eps)[i].title.empty() ? item.name : (*eps)[i].title;
            watch.displayTitle += "  (" + std::to_string((*eps)[i].season) +
                                  " · " + std::to_string((*eps)[i].episode) + ")";
            // Resume only in the episode the account actually stopped in.
            if ((*eps)[i].id == item.videoId)
                watch.resumeSec = resumeFrom(item.timeOffsetMs, item.durationMs);
            std::string epTitle = title + " · Episode " + std::to_string((*eps)[i].episode);
            showAddons(authKey, type, (*eps)[i].id, epTitle, epArt, watch);
        },
        art.posterPath);
    epList->setRebuildOnReturn(buildRows);

    // Land the cursor on the episode the account is mid-way through, so opening
    // a started show drops you right on it. The local record (just-played) wins
    // over the library snapshot, same as the bar above.
    stremio::LocalWatch lw = stremio::lastWatch();
    std::string focusVid   = (lw.itemId == item.id && lw.progress() >= 0.0)
                                 ? lw.videoId
                                 : item.videoId;
    if (!focusVid.empty())
        for (size_t i = 0; i < eps->size(); i++)
            if ((*eps)[i].id == focusVid)
            {
                epList->setInitialFocus((int)i);
                break;
            }
    brls::Application::pushActivity(epList);
}

} // namespace

ListActivity::ListActivity(std::string title, std::string subtitle,
                           std::vector<Row> rows, std::function<void(int)> onSelect,
                           std::string iconPath)
    : title(std::move(title))
    , subtitle(std::move(subtitle))
    , iconPath(std::move(iconPath))
    , rows(std::move(rows))
    , onSelect(std::move(onSelect))
{
}

brls::View* ListActivity::createContentView()
{
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setGrow(1.0f);
    // setPadding(top, right, bottom, left).
    //  - no bottom padding: the list scrolls, so it only wasted a strip of
    //    screen and left the last row floating. The breathing room goes under
    //    the last row instead (below).
    //  - no right padding: ScrollingFrame pins its indicator to its OWN right
    //    edge (getWidth() - 14), so any padding here pulled the bar inwards on
    //    top of the row text. The frame reaches the screen edge and the rows
    //    carry the inset instead (see RowCell).
    box->setPadding(20.0f, 0.0f, 0.0f, 60.0f);

    if (!subtitle.empty())
    {
        auto* s = new brls::Label();
        s->setText(subtitle);
        s->setFontSize(19);
        s->setTextColor(nvgRGB(190, 190, 195));
        s->setMargins(0, 60.0f, 14, 0);  // matches the inset the rows carry
        box->addView(s);
    }

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    // CENTERED, not the default NATURAL: only CENTERED scrolls the frame to the
    // focused row -- both on appear (so opening a started show lands on the
    // in-progress episode even when it is far down the list) and as you navigate.
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* list = new brls::Box();
    list->setAxis(brls::Axis::COLUMN);
    listBox = list;
    populate(list);
    if (initialFocus > 0 && initialFocus < (int)rows.size())
        list->setDefaultFocusedIndex(initialFocus);
    scroll->setContentView(list);
    box->addView(scroll);

    // A list that can restate its rows watches for the watch-state to change
    // (playback pushing new progress) and rebuilds when focus returns to it.
    seenGen = stremio::libraryGen();
    if (rebuildRows)
    {
        focusSub    = brls::Application::getGlobalFocusChangeEvent()->subscribe(
            [this](brls::View* v) { onGlobalFocus(v); });
        focusSubbed = true;
    }

    auto* frame = new brls::AppletFrame();
    frame->pushContentView(box);
    // After pushContentView: it overwrites the title with the content view's.
    frame->setTitle(title);
    if (!iconPath.empty())
    {
        frame->setIcon(iconPath);
        // The header icon is sized "auto", i.e. from the texture -- a poster is
        // several hundred pixels tall and would blow the header open. Pin it to
        // 2:3 inside the 88px header.
        auto* ic = dynamic_cast<brls::Image*>(
            frame->getView("brls/applet_frame/title_icon"));
        if (ic)
        {
            ic->setDimensions(40.0f, 60.0f);
            ic->setScalingType(brls::ImageScalingType::FIT);
        }
    }
    return frame;
}

ListActivity::~ListActivity()
{
    *alive = false;
    if (focusSubbed)
        brls::Application::getGlobalFocusChangeEvent()->unsubscribe(focusSub);
}

void ListActivity::setRebuildOnReturn(std::function<std::vector<Row>()> rebuild)
{
    rebuildRows = std::move(rebuild);
}

void ListActivity::populate(brls::Box* list)
{
    brls::Box* lastRow = nullptr;
    for (size_t i = 0; i < rows.size(); i++)
    {
        auto cb = onSelect;
        int idx = (int)i;
        auto* cell = new RowCell(rows[i], [cb, idx]() { cb(idx); });
        list->addView(cell);
        lastRow = cell;
    }
    // Scrolled to the end, the last row sits above the screen edge instead of
    // against it. On the row rather than on `list`: setContentView() ignores
    // that box's own margins.
    if (lastRow) lastRow->setMarginBottom(32.0f);
}

void ListActivity::onGlobalFocus(brls::View* focused)
{
    // Rebuild only when the watch-state has actually changed since we last drew,
    // and only as focus returns into our own list (not while it is buried under
    // deeper screens). Generation-tracked so the rebuild -- which moves focus and
    // re-fires this -- settles instead of looping.
    if (!listBox || !rebuildRows || !focused || !isUnder(focused, listBox))
        return;
    if (stremio::libraryGen() == seenGen) return;
    seenGen = stremio::libraryGen();

    // Defer: we are inside a focus-change dispatch (an activity above just
    // popped and restored focus here, e.g. the player closing at end of an
    // episode). clearViews() would free the row that is currently focused,
    // and the crash is the next giveFocus() dereferencing it. Run on the next
    // UI tick, and move focus off the list first so the rebuild frees nothing
    // live.
    auto live = alive;
    brls::sync([this, live]() {
        if (!*live || !listBox || !rebuildRows) return;
        brls::View* cur = brls::Application::getCurrentFocus();
        bool refocus    = cur && isUnder(cur, listBox);
        // Park focus on the (alive) listBox before clearViews frees the focused
        // row, so the following giveFocus never touches a freed view.
        if (refocus)
        {
            listBox->setFocusable(true);
            listBox->setHideHighlight(true);
            brls::Application::giveFocus(listBox);
        }
        rows = rebuildRows();
        listBox->clearViews();
        populate(listBox);
        if (refocus)
        {
            listBox->setFocusable(false);
            if (!listBox->getChildren().empty())
                brls::Application::giveFocus(listBox->getChildren()[0]);
        }
    });
}

void openLibraryItem(const std::string& authKey, const stremio::LibItem& item)
{
    // The library list already pulled this poster into the cache, so a hit is
    // the norm; on a miss the player just falls back to the app logo rather
    // than us blocking the tap on a download.
    PlayerArt art;
    art.posterPath = stremio::cachedPosterPath(item.id);
    // The background: the poster again, full-size and blurred. An episode
    // overrides this with its own still further down (see showEpisodes).
    art.bgId   = item.id;
    art.bgUrl  = item.poster;
    art.blurBg = true;

    // A film has no season/episode tree -- go straight to the sources.
    if (item.type != "series")
    {
        WatchInfo watch;
        watch.authKey      = authKey;
        watch.itemId       = item.id;
        watch.videoId      = item.videoId.empty() ? item.id : item.videoId;
        watch.displayTitle = item.name;  // shown top-left on pause
        watch.resumeSec    = resumeFrom(item.timeOffsetMs, item.durationMs);
        showAddons(authKey, item.type, item.id, item.name, art, watch);
        return;
    }

    brls::Application::pushActivity(
        new LoadingActivity("Episodes...", item.name, art.posterPath));

    // The episode list comes from a meta addon (Cinemeta by default). Pick the
    // user's own first so a custom catalogue still resolves, and fall back to
    // Cinemeta rather than dead-ending if none of theirs serves meta.
    stremio::fetchAddonsAsync(
        authKey, [authKey, item, art](stremio::AddonsResult ar) {
        std::string metaBase = "https://v3-cinemeta.strem.io";
        if (ar.ok)
            for (const auto& a : ar.addons)
                if (a.hasMeta && a.supportsType("series")) { metaBase = a.base; break; }

        stremio::fetchMetaAsync(
            metaBase, "series", item.id,
            [authKey, item, art](stremio::MetaResult mr) {
                if (!mr.ok || mr.videos.empty())
                {
                    closeLoading();
                    dialog(mr.ok ? "No episode found for this show."
                                 : ("Episodes unavailable: " + mr.error));
                    return;
                }

                auto vids =
                    std::make_shared<std::vector<stremio::Video>>(mr.videos);

                // Seasons, in order. Season 0 is Stremio's "specials" bucket.
                std::map<int, int> count;  // season -> episodes
                for (const auto& v : *vids)
                    if (v.season >= 0) count[v.season]++;

                auto seasons = std::make_shared<std::vector<int>>();
                std::vector<Row> rows;
                for (const auto& kv : count)
                {
                    seasons->push_back(kv.first);
                    Row row;
                    row.label  = kv.first == 0
                                     ? std::string("Specials")
                                     : "Season " + std::to_string(kv.first);
                    row.detail = std::to_string(kv.second) + " episodes";
                    rows.push_back(row);
                }

                auto* seasonsAct = new ListActivity(
                    item.name, "Pick a season", rows,
                    [authKey, vids, seasons, item, art](int i) {
                        showEpisodes(authKey, "series", vids, (*seasons)[i],
                                     item, art);
                    },
                    art.posterPath);

                // A show with an episode in progress opens directly on that
                // episode's season. The seasons list is pushed underneath, so B
                // from the episodes walks back to it as usual.
                int cur       = seasonOfVideoId(item.videoId);
                bool autoJump = cur >= 0 && count.count(cur) > 0;
                brls::Application::popActivity(
                    brls::TransitionAnimation::NONE,
                    [seasonsAct, autoJump, authKey, vids, cur, item, art]() {
                        brls::Application::pushActivity(seasonsAct);
                        if (autoJump)
                            showEpisodes(authKey, "series", vids, cur, item,
                                         art);
                    });
            });
    });
}
