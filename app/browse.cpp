#include "browse.hpp"

#include <borealis/views/applet_frame.hpp>
#include <borealis/views/dialog.hpp>
#include <borealis/views/h_scrolling_frame.hpp>
#include <borealis/views/image.hpp>
#include <borealis/views/scrolling_frame.hpp>

#include <algorithm>
#include <cmath>
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

// The account's addons that can actually serve a playable source for `type`.
// Drops the ones with no "stream" resource, the ones scoped to another type, and
// the few that declare "stream" but only ever hand back things our BitTorrent
// engine cannot play (local files / their own hosted transports) -- listing
// those just leads to an empty screen after a tap.
std::vector<stremio::Addon> usableStreamAddons(
    const std::vector<stremio::Addon>& all, const std::string& type)
{
    static const char* kUnsupported[] = { "WatchHub", "Local Files", "Peario" };
    std::vector<stremio::Addon> out;
    for (const auto& a : all)
    {
        if (!a.hasStream || !a.supportsType(type)) continue;
        bool skip = false;
        for (const char* bad : kUnsupported)
            if (a.name.find(bad) != std::string::npos) { skip = true; break; }
        if (skip) continue;
        out.push_back(a);
    }
    return out;
}

// Addons that can serve a stream for this type. One list, one tap. (Films get
// the richer MovieDetailActivity instead; this is the episode/series path.)
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

        auto usable = std::make_shared<std::vector<stremio::Addon>>(
            usableStreamAddons(r.addons, type));

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
// Superseded by SeriesDetailActivity / EpisodeDetailActivity; kept (with the
// showAddons/showStreams chain it drives) as the ListActivity-based fallback.
[[maybe_unused]] void showEpisodes(
    const std::string& authKey, const std::string& type,
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

// Cinemeta gives a runtime as "127 min" (occasionally already "2h 7min"). Show
// it the way the design does: "2h 7min".
std::string formatRuntime(const std::string& s)
{
    if (s.empty() || s.find('h') != std::string::npos) return s;
    int mins = std::atoi(s.c_str());
    if (mins <= 0) return s;
    int h = mins / 60, m = mins % 60;
    if (h <= 0) return std::to_string(m) + "min";
    return std::to_string(h) + "h " + std::to_string(m) + "min";
}

// Trims a synopsis to keep the panel from growing without bound, cutting on a
// word (and never through a UTF-8 sequence) and adding an ellipsis.
std::string clampText(const std::string& s, size_t max)
{
    if (s.size() <= max) return s;
    size_t cut = s.rfind(' ', max);
    if (cut == std::string::npos || cut < max / 2) cut = max;
    while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;  // utf-8 boundary
    std::string out = s.substr(0, cut);
    while (!out.empty() && (out.back() == ' ' || out.back() == ','))
        out.pop_back();
    return out + "…";
}

// AppletFrame that paints the design's dark-navy radial gradient behind the
// whole frame -- header gap, content and (transparent) footer alike -- so the
// footer no longer falls back to the flat theme colour. Everything on top draws
// over it; the content box is left transparent so the gradient shows through.
// The detail screens' navy background, defined once so the edge fades can sample
// the exact same field and blend into it seamlessly (see FadeHScrollingFrame).
const NVGcolor kBgInner = nvgRGB(0x25, 0x25, 0x4e);
const NVGcolor kBgOuter = nvgRGB(0x07, 0x07, 0x14);

NVGpaint bgGradient(NVGcontext* vg, float x, float y, float w, float h)
{
    return nvgRadialGradient(vg, x + w * 0.60f, y + h * 0.30f, h * 0.15f,
                             w * 0.90f, kBgInner, kBgOuter);
}

// The background colour at an absolute screen point, matching bgGradient over the
// full 1280x720 frame -- nvgRadialGradient interpolates linearly in
// (distance - inner)/(outer - inner), which this reproduces so the fade lands on
// the true background colour rather than a guessed one.
NVGcolor bgColorAt(float px, float py)
{
    const float W = 1280.0f, H = 720.0f;
    float cx = W * 0.60f, cy = H * 0.30f, inr = H * 0.15f, outr = W * 0.90f;
    float d  = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    float t  = (d - inr) / (outr - inr);
    t        = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return nvgRGBAf(kBgInner.r + (kBgOuter.r - kBgInner.r) * t,
                    kBgInner.g + (kBgOuter.g - kBgInner.g) * t,
                    kBgInner.b + (kBgOuter.b - kBgInner.b) * t, 1.0f);
}

class GradientAppletFrame : public brls::AppletFrame
{
  public:
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override
    {
        nvgSave(vg);
        nvgBeginPath(vg);
        nvgRect(vg, x, y, width, height);
        nvgFillPaint(vg, bgGradient(vg, x, y, width, height));
        nvgFill(vg);
        nvgRestore(vg);
        brls::AppletFrame::draw(vg, x, y, width, height, style, ctx);
    }
};

// A Material "star" glyph (U+E838) as UTF-8 bytes, for the rating chip.
const std::string kStar = "\xEE\xA0\xB8";

// The 4-digit year out of a release string ("2011-04-27" -> "2011"), unchanged
// if it does not start with one.
std::string formatYear(const std::string& s)
{
    if (s.size() >= 4 && s.find_first_not_of("0123456789") >= 4) return s.substr(0, 4);
    return s;
}

// A horizontal scroller that fades its content out at whichever edge still has
// off-screen content, so cards/tabs dissolve into the background instead of
// hard-cutting. The fade is a background-coloured scrim drawn over the content
// edges; it is suppressed on an edge with nothing beyond it (so the first tab at
// rest is never dimmed).
class FadeHScrollingFrame : public brls::HScrollingFrame
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

        const float fade = 56.0f;
        float offset     = getContentOffsetX();
        float rightLimit = content ? content->getWidth() - w : 0.0f;
        float midY       = y + h * 0.5f;

        // The scrim is the true background colour sampled AT the edge, fading to
        // transparent inward -- so content dissolves into the background instead
        // of into a flat block.
        if (offset > 2.0f)  // scrolled: content runs off the left
        {
            NVGcolor edge = bgColorAt(x, midY);
            NVGcolor gone  = edge; gone.a = 0.0f;
            NVGpaint p = nvgLinearGradient(vg, x, y, x + fade, y, edge, gone);
            nvgBeginPath(vg);
            nvgRect(vg, x, y, fade, h);
            nvgFillPaint(vg, p);
            nvgFill(vg);
        }
        if (offset < rightLimit - 2.0f)  // more content off the right
        {
            NVGcolor edge = bgColorAt(x + w, midY);
            NVGcolor gone  = edge; gone.a = 0.0f;
            NVGpaint p =
                nvgLinearGradient(vg, x + w - fade, y, x + w, y, gone, edge);
            nvgBeginPath(vg);
            nvgRect(vg, x + w - fade, y, fade, h);
            nvgFillPaint(vg, p);
            nvgFill(vg);
        }
    }

  private:
    brls::View* content = nullptr;
};

// Base for the film / episode source screens: an addon switcher (a tab bar, like
// the menus) whose selected addon's playable sources show as cards directly
// below. The subclass builds the page-specific header, creates addonBar +
// sourcesRow and places them, sets the playback context (type/videoId/label/
// art/watch/authKey), then calls startAddonSources().
class AddonSourcePicker : public brls::Activity
{
  public:
    // Async fetches (addons / streams, plus the subclass's own) can land after
    // B pops us.
    ~AddonSourcePicker() override { *alive = false; }

  protected:
    std::string authKey, type, videoId, label;
    PlayerArt art;
    WatchInfo watch;
    brls::Box* addonBar   = nullptr;  // subclass creates + places these two
    brls::Box* sourcesRow = nullptr;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    void startAddonSources() { loadAddons(); }

    // One button per usable addon, the first selected. selectAddon does the
    // per-addon work (highlight + load its sources).
    void loadAddons()
    {
        auto live = alive;
        stremio::fetchAddonsAsync(authKey, [this, live](stremio::AddonsResult r) {
            if (!*live) return;
            if (!r.ok) { showSourcesMessage("Addons unavailable", true); return; }
            addons = std::make_shared<std::vector<stremio::Addon>>(
                usableStreamAddons(r.addons, type));
            if (addons->empty())
            {
                showSourcesMessage("No addon provides a source for this title",
                                   true);
                return;
            }
            for (size_t i = 0; i < addons->size(); i++)
            {
                auto* b = new brls::Button();
                b->setText((*addons)[i].name);
                b->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
                if (i) b->setMarginLeft(6.0f);
                int idx = (int)i;
                b->registerClickAction([this, idx](brls::View*) {
                    selectAddon(idx);
                    return true;
                });
                addonBar->addView(b);
                addonBtns.push_back(b);
            }
            addonBtns.back()->setMarginRight(40.0f);
            brls::Application::giveFocus(addonBtns[0]);
            selectAddon(0);
        });
    }

    void selectAddon(int i)
    {
        if (!addons || i < 0 || i >= (int)addons->size()) return;
        activeAddon = i;
        for (size_t k = 0; k < addonBtns.size(); k++)
            addonBtns[k]->setStyle((int)k == i ? &brls::BUTTONSTYLE_PRIMARY
                                               : &brls::BUTTONSTYLE_BORDERLESS);
        auto hit = streamCache.find(i);
        if (hit != streamCache.end()) { buildSourceCards(hit->second); return; }

        showSourcesMessage("Loading sources...", false);
        auto live = alive;
        stremio::fetchStreamsAsync(
            (*addons)[i].base, type, videoId,
            [this, live, i](stremio::StreamsResult r) {
                if (!*live || activeAddon != i) return;  // switched away meanwhile
                if (!r.ok) { showSourcesMessage("Sources unavailable", false); return; }
                // The Switch outputs 1080p docked, so 4K streams cost bandwidth
                // for pixels it cannot show; hidden by default (Options).
                std::vector<stremio::Stream> playable;
                for (const auto& s : r.streams)
                    if (!(config::get().hide4k && isFourK(s)))
                        playable.push_back(s);
                streamCache[i] = playable;
                if (playable.empty())
                {
                    showSourcesMessage("No source from this addon", false);
                    return;
                }
                buildSourceCards(playable);
            });
    }

    void buildSourceCards(const std::vector<stremio::Stream>& streams)
    {
        parkFocusOffSources();
        sourcesRow->setFocusable(false);
        sourcesRow->clearViews();
        for (size_t i = 0; i < streams.size(); i++)
        {
            auto* card = makeSourceCard(streams[i], i > 0);
            if (activeAddon >= 0 && activeAddon < (int)addonBtns.size())
                card->setCustomNavigationRoute(brls::FocusDirection::UP,
                                               addonBtns[activeAddon]);
            sourcesRow->addView(card);
        }
        if (!sourcesRow->getChildren().empty())
            sourcesRow->getChildren().back()->setMarginRight(60.0f);
        if (activeAddon >= 0 && activeAddon < (int)addonBtns.size() &&
            !sourcesRow->getChildren().empty())
            addonBtns[activeAddon]->setCustomNavigationRoute(
                brls::FocusDirection::DOWN, sourcesRow->getChildren()[0]);
    }

    brls::Box* makeSourceCard(const stremio::Stream& s, bool marginLeft)
    {
        auto nameLines  = splitLines(s.name);
        auto titleLines = splitLines(s.title);
        std::string head = deEmoji(joinSpace(nameLines));
        if (head.empty() && !titleLines.empty()) head = titleLines[0];
        if (head.empty()) head = "Source";
        std::string info = deEmoji(joinSpace(titleLines));

        auto* card = new brls::Box();
        card->setFocusable(true);
        card->setAxis(brls::Axis::COLUMN);
        card->setWidth(432.0f);
        card->setHeight(200.0f);
        card->setCornerRadius(10.0f);
        card->setBackgroundColor(nvgRGB(0x2c, 0x2c, 0x31));
        card->setPadding(24.0f, 26.0f, 24.0f, 28.0f);
        if (marginLeft) card->setMarginLeft(28.0f);

        stremio::Stream stream = s;
        PlayerArt cardArt      = art;
        WatchInfo cardW        = watch;
        std::string cardLabel  = label;
        card->registerClickAction(
            [stream, cardArt, cardW, cardLabel](brls::View*) {
                if (stream.infoHash.empty())
                {
                    dialog("Unsupported source: only torrents (infoHash) can be "
                           "played for now.");
                    return true;
                }
                std::string magnet =
                    "magnet:?xt=urn:btih:" + stream.infoHash + kPublicTrackers;
                brls::Logger::info("[stremio] play {}", magnet);
                WatchInfo w = cardW;
                // Player sits directly on the detail screen: pop both at EOF to
                // land back on the library (film) or the series screen (episode).
                w.endPop = 2;
                brls::Application::pushActivity(new PlayerActivity(
                    magnet, cardArt, cardLabel, stream.fileIdx, w));
                return true;
            });
        card->addGestureRecognizer(new brls::TapGestureRecognizer(card));

        auto* h = new brls::Label();
        h->setText(head);
        h->setFontSize(24.0f);
        h->setSingleLine(true);
        card->addView(h);
        if (!info.empty())
        {
            auto* d = new brls::Label();
            d->setText(clampText(info, 150));
            d->setFontSize(17.0f);
            d->setTextColor(nvgRGB(170, 170, 178));
            d->setMarginTop(10.0f);
            card->addView(d);
        }
        if (stream.infoHash.empty())
        {
            auto* u = new brls::Label();
            u->setText("unsupported");
            u->setFontSize(15.0f);
            u->setTextColor(nvgRGB(205, 140, 140));
            u->setMarginTop(8.0f);
            card->addView(u);
        }
        return card;
    }

    void parkFocusOffSources()
    {
        brls::View* cur = brls::Application::getCurrentFocus();
        if (!cur || !isUnder(cur, sourcesRow)) return;
        if (activeAddon >= 0 && activeAddon < (int)addonBtns.size())
            brls::Application::giveFocus(addonBtns[activeAddon]);
        else
        {
            sourcesRow->setFocusable(true);
            sourcesRow->setHideHighlight(true);
            brls::Application::giveFocus(sourcesRow);
        }
    }

    void showSourcesMessage(const std::string& msg, bool focusable)
    {
        parkFocusOffSources();
        sourcesRow->setFocusable(false);
        sourcesRow->clearViews();
        auto* box = new brls::Box();
        box->setFocusable(focusable);
        if (focusable) box->setHideHighlight(true);
        box->setHeight(200.0f);
        box->setJustifyContent(brls::JustifyContent::CENTER);
        box->setAlignItems(brls::AlignItems::CENTER);
        auto* l = new brls::Label();
        l->setText(msg);
        l->setFontSize(20.0f);
        l->setTextColor(nvgRGB(150, 150, 155));
        box->addView(l);
        sourcesRow->addView(box);
        if (focusable && !brls::Application::getCurrentFocus())
            brls::Application::giveFocus(box);
    }

  private:
    std::shared_ptr<std::vector<stremio::Addon>> addons;
    std::vector<brls::Button*> addonBtns;
    std::map<int, std::vector<stremio::Stream>> streamCache;
    int activeAddon = -1;
};

// The film detail screen: a large, full-resolution poster on the left; the
// title, year, runtime and rating over a synopsis on the right; then the addon
// switcher + its sources (from AddonSourcePicker) filling the right column.
class MovieDetailActivity : public AddonSourcePicker
{
  public:
    MovieDetailActivity(std::string ak, stremio::LibItem it, PlayerArt ar,
                        WatchInfo w)
        : item(std::move(it))
    {
        authKey = std::move(ak);
        art     = std::move(ar);
        watch   = std::move(w);
        type    = item.type;  // playback context for the base picker
        videoId = item.id;
        label   = item.name;
    }

    brls::View* createContentView() override
    {
        auto* root = new brls::Box();
        root->setAxis(brls::Axis::ROW);
        root->setGrow(1.0f);
        // No right padding: the addon and source rows run all the way to the
        // screen edge so their overflow scrolls off it (the text blocks carry
        // their own right margin instead).
        root->setPadding(82.0f, 0.0f, 40.0f, 64.0f);  // top,right,bottom,left

        // ---- poster (left) ----------------------------------------------
        poster = new brls::Image();
        poster->setDimensions(324.0f, 486.0f);  // 2:3
        poster->setScalingType(brls::ImageScalingType::FIT);
        poster->setCornerRadius(10.0f);
        poster->setShrink(0.0f);
        // The cached thumbnail fills the slot at once so it is never blank; the
        // full-size art swaps in when it lands (loadPoster) -- the point of this
        // screen is that the poster is shown large and uncompressed.
        if (!art.posterPath.empty())
            poster->setImageFromFile(art.posterPath);
        root->addView(poster);

        // ---- details (right) --------------------------------------------
        auto* right = new brls::Box();
        right->setAxis(brls::Axis::COLUMN);
        right->setGrow(1.0f);
        right->setMarginLeft(56.0f);

        // The text blocks keep a right margin off the screen edge; the scrolling
        // rows below deliberately do not (they run to the edge).
        auto* titleL = new brls::Label();
        titleL->setText(item.name);
        titleL->setFontSize(46.0f);
        titleL->setSingleLine(true);
        titleL->setMarginRight(60.0f);
        right->addView(titleL);

        metaLine = new brls::Label();
        metaLine->setText(item.type == "series" ? "Show" : "Film");
        metaLine->setFontSize(19.0f);
        metaLine->setTextColor(nvgRGB(205, 205, 212));
        metaLine->setMarginTop(14.0f);
        metaLine->setMarginRight(60.0f);
        right->addView(metaLine);

        descLabel = new brls::Label();
        descLabel->setText("");
        descLabel->setFontSize(18.0f);
        descLabel->setTextColor(nvgRGB(198, 198, 206));
        descLabel->setMarginTop(18.0f);
        descLabel->setMarginRight(60.0f);
        right->addView(descLabel);

        // ---- addon tab bar (scrolls horizontally, like the sources) ------
        auto* addonScroll = new FadeHScrollingFrame();
        addonScroll->setHeight(64.0f);  // room for the button focus glow
        addonScroll->setMarginTop(28.0f);
        addonScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        addonBar = new brls::Box();
        addonBar->setAxis(brls::Axis::ROW);
        addonBar->setAlignItems(brls::AlignItems::CENTER);
        addonScroll->setContentView(addonBar);
        right->addView(addonScroll);

        // ---- the selected addon's sources (scrollable row) --------------
        auto* hs = new FadeHScrollingFrame();
        hs->setHeight(210.0f);
        hs->setMarginTop(16.0f);
        hs->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        sourcesRow = new brls::Box();
        sourcesRow->setAxis(brls::Axis::ROW);
        sourcesRow->setAlignItems(brls::AlignItems::CENTER);
        hs->setContentView(sourcesRow);
        right->addView(hs);

        // A focusable placeholder so borealis has a focus target until the addon
        // list arrives (loadAddons then moves focus onto the tab bar).
        showSourcesMessage("Loading...", true);

        root->addView(right);

        auto* frame = new GradientAppletFrame();
        frame->pushContentView(root);
        // No header bar: the title lives in the panel (Horizon-immersive, as
        // designed). The footer -- battery/wifi/clock and the button hints --
        // stays, now over the gradient.
        frame->setHeaderVisibility(brls::Visibility::GONE);

        loadPoster();
        loadMeta();
        startAddonSources();
        return frame;
    }

  private:
    void loadPoster()
    {
        auto live = alive;
        stremio::fetchHqArtAsync(item.id, item.poster,
                                 [this, live](std::string path) {
                                     if (!*live || path.empty() || !poster) return;
                                     poster->setImageFromFile(path);
                                 });
    }

    void loadMeta()
    {
        auto live = alive;
        // Cinemeta is the default meta provider and keys on the IMDB id every
        // library film carries, so go straight to it rather than resolving an
        // addon first.
        stremio::fetchMetaAsync(
            "https://v3-cinemeta.strem.io", "movie", item.id,
            [this, live](stremio::MetaResult r) {
                if (!*live || !r.ok) return;
                std::string line;
                auto add = [&](const std::string& s) {
                    if (s.empty()) return;
                    if (!line.empty()) line += "     ";
                    line += s;
                };
                add(r.releaseInfo);
                add(formatRuntime(r.runtime));
                if (!r.imdbRating.empty())
                    add(" " + r.imdbRating);  // Material "star" glyph
                if (!line.empty()) metaLine->setText(line);
                if (!r.description.empty())
                    descLabel->setText(clampText(r.description, 210));  // ~3 lines
            });
    }

    stremio::LibItem item;
    brls::Image* poster    = nullptr;
    brls::Label* metaLine  = nullptr;
    brls::Label* descLabel = nullptr;
};

// The episode source screen: the still + episode title / meta / synopsis across
// the top, then the addon switcher and its sources (from AddonSourcePicker)
// full-width below. Reached by picking an episode on SeriesDetailActivity.
class EpisodeDetailActivity : public AddonSourcePicker
{
  public:
    EpisodeDetailActivity(std::string ak, stremio::Video episode,
                          std::string seriesRating, PlayerArt ar, WatchInfo w)
        : ep(std::move(episode)), seriesRating(std::move(seriesRating))
    {
        authKey = std::move(ak);
        art     = std::move(ar);
        watch   = std::move(w);
        type    = "series";
        videoId = ep.id;
        label   = ep.title.empty() ? ("Episode " + std::to_string(ep.episode))
                                    : ep.title;
    }

    brls::View* createContentView() override
    {
        auto* root = new brls::Box();
        root->setAxis(brls::Axis::COLUMN);
        root->setGrow(1.0f);
        root->setPadding(64.0f, 0.0f, 40.0f, 64.0f);

        // ---- top row: still + episode meta ------------------------------
        auto* top = new brls::Box();
        top->setAxis(brls::Axis::ROW);
        top->setMarginRight(60.0f);

        auto* still = new brls::Image();
        still->setDimensions(360.0f, 202.0f);  // 16:9
        still->setScalingType(brls::ImageScalingType::FIT);
        still->setCornerRadius(8.0f);
        still->setShrink(0.0f);
        if (!ep.thumbnail.empty())
        {
            auto live = alive;
            stremio::fetchPosterAsync(
                ep.id, ep.thumbnail, [still, live](std::string p) {
                    if (*live && !p.empty()) still->setImageFromFile(p);
                });
        }
        top->addView(still);

        auto* meta = new brls::Box();
        meta->setAxis(brls::Axis::COLUMN);
        meta->setGrow(1.0f);
        meta->setMarginLeft(40.0f);
        meta->setJustifyContent(brls::JustifyContent::CENTER);

        auto* titleL = new brls::Label();
        titleL->setText(label);
        titleL->setFontSize(40.0f);
        titleL->setSingleLine(true);
        meta->addView(titleL);

        auto* metaLine = new brls::Label();
        std::string line;
        auto add = [&](const std::string& s) {
            if (s.empty()) return;
            if (!line.empty()) line += "     ";
            line += s;
        };
        add(formatYear(ep.released));
        add("Season " + std::to_string(ep.season));
        if (!seriesRating.empty()) add(kStar + " " + seriesRating);
        metaLine->setText(line);
        metaLine->setFontSize(18.0f);
        metaLine->setTextColor(nvgRGB(205, 205, 212));
        metaLine->setMarginTop(12.0f);
        meta->addView(metaLine);

        if (!ep.overview.empty())
        {
            auto* desc = new brls::Label();
            desc->setText(clampText(ep.overview, 210));
            desc->setFontSize(18.0f);
            desc->setTextColor(nvgRGB(198, 198, 206));
            desc->setMarginTop(12.0f);
            meta->addView(desc);
        }
        top->addView(meta);
        root->addView(top);

        // ---- addon tab bar (scrolls horizontally) -----------------------
        auto* addonScroll = new FadeHScrollingFrame();
        addonScroll->setHeight(64.0f);
        addonScroll->setMarginTop(36.0f);
        addonScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        addonBar = new brls::Box();
        addonBar->setAxis(brls::Axis::ROW);
        addonBar->setAlignItems(brls::AlignItems::CENTER);
        addonScroll->setContentView(addonBar);
        root->addView(addonScroll);

        // ---- sources ----------------------------------------------------
        auto* hs = new FadeHScrollingFrame();
        hs->setHeight(210.0f);
        hs->setMarginTop(16.0f);
        hs->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        sourcesRow = new brls::Box();
        sourcesRow->setAxis(brls::Axis::ROW);
        sourcesRow->setAlignItems(brls::AlignItems::CENTER);
        hs->setContentView(sourcesRow);
        root->addView(hs);

        showSourcesMessage("Loading...", true);

        auto* frame = new GradientAppletFrame();
        frame->pushContentView(root);
        frame->setHeaderVisibility(brls::Visibility::GONE);

        startAddonSources();
        return frame;
    }

  private:
    stremio::Video ep;
    std::string seriesRating;
};

// An episode still (the focusable card). While it holds the focus, its caption
// scrolls if the episode name is too long -- borealis only marquees a label that
// is itself focused, and the caption sits above the still, not inside it.
class EpisodeThumb : public brls::Box
{
  public:
    void onFocusGained() override
    {
        brls::Box::onFocusGained();
        if (caption) caption->setAnimated(true);
    }
    void onFocusLost() override
    {
        brls::Box::onFocusLost();
        if (caption) caption->setAnimated(false);
    }

    // The watch-progress bar, drawn here rather than as a child box so it can
    // span the full width flush to the edges while its bottom corners follow the
    // still's rounding (a rectangular child clip cannot round).
    void draw(NVGcontext* vg, float x, float y, float w, float h,
              brls::Style style, brls::FrameContext* ctx) override
    {
        brls::Box::draw(vg, x, y, w, h, style, ctx);
        if (progress <= 0.005f) return;
        const float R = 10.0f, barH = 12.0f;  // R matches the still's corners
        float top = y + h - barH;
        auto barPath = [&]() {
            nvgBeginPath(vg);
            nvgMoveTo(vg, x, top);
            nvgLineTo(vg, x + w, top);
            nvgLineTo(vg, x + w, y + h - R);
            nvgArcTo(vg, x + w, y + h, x + w - R, y + h, R);
            nvgLineTo(vg, x + R, y + h);
            nvgArcTo(vg, x, y + h, x, y + h - R, R);
            nvgClosePath(vg);
        };
        barPath();
        nvgFillColor(vg, nvgRGBA(0, 0, 0, 150));
        nvgFill(vg);

        float fw = w * (progress > 1.0f ? 1.0f : progress);
        nvgSave(vg);
        nvgIntersectScissor(vg, x, top, fw, barH);
        barPath();
        nvgFillColor(vg, brls::Application::getTheme().getColor("brls/accent"));
        nvgFill(vg);
        nvgRestore(vg);
    }

    brls::Label* caption = nullptr;
    float progress       = -1.0f;
};

// The series detail screen: poster on the left; title / years / rating over a
// synopsis on the right; then a season switcher (a tab bar) with that season's
// episodes as still cards below. Picking an episode opens EpisodeDetailActivity.
class SeriesDetailActivity : public brls::Activity
{
  public:
    SeriesDetailActivity(std::string authKey, stremio::LibItem item, PlayerArt art)
        : authKey(std::move(authKey))
        , item(std::move(item))
        , art(std::move(art))
    {
    }

    ~SeriesDetailActivity() override { *alive = false; }

    brls::View* createContentView() override
    {
        auto* root = new brls::Box();
        root->setAxis(brls::Axis::ROW);
        root->setGrow(1.0f);
        root->setPadding(82.0f, 0.0f, 40.0f, 64.0f);

        poster = new brls::Image();
        poster->setDimensions(324.0f, 486.0f);
        poster->setScalingType(brls::ImageScalingType::FIT);
        poster->setCornerRadius(10.0f);
        poster->setShrink(0.0f);
        if (!art.posterPath.empty()) poster->setImageFromFile(art.posterPath);
        root->addView(poster);

        auto* right = new brls::Box();
        right->setAxis(brls::Axis::COLUMN);
        right->setGrow(1.0f);
        right->setMarginLeft(56.0f);

        auto* titleL = new brls::Label();
        titleL->setText(item.name);
        titleL->setFontSize(46.0f);
        titleL->setSingleLine(true);
        titleL->setMarginRight(60.0f);
        right->addView(titleL);

        metaLine = new brls::Label();
        metaLine->setText("Show");
        metaLine->setFontSize(19.0f);
        metaLine->setTextColor(nvgRGB(205, 205, 212));
        metaLine->setMarginTop(14.0f);
        metaLine->setMarginRight(60.0f);
        right->addView(metaLine);

        descLabel = new brls::Label();
        descLabel->setText("");
        descLabel->setFontSize(18.0f);
        descLabel->setTextColor(nvgRGB(198, 198, 206));
        descLabel->setMarginTop(16.0f);
        descLabel->setMarginRight(60.0f);
        right->addView(descLabel);

        // ---- season tab bar ---------------------------------------------
        auto* seasonScroll = new FadeHScrollingFrame();
        seasonScroll->setHeight(64.0f);
        seasonScroll->setMarginTop(24.0f);
        seasonScroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        seasonBar = new brls::Box();
        seasonBar->setAxis(brls::Axis::ROW);
        seasonBar->setAlignItems(brls::AlignItems::CENTER);
        seasonScroll->setContentView(seasonBar);
        right->addView(seasonScroll);

        // ---- episodes of the selected season ----------------------------
        auto* hs = new FadeHScrollingFrame();
        hs->setHeight(262.0f);
        hs->setMarginTop(14.0f);
        hs->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        episodesRow = new brls::Box();
        episodesRow->setAxis(brls::Axis::ROW);
        episodesRow->setAlignItems(brls::AlignItems::CENTER);
        hs->setContentView(episodesRow);
        right->addView(hs);

        showEpisodesMessage("Loading...", true);

        root->addView(right);

        auto* frame = new GradientAppletFrame();
        frame->pushContentView(root);
        frame->setHeaderVisibility(brls::Visibility::GONE);

        loadPoster();
        loadMeta();
        return frame;
    }

  private:
    void loadPoster()
    {
        auto live = alive;
        stremio::fetchHqArtAsync(item.id, item.poster,
                                 [this, live](std::string path) {
                                     if (!*live || path.empty() || !poster) return;
                                     poster->setImageFromFile(path);
                                 });
    }

    // One Cinemeta fetch gives both the series' descriptive fields and its
    // episode list, so it drives the meta line, the synopsis and the seasons.
    void loadMeta()
    {
        auto live = alive;
        stremio::fetchMetaAsync(
            "https://v3-cinemeta.strem.io", "series", item.id,
            [this, live](stremio::MetaResult r) {
                if (!*live) return;
                if (!r.ok || r.videos.empty())
                {
                    showEpisodesMessage(
                        r.ok ? "No episodes found" : "Episodes unavailable", true);
                    return;
                }
                seriesRating = r.imdbRating;
                std::string line;
                auto add = [&](const std::string& s) {
                    if (s.empty()) return;
                    if (!line.empty()) line += "     ";
                    line += s;
                };
                add(r.releaseInfo);
                if (!r.imdbRating.empty()) add(kStar + " " + r.imdbRating);
                if (!line.empty()) metaLine->setText(line);
                if (!r.description.empty())
                    descLabel->setText(clampText(r.description, 150));

                videos = std::make_shared<std::vector<stremio::Video>>(r.videos);
                buildSeasonBar();
            });
    }

    void buildSeasonBar()
    {
        std::map<int, int> count;  // season -> episode count, sorted
        for (const auto& v : *videos)
            if (v.season >= 0) count[v.season]++;
        seasons.clear();
        for (const auto& kv : count) seasons.push_back(kv.first);
        if (seasons.empty()) { showEpisodesMessage("No episodes", true); return; }

        for (size_t i = 0; i < seasons.size(); i++)
        {
            auto* b = new brls::Button();
            b->setText(seasons[i] == 0 ? std::string("Specials")
                                       : "Season " + std::to_string(seasons[i]));
            b->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
            if (i) b->setMarginLeft(6.0f);
            int idx = (int)i;
            b->registerClickAction([this, idx](brls::View*) {
                selectSeason(idx, true);  // A on a season drops into its episodes
                return true;
            });
            seasonBar->addView(b);
            seasonBtns.push_back(b);
        }
        seasonBtns.back()->setMarginRight(40.0f);

        // Open on the season of the in-progress episode, else the first. Land on
        // the season bar (not the episodes) so the layout matches the other
        // screens; a deliberate A on a season is what dives into its episodes.
        int cur     = seasonOfVideoId(item.videoId);
        int initIdx = 0;
        for (size_t i = 0; i < seasons.size(); i++)
            if (seasons[i] == cur) { initIdx = (int)i; break; }
        brls::Application::giveFocus(seasonBtns[initIdx]);
        selectSeason(initIdx, false);
    }

    void selectSeason(int idx, bool focusEpisodes)
    {
        if (idx < 0 || idx >= (int)seasons.size()) return;
        activeSeason = idx;
        for (size_t k = 0; k < seasonBtns.size(); k++)
            seasonBtns[k]->setStyle((int)k == idx ? &brls::BUTTONSTYLE_PRIMARY
                                                  : &brls::BUTTONSTYLE_BORDERLESS);
        buildEpisodeCards(seasons[idx]);
        if (focusEpisodes && firstEpisode)
            brls::Application::giveFocus(firstEpisode);
    }

    void buildEpisodeCards(int season)
    {
        parkFocusOffEpisodes();
        episodesRow->setFocusable(false);
        episodesRow->clearViews();
        firstEpisode = nullptr;

        std::vector<stremio::Video> eps;
        for (const auto& v : *videos)
            if (v.season == season) eps.push_back(v);
        std::sort(eps.begin(), eps.end(),
                  [](const stremio::Video& a, const stremio::Video& b) {
                      return a.episode < b.episode;
                  });
        if (eps.empty()) { showEpisodesMessage("No episodes", false); return; }

        brls::View* firstThumb = nullptr;
        for (size_t i = 0; i < eps.size(); i++)
        {
            brls::View* thumb = nullptr;
            auto* col = makeEpisodeCard(eps[i], i > 0, &thumb);
            if (thumb && activeSeason >= 0 && activeSeason < (int)seasonBtns.size())
                thumb->setCustomNavigationRoute(brls::FocusDirection::UP,
                                                seasonBtns[activeSeason]);
            if (!firstThumb) firstThumb = thumb;
            episodesRow->addView(col);
        }
        firstEpisode = firstThumb;  // where A on the season lands
        if (!episodesRow->getChildren().empty())
            episodesRow->getChildren().back()->setMarginRight(60.0f);
        if (firstThumb && activeSeason >= 0 &&
            activeSeason < (int)seasonBtns.size())
            seasonBtns[activeSeason]->setCustomNavigationRoute(
                brls::FocusDirection::DOWN, firstThumb);
    }

    // A caption ("3 · Title") over a focusable still with a watch-progress bar.
    // The still is the focus target; *outThumb returns it for the nav routes.
    brls::Box* makeEpisodeCard(const stremio::Video& v, bool marginLeft,
                               brls::View** outThumb)
    {
        auto* col = new brls::Box();
        col->setAxis(brls::Axis::COLUMN);
        if (marginLeft) col->setMarginLeft(24.0f);

        auto* cap = new brls::Label();
        std::string t = v.title.empty() ? ("Episode " + std::to_string(v.episode))
                                        : v.title;
        cap->setText(std::to_string(v.episode) + " \xC2\xB7 " + t);  // middle dot
        cap->setFontSize(20.0f);
        cap->setSingleLine(true);
        cap->setWidth(380.0f);  // truncates (and, when focused, scrolls) to here
        cap->setMarginBottom(8.0f);
        col->addView(cap);

        auto* thumb = new EpisodeThumb();
        thumb->caption  = cap;  // scroll it while this still is focused
        thumb->progress = progressFor(v);  // drawn by EpisodeThumb::draw
        thumb->setFocusable(true);
        thumb->setWidth(380.0f);
        thumb->setHeight(214.0f);  // 16:9
        thumb->setCornerRadius(10.0f);
        thumb->setBackgroundColor(nvgRGB(0x1a, 0x1a, 0x20));
        stremio::Video ev = v;
        thumb->registerClickAction([this, ev](brls::View*) {
            openEpisode(ev);
            return true;
        });
        thumb->addGestureRecognizer(new brls::TapGestureRecognizer(thumb));

        auto* img = new brls::Image();
        img->setDimensions(380.0f, 214.0f);
        img->setScalingType(brls::ImageScalingType::FIT);
        img->setCornerRadius(10.0f);  // round the still itself, not just the box
        img->setPositionType(brls::PositionType::ABSOLUTE);
        img->setPositionTop(0.0f);
        img->setPositionLeft(0.0f);
        thumb->addView(img);
        if (!v.thumbnail.empty())
        {
            auto live = alive;
            stremio::fetchPosterAsync(
                v.id, v.thumbnail, [img, live](std::string p) {
                    if (*live && !p.empty()) img->setImageFromFile(p);
                });
        }

        col->addView(thumb);

        if (outThumb) *outThumb = thumb;
        return col;
    }

    // 0..1 through this episode, or -1 -- only the show's last-watched episode
    // carries a bar (Stremio keeps one position per library item). The local
    // just-played record wins over the library snapshot.
    double progressFor(const stremio::Video& v)
    {
        stremio::LocalWatch lw = stremio::lastWatch();
        bool haveLocal = lw.itemId == item.id && lw.progress() >= 0.0;
        std::string vid = haveLocal ? lw.videoId : item.videoId;
        double prog     = haveLocal ? lw.progress() : item.progress();
        return (!vid.empty() && v.id == vid) ? prog : -1.0;
    }

    void openEpisode(const stremio::Video& v)
    {
        PlayerArt epArt = art;
        if (!v.thumbnail.empty())
        {
            epArt.bgId   = v.id;
            epArt.bgUrl  = v.thumbnail;
            epArt.blurBg = false;  // a still is already busy and low-contrast
        }
        WatchInfo w;
        w.authKey      = authKey;
        w.itemId       = item.id;
        w.videoId      = v.id;
        w.displayTitle = (v.title.empty() ? item.name : v.title) + "  (" +
                         std::to_string(v.season) + " \xC2\xB7 " +
                         std::to_string(v.episode) + ")";
        if (v.id == item.videoId)
            w.resumeSec = resumeFrom(item.timeOffsetMs, item.durationMs);
        brls::Application::pushActivity(
            new EpisodeDetailActivity(authKey, v, seriesRating, epArt, w));
    }

    void parkFocusOffEpisodes()
    {
        brls::View* cur = brls::Application::getCurrentFocus();
        if (!cur || !isUnder(cur, episodesRow)) return;
        if (activeSeason >= 0 && activeSeason < (int)seasonBtns.size())
            brls::Application::giveFocus(seasonBtns[activeSeason]);
        else
        {
            episodesRow->setFocusable(true);
            episodesRow->setHideHighlight(true);
            brls::Application::giveFocus(episodesRow);
        }
    }

    void showEpisodesMessage(const std::string& msg, bool focusable)
    {
        parkFocusOffEpisodes();
        episodesRow->setFocusable(false);
        episodesRow->clearViews();
        auto* box = new brls::Box();
        box->setFocusable(focusable);
        if (focusable) box->setHideHighlight(true);
        box->setHeight(214.0f);
        box->setJustifyContent(brls::JustifyContent::CENTER);
        box->setAlignItems(brls::AlignItems::CENTER);
        auto* l = new brls::Label();
        l->setText(msg);
        l->setFontSize(20.0f);
        l->setTextColor(nvgRGB(150, 150, 155));
        box->addView(l);
        episodesRow->addView(box);
        if (focusable && !brls::Application::getCurrentFocus())
            brls::Application::giveFocus(box);
    }

    std::string authKey;
    stremio::LibItem item;
    PlayerArt art;

    brls::Image* poster     = nullptr;
    brls::Label* metaLine   = nullptr;
    brls::Label* descLabel  = nullptr;
    brls::Box* seasonBar    = nullptr;
    brls::Box* episodesRow  = nullptr;

    std::shared_ptr<std::vector<stremio::Video>> videos;
    std::vector<int> seasons;
    std::vector<brls::Button*> seasonBtns;
    int activeSeason           = -1;
    brls::View* firstEpisode   = nullptr;  // first card of the active season
    std::string seriesRating;

    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

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

    // A film has no season/episode tree -- open its detail screen (poster,
    // synopsis, and the addons as source cards).
    if (item.type != "series")
    {
        WatchInfo watch;
        watch.authKey      = authKey;
        watch.itemId       = item.id;
        watch.videoId      = item.videoId.empty() ? item.id : item.videoId;
        watch.displayTitle = item.name;  // shown top-left on pause
        watch.resumeSec    = resumeFrom(item.timeOffsetMs, item.durationMs);
        brls::Application::pushActivity(
            new MovieDetailActivity(authKey, item, art, watch));
        return;
    }

    // A series opens its detail screen: poster + synopsis, a season switcher and
    // that season's episodes as still cards (SeriesDetailActivity fetches the
    // meta/episodes itself).
    brls::Application::pushActivity(new SeriesDetailActivity(authKey, item, art));
}
