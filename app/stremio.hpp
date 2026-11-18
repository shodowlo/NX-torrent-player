#pragma once

#include <borealis.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Stremio account integration.
//
// Talks to the official API at https://api.strem.io. Login exchanges an
// email/password for an authKey; that key is persisted and authenticates every
// later call (currently: fetching the user's library).
namespace stremio
{

struct LoginResult
{
    bool ok = false;
    std::string authKey;  // set when ok
    std::string error;    // human-readable, set when !ok
};

struct LibItem
{
    std::string id;
    std::string name;
    std::string type;    // "movie" / "series"
    std::string poster;  // artwork URL, may be empty
    // Release year as the account or the catalog spells it: "2016", or a range
    // ("2013-2019") for a show. Display only, never parsed. Empty when whoever
    // sent the item did not say.
    std::string year;

    // Watch state, from the item's "state" object. For a film videoId is the
    // item id; for a series it is the LAST WATCHED episode ("tt123:1:3") --
    // Stremio only keeps one position per library item, not one per episode.
    std::string videoId;      // state.video_id, may be empty (never watched)
    double timeOffsetMs = 0;  // position in videoId
    double durationMs   = 0;  // duration of videoId

    // Last time the item changed on the account (ISO 8601, so it sorts
    // lexicographically = chronologically). Watching pushes a new state, which
    // bumps it, so sorting by it descending puts the most recently viewed first.
    std::string mtime;

    // removed = not in the explicit "+ Library" grid; temp = auto-added by
    // watching. Neither excludes it from Continue Watching, which is driven by
    // watch state -- Stremio shows a removed-but-watched title there too. The
    // Library view filters these out.
    bool removed = false;
    bool temp    = false;

    // 0..1 through the last watched video, or -1 when there is nothing to show.
    double progress() const
    {
        if (timeOffsetMs <= 0 || durationMs <= 0) return -1.0;
        double p = timeOffsetMs / durationMs;
        return p > 1.0 ? 1.0 : p;
    }
};

// Reports a playback position to the account ("continue watching"). Reads the
// library item back first and rewrites only its state fields, so nothing else
// on the item is lost. Fire-and-forget: runs on a background thread, failures
// are logged and dropped.
void pushWatchStateAsync(const std::string& authKey, const std::string& itemId,
                         const std::string& videoId, double posSec,
                         double durSec);

// Fetches `url` and caches it under the app's poster folder, keyed by `id`.
// Calls back on the UI thread with the on-disk path, or "" on failure. A cached
// file is returned immediately without touching the network.
void fetchPosterAsync(const std::string& id, const std::string& url,
                      std::function<void(std::string)> done);

// Full-size artwork for `id`, cached separately from the list thumbnail (which
// is fetched at ~100px wide and looks it when blown up to the screen). Calls
// back on the UI thread with the on-disk path, or "" on failure.
void fetchHqArtAsync(const std::string& id, const std::string& url,
                     std::function<void(std::string)> done);

// The on-disk poster for `id` if it has already been cached (by a prior
// fetchPosterAsync), "" otherwise. Never touches the network -- for callers
// that need a path right now and can do without artwork if there is none.
std::string cachedPosterPath(const std::string& id);

struct LibraryResult
{
    bool ok = false;
    std::vector<LibItem> items;
    std::string error;
};

// An installed addon. `base` is transportUrl minus the trailing
// "/manifest.json" -- every resource call hangs off it.
struct Addon
{
    std::string name;
    std::string base;
    bool hasMeta      = false;  // serves /meta/...
    bool hasStream    = false;  // serves /stream/...
    bool hasSubtitles = false;  // serves /subtitles/...
    std::vector<std::string> types;  // "movie", "series", ...

    bool supportsType(const std::string& t) const
    {
        for (const auto& x : types)
            if (x == t) return true;
        return types.empty();  // unscoped manifests apply to everything
    }
};

// One entry of a series' meta: an episode.
struct Video
{
    std::string id;  // "tt1234567:1:3"
    int season  = 0;
    int episode = 0;
    std::string title;
    std::string thumbnail;  // episode still, 16:9, may be empty
    std::string released;   // air date (ISO) or year, for the episode meta line
    std::string overview;   // episode synopsis, may be empty
};

// A playable source for a video.
struct Stream
{
    std::string name;      // addon-provided label (quality, group, ...)
    std::string title;     // longer description
    std::string infoHash;  // BitTorrent: what we can actually play
    std::string url;       // direct http(s) stream (unsupported for now)
    int fileIdx = -1;      // which file in the torrent to play (season packs);
                           // -1 = not given, fall back to the largest file
};

// One subtitle file an addon offers for a video.
struct Subtitle
{
    std::string url;    // the .srt/.vtt itself
    std::string lang;   // as the addon spells it: "eng", "fr", sometimes "French"
    std::string id;     // addon-side id; names the cache file when present
    std::string addon;  // which addon served it, so the picker can say
};

struct AddonsResult
{
    bool ok = false;
    std::vector<Addon> addons;
    std::string error;
};
struct SubtitlesResult
{
    // ok means at least one addon answered. No subtitle addon installed is
    // ok with an empty list -- nothing failed, there was just nothing to ask.
    bool ok = false;
    std::vector<Subtitle> subs;
    std::string error;
};
struct MetaResult
{
    bool ok = false;
    std::vector<Video> videos;

    // Descriptive fields from the meta object. A film's detail screen uses them;
    // the series episode list ignores them (they stay empty if the addon omits
    // them). runtime is left as the addon wrote it ("127 min"); the UI formats it.
    std::string description;
    std::string releaseInfo;  // release year ("2026"), or a span for a series
    std::string runtime;      // "127 min"
    std::string imdbRating;   // "7.5"

    std::string error;
};
struct StreamsResult
{
    bool ok = false;
    std::vector<Stream> streams;
    std::string error;
};

// Both run on a background thread and deliver on the UI thread. They never
// block the caller: these take seconds over a Switch's wifi and the UI has to
// keep drawing.
void loginAsync(const std::string& email, const std::string& password,
                std::function<void(LoginResult)> done);

// QR sign-in, through Stremio's link service (link.stremio.com, what its TV apps
// use): createLinkAsync gets a short code plus a QR of the page that approves
// it; the user scans it with a phone that is signed in, and readLinkAsync --
// polled -- returns the account's authKey once they have.
struct LinkCode
{
    bool ok = false;
    std::string code;   // "MW3U": typed at link.stremio.com without the QR
    std::string link;   // https://link.stremio.com/MW3U, what the QR encodes
    std::string qrPng;  // the QR as PNG bytes, from Stremio's own renderer
    std::string error;  // set when !ok
};
void createLinkAsync(std::function<void(LinkCode)> done);
// ok + authKey once the code has been approved. Not ok while it is still
// waiting -- the service answers "invalid or expired" for both, so the caller
// tells them apart by the code's age, not by this.
void readLinkAsync(const std::string& code, std::function<void(LoginResult)> done);
// The account's email, for the "signed in as" line in Options: a QR sign-in
// never sees it typed. "" on failure.
void fetchEmailAsync(const std::string& authKey,
                     std::function<void(std::string)> done);
void fetchLibraryAsync(const std::string& authKey,
                       std::function<void(LibraryResult)> done);
void fetchAddonsAsync(const std::string& authKey,
                      std::function<void(AddonsResult)> done);
// Episode list for a series, from an addon that serves meta (Cinemeta usually).
void fetchMetaAsync(const std::string& addonBase, const std::string& type,
                    const std::string& id, std::function<void(MetaResult)> done);
// Sources for one video id ("tt123" for a film, "tt123:1:3" for an episode).
void fetchStreamsAsync(const std::string& addonBase, const std::string& type,
                       const std::string& id,
                       std::function<void(StreamsResult)> done);

// Every installed subtitle addon asked for one video id, merged into a single
// list (duplicate URLs dropped, the preferred subtitle language first). One
// addon failing does not sink the rest. `type` is "movie" or "series".
void fetchSubtitlesAsync(const std::string& authKey, const std::string& type,
                         const std::string& videoId,
                         std::function<void(SubtitlesResult)> done);

// Downloads one subtitle into the app's subtitle cache and calls back on the UI
// thread with the on-disk path, or "" on failure. A file already there is
// returned without touching the network, so re-picking a subtitle is free.
void downloadSubtitleAsync(const Subtitle& sub,
                           std::function<void(std::string)> done);

// Full-text search against Cinemeta's catalog, movies then series, merged into
// one LibItem list (id/name/type/poster). One background call, one UI callback.
void fetchSearchAsync(const std::string& query,
                      std::function<void(LibraryResult)> done);

// The "extra" props a catalog request can carry. The default is the plain
// catalog: every genre, from the top.
struct CatalogQuery
{
    std::string genre;  // "" = no genre filter
    int skip = 0;       // items to skip -- this is how a Stremio catalog pages
};

// A meta catalog ("popular", "top", ...) from an addon (Cinemeta by default):
// {base}/catalog/{type}/{catalogId}.json. The metas come back as LibItems
// (id/name/type/poster, no watch state), so the same row builder shows them.
// Used for the Popular movies / series views.
void fetchCatalogAsync(const std::string& addonBase, const std::string& type,
                       const std::string& catalogId,
                       std::function<void(LibraryResult)> done);
// The same, filtered and/or paged. An addon that does not support an extra
// simply ignores it, so this is never worse than the call above.
void fetchCatalogAsync(const std::string& addonBase, const std::string& type,
                       const std::string& catalogId, const CatalogQuery& query,
                       std::function<void(LibraryResult)> done);

// The genres an addon offers for one of its catalogs, read off its manifest
// (catalogs[].extra where name is "genre"). Empty when the catalog does not
// filter by genre -- and also when the manifest cannot be read, which amounts
// to the same thing for the UI. Cached per (addon, type, catalog).
void fetchCatalogGenresAsync(const std::string& addonBase,
                             const std::string& type,
                             const std::string& catalogId,
                             std::function<void(std::vector<std::string>)> done);

// Whether the app deliberately ignores this addon's STREAMS. Some of them
// cannot serve a torrent it can play at all; the Public Domain ones can, but
// they are noise in every source list. Matched on a substring of the name.
// Says nothing about the addon's metadata or subtitles, which are still used.
bool isStreamAddonHidden(const std::string& name);

// How many titles the account holds, or -1 when the library has not been
// fetched this session and the honest answer is "not known yet".
int libraryCount();

// Whether `itemId` is in the account's library right now. Reads the set the
// last fetchLibraryAsync filled, plus whatever has been toggled since -- so a
// screen opened from a catalog or a search, whose LibItem carries no membership
// flags at all, still knows which way round its button goes.
bool inLibrary(const std::string& itemId);

// Adds `item` to the library, or removes it. Stremio deletes nothing: a removed
// item is simply one with "removed": true, so both directions are the same
// write. The local answer to inLibrary() flips immediately; `done` reports on
// the UI thread whether the API actually accepted it.
void setLibraryMemberAsync(const std::string& authKey, const LibItem& item,
                           bool add, std::function<void(bool)> done);

// Clears an item's watch state on the account, which is what takes it out of
// Continue Watching -- that row is derived from the state, not from a list of
// its own. A "temp" item (one the account only holds because it was watched,
// never added on purpose) is dropped from the library at the same time, so it
// does not sit there emptied out. Calls back on the UI thread.
void clearWatchStateAsync(const std::string& authKey, const std::string& itemId,
                          std::function<void(bool)> done);

// authKey persistence, so a sign-in survives a restart.
bool saveAuthKey(const std::string& key);
std::string loadAuthKey();

// Signs out: drops the stored authKey (and email). The next start shows the
// sign-in form.
void clearAuthKey();

// The signed-in address, remembered only so Options can show it. "" if unknown
// -- a session signed in before this was stored still works, it just cannot say
// who it belongs to.
bool saveEmail(const std::string& email);
std::string loadEmail();

// Where UP from the top of the library should land (the header tab bar, in the
// top-bar layout). nullptr -- the default -- leaves normal focus traversal
// alone, which is what the sidebar layout wants.
//
// It takes a route because traversal cannot be relied on to get out of the
// list: ScrollingFrame::getNextFocus returns itself while the list can still
// scroll up, and giveFocus() then re-focuses the same row -- a dead end. A
// custom route is checked on the focused view before any of that.
void setLibraryUpTarget(brls::View* target);

// Where the library's item count is shown. The tab's header (AppletFrame title)
// carries it so the count does not eat a row above the list; main.cpp wires this
// to the frame. Called with the count text on load, or "" to clear it back to a
// plain title. No-op if never set.
void setLibraryCountSink(std::function<void(const std::string&)> sink);

// The Stremio tab registers a callback here (constructor) so R/L can cycle its
// view even when focus is on the header tab bar, which sits outside the tab's
// view tree. main.cpp registers R/L on the frame and calls cycleActiveView;
// dir is +1 (R, next) or -1 (L, previous). No-op when no Stremio tab is live.
void setViewCycler(std::function<void(int)> cycler);
void cycleActiveView(int dir);

// The header's Stremio view tab bar (top-right). main.cpp builds one button per
// viewLabels() entry (index-matched to the view cycle order), registers
// setViewTabSink to highlight the active view -- or hide the whole bar with
// index -1, used on the Local tab and the sign-in screen -- and calls
// selectActiveView from a button to jump straight to a view. The live tab drives
// the highlight and registers the selector.
const std::vector<std::string>& viewLabels();
void setViewTabSink(std::function<void(int)> sink);
void setViewSelector(std::function<void(int)> selector);
void selectActiveView(int index);

// A blurred, screen-sized-friendly copy of a cached poster, made once and
// cached next to it. "" if the poster cannot be read.
std::string blurredPosterPath(const std::string& posterPath);

// Drops the cached addon collection, so the next fetchAddonsAsync goes to the
// network again. Sign out / sign in as another account. Also drops the meta
// cache below.
void clearAddonCache();

// Forgets every cached meta answer. fetchMetaAsync keeps them for the session
// so that walking a season costs no network -- see the note at its definition.
void clearMetaCache();

// Bumped whenever playback pushes newer progress to the account. A view stores
// the value it last rendered and reloads when this differs, so stacked lists
// each refresh once (see StremioTab, ListActivity).
uint32_t libraryGen();

// Forces the library to reload when the Stremio tab is next focused (same path
// as a watch push). Used after clearing the poster cache so the artwork is
// re-fetched.
void markLibraryStale();

// Total bytes of cached posters on disk, and a way to delete them all. For the
// "poster cache" line + Clear button in Options.
int64_t posterCacheBytes();
void clearPosterCache();

// The position we last reported. Stremio keeps one position per show and we just
// set it, so this is the current truth for `itemId` -- the episode/season lists
// use it to show fresh progress without re-fetching.
struct LocalWatch
{
    std::string itemId;
    std::string videoId;
    double offsetMs = 0;
    double durationMs = 0;
    double progress() const
    {
        if (offsetMs <= 0 || durationMs <= 0) return -1.0;
        double p = offsetMs / durationMs;
        return p > 1.0 ? 1.0 : p;
    }
};
LocalWatch lastWatch();

} // namespace stremio

// The "Stremio" tab: signs in, then lists the account's library.
class StremioTab : public brls::Box
{
  public:
    StremioTab();
    ~StremioTab() override;

    // Animates the indeterminate loading bar while it is shown.
    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

  private:
    void promptEmail();
    void promptPassword();
    // One row of the sign-in card: a caption over its value, the whole thing
    // focusable and opening the keyboard. Returns the value label to fill in.
    brls::Box* loginField(const char* caption, const char* placeholder,
                          std::function<void()> onPress, brls::Label** value);
    void doLogin();
    // The QR sign-in dialog: shows a link code as a QR and polls until a phone
    // approves it (see stremio::createLinkAsync).
    void promptQrLogin();
    void onAuthenticated(const std::string& key, bool announce);
    void loadLibrary();

    // The Stremio tab cycles through these with R. Continue Watching is the
    // default landing view; Popular pulls Cinemeta's top catalog.
    enum class View
    {
        ContinueWatching = 0,
        PopularMovies,
        PopularSeries,
        Library,
        Search,
        COUNT
    };
    View view = View::ContinueWatching;
    void cycleView(int dir);       // R/L: advance/back a view and render it
    void selectView(View v);       // jump straight to a view (header tab bar)
    void renderView();         // (re)build the list for the current view
    void showItems(const std::vector<stremio::LibItem>& items,
                   const std::string& header, const char* emptyMsg);
    void loadCatalog(const char* type, std::vector<stremio::LibItem>& cache,
                     bool& loaded, const std::string& header);
    brls::Box* addItemRow(const stremio::LibItem& it);  // one poster row into libList
    // Builds a poster row without parenting it (Search's split columns place it
    // themselves). showType appends the "Movie"/"Show" tag; a one-type column hides it.
    // Dispatches on config::listStyle to one of the two builders below.
    brls::Box* buildItemRow(const stremio::LibItem& it, bool showType);
    brls::Box* buildCardRow(const stremio::LibItem& it, bool showType);
    brls::Box* buildClassicRow(const stremio::LibItem& it, bool showType);
    // The poster style: one horizontally scrolling strip of upright cards,
    // instead of a column of rows. Built as a whole rather than item by item --
    // the strip is a single child of the list.
    // upToHeader: this is the first strip on the page, so its cards route UP to
    // the header. A strip below one walks up the page instead.
    brls::View* buildPosterStrip(const std::vector<stremio::LibItem>& items,
                                 bool upToHeader, brls::Box* seeMore = nullptr);
    brls::Box*  buildPosterCard(const stremio::LibItem& it, bool upToHeader);
    // A titled strip into libList ("Popular", "Movies", ...); returns the strip,
    // for finishList's bottom inset.
    brls::View* addStripSection(const std::string& title,
                                const std::vector<stremio::LibItem>& items,
                                brls::Box* seeMore);
    // Catalog views show a screenful and put the rest behind a See More tile,
    // shaped like the items it follows, which opens the section full-screen.
    bool capped() const;
    // catType/catId identify the catalog the items came from, so the section
    // page can keep querying it -- for another page as you scroll, or for a
    // different genre. See openSection.
    brls::Box* buildSeeMoreCard(const std::string& title,
                                std::vector<stremio::LibItem> all,
                                std::string catType, std::string catId);
    brls::Box* buildSeeMoreRow(const std::string& title,
                               std::vector<stremio::LibItem> all,
                               std::string catType, std::string catId);
    void openSection(std::string title, std::vector<stremio::LibItem> items,
                     std::string catType, std::string catId);
    // "movie" or "series" for the view we are on -- the type half of every
    // catalog request the Popular views make.
    const char* catalogType() const;
    void addHeading(const std::string& title, float top, float bottom, float left);
    float headingInset() const;  // where a heading starts, per style
    // What the heading over the current view's strip says.
    const char* sectionTitle() const;
    // Cinemeta's second catalog for the view we are on, or nullptr where there
    // is no second section (everything but Popular Movies / Shows).
    const std::vector<stremio::LibItem>* featuredCache();
    void loadFeatured(const char* type);  // one attempt per type per session
    // X on a Continue Watching tile takes it out of that row (by clearing its
    // watch state, which is what the row is derived from). No-op in every other
    // view -- the footer hint comes from the registered action, so registering
    // it where it does nothing would advertise a key that does nothing.
    void bindRemoveFromContinue(brls::Box* card, const stremio::LibItem& it);
    // Shared by both styles: the focusable row shell (size, click, tap) and the
    // poster that fills in once the artwork lands.
    brls::Box* newRowShell(const stremio::LibItem& it, float height);
    brls::Image* newPoster(const stremio::LibItem& it, float w, float h,
                           float marginRight);
    void attachPoster(brls::Image* art, const stremio::LibItem& it);
    void finishList(brls::View* lastRow);  // focus/reset/slide after (re)building
    void renderSearch();       // the Search view: a search bar + results
    void promptSearch();       // opens the keyboard, runs the query

    std::string searchQuery;
    std::vector<stremio::LibItem> searchResults;
    bool searchLoaded = false;

    // The last library fetch, kept so Continue Watching and Library render
    // without re-hitting the network on every R press.
    std::vector<stremio::LibItem> libItems;
    bool libLoaded = false;
    // Popular catalogs, fetched lazily on first view and cached after.
    std::vector<stremio::LibItem> popMovies, popSeries;
    bool popMoviesLoaded = false, popSeriesLoaded = false;
    // The "Featured" strip under them (Cinemeta's other catalog). Tried once per
    // type per session -- the flag means "asked", not "got something", so a
    // failing addon costs one request rather than one per visit.
    std::vector<stremio::LibItem> featMovies, featSeries;
    bool featMoviesAsked = false, featSeriesAsked = false;

    std::string email;
    std::string password;
    std::string authKey;

    brls::Box* loginBox      = nullptr;  // the sign-in form
    brls::Box* libraryBox    = nullptr;  // the list, once signed in
    brls::Label* emailLabel  = nullptr;
    brls::Label* passLabel   = nullptr;  // bullets, never the password
    brls::Label* statusLabel = nullptr;
    brls::Button* loginBtn   = nullptr;
    brls::Label* libStatus   = nullptr;
    brls::Box* libList       = nullptr;
    brls::ScrollingFrame* libScroll = nullptr;  // to reset the scroll on a view change

    // Centered status/loading overlay: the message label (libStatus) plus an
    // indeterminate bar shown only while loading (a sliding segment, no percent).
    brls::Box* loadingBox  = nullptr;
    brls::Box* loadingBar  = nullptr;   // the track; visible only while loading
    brls::Box* loadingFill = nullptr;   // the sliding segment
    void showStatus(const std::string& msg, bool loading);  // centered message +/- bar

    // Set by cycleView so the next showItems lands the cursor on the first row
    // and scrolls back to the top -- a view change should not keep the old
    // scroll position or focus deep in the previous list.
    bool resetOnShow = false;

    // Set by selectView (a header tab-bar pick) so finishList rebuilds the list
    // and resets the scroll but leaves focus on the header button, instead of
    // diving into the list the way an R/L cycle does. Lets you click through the
    // view buttons without being thrown back down each time.
    bool suppressFocusMove = false;

    // The current render laid its items out in two columns (Search's two types,
    // or Popular next to Featured). Left/Right then belong to the list first and
    // only cycle the view at the outer edge -- see the actions in the ctor.
    bool columnsShown = false;

    // Horizontal slide-in when the view changes: cycleView records the direction
    // (R = +1 enters from the right, L = -1 from the left); showItems starts the
    // animation once its rows are up; draw() eases the list back to x=0.
    int pendingSlide = 0;   // direction to play on the next showItems, 0 = none
    int slideSign    = 0;   // direction of the running animation
    double slideStart = 0;  // steady_clock seconds when it began
    bool sliding      = false;

    // Analog stick view cycling: one cycle per horizontal flick (latched until
    // the stick recenters), polled in draw() since the stick is an axis.
    bool stickLatched = false;

    // Invalidated whenever the list is rebuilt. Poster fetches hold a raw
    // pointer to their Image; clearViews() deletes those, so a fetch that lands
    // afterwards must know not to touch it.
    std::shared_ptr<bool> rowsAlive = std::make_shared<bool>(true);

    // Set, while a pushed page (openSection) builds cards, to that page's own
    // token -- so its poster fetches die with the page rather than with the
    // tab's list, which outlives it. Null the rest of the time, meaning "use
    // rowsAlive". See attachPoster.
    std::shared_ptr<bool> artToken;

    // Cleared by the destructor. Switching tabs deletes this view on the spot
    // (TabFrame::addTab removeView()s the old tab), so a login/library request
    // still in flight would come back to a dead `this`. Every callback that
    // captures `this` has to check this flag first.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    // Reloads the library when focus returns into the list after playback
    // reported newer progress. willAppear does not fire on returning to a
    // shown activity, so a focus hook is the reliable signal. Unsubscribed in
    // the destructor.
    brls::GenericEvent::Subscription focusSub {};
    bool focusSubbed = false;
    uint32_t seenGen = 0;  // library generation last rendered
    void onGlobalFocus(brls::View* focused);
    // Moves focus to libraryBox if it is currently on a row about to be deleted,
    // so clearViews() never frees the focused view (a use-after-free in the next
    // giveFocus()). Must run before every libList->clearViews().
    void parkFocusOffList();
};
