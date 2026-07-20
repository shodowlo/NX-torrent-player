#pragma once

#include <borealis.hpp>

#include <cstdint>
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
    bool hasMeta   = false;  // serves /meta/...
    bool hasStream = false;  // serves /stream/...
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

struct AddonsResult
{
    bool ok = false;
    std::vector<Addon> addons;
    std::string error;
};
struct MetaResult
{
    bool ok = false;
    std::vector<Video> videos;
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

// A blurred, screen-sized-friendly copy of a cached poster, made once and
// cached next to it. "" if the poster cannot be read.
std::string blurredPosterPath(const std::string& posterPath);

// Drops the cached addon collection, so the next fetchAddonsAsync goes to the
// network again. Sign out / sign in as another account.
void clearAddonCache();

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

  private:
    void promptEmail();
    void promptPassword();
    void doLogin();
    void onAuthenticated(const std::string& key, bool announce);
    void loadLibrary();
    void showLibrary(const std::vector<stremio::LibItem>& items);

    std::string email;
    std::string password;
    std::string authKey;

    brls::Box* loginBox      = nullptr;  // the sign-in form
    brls::Box* libraryBox    = nullptr;  // the list, once signed in
    brls::Label* emailLabel  = nullptr;
    brls::Label* statusLabel = nullptr;
    brls::Button* loginBtn   = nullptr;
    brls::Label* libStatus   = nullptr;
    brls::Box* libList       = nullptr;

    // Invalidated whenever the list is rebuilt. Poster fetches hold a raw
    // pointer to their Image; clearViews() deletes those, so a fetch that lands
    // afterwards must know not to touch it.
    std::shared_ptr<bool> rowsAlive = std::make_shared<bool>(true);

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
