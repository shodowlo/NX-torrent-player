#pragma once

#include <borealis.hpp>

#include <chrono>
#include <memory>
#include <string>

struct mpv_handle;
struct mpv_render_context;
struct torrentfs;

// A borealis view that streams a torrent (path to a .torrent file, or a magnet:
// URI) through our on-device engine and renders it full-screen with mpv on top
// of the borealis OpenGL context. Until mpv presents its first frame a borealis
// loading screen (buffering bar + torrent stats) is shown on top of the video.
// What the loading screen shows while the torrent opens. Empty = the app logo,
// no background (a local .torrent tells us nothing about what is inside it).
struct PlayerArt
{
    // On-disk image shown in the middle of the loading screen: the film's or the
    // show's poster. The cached thumbnail is enough at that size.
    std::string posterPath;

    // Full-screen background, downloaded at full size when playback starts (the
    // thumbnail is ~100px wide and looks like it when blown up). For a film this
    // is the poster again; for an episode, the episode's own still.
    std::string bgId;   // cache key
    std::string bgUrl;  // where to fetch it

    // Blur the background. A poster is one big image behind text, so it has to
    // be pushed back; an episode still is already a busy, low-contrast frame.
    bool blurBg = false;
};

// Ties a playback to its Stremio library entry, so the player can resume where
// the account left off and report the position back (datastorePut). All fields
// empty (the default) = not a Stremio playback, nothing is sent anywhere.
struct WatchInfo
{
    std::string authKey;
    std::string itemId;   // library _id ("tt1234567")
    std::string videoId;  // what is playing: itemId for a film, "tt123:1:3" ep
    double resumeSec = 0.0;  // start playback here (0 = from the beginning)
    std::string displayTitle;  // shown top-left on pause; "" falls back to the
                               // name derived from the source

    // Activities to pop when the video plays to its end (not on B). 1 closes
    // just the player (local). A Stremio play sits under the addon + source
    // lists, so 3 returns to the library (film) or the episode list (series).
    int endPop = 1;
};

class MpvView : public brls::Box
{
  public:
    // `art` is the artwork for the loading screen (see PlayerArt). `title`
    // overrides the name derived from the source, which for a magnet is just
    // "Torrent". `fileIndex` selects which file of a multi-file torrent to
    // stream; -1 (the default) streams the largest one.
    explicit MpvView(const std::string& source, const PlayerArt& art = {},
                     const std::string& title = "", int fileIndex = -1,
                     WatchInfo watch = {});
    ~MpvView() override;

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;

  private:
    void pumpEvents();
    void startEngine(const std::string& source, int fileIndex);
    bool startMpv();
    void registerPlayerActions();
    void buildLoadingOverlay(const std::string& title);
    void setBackgroundArt(const std::string& path);
    PlayerArt art;
    brls::Image* bgImage = nullptr;  // filled in once the full-size art lands

    // Opening a magnet announces to trackers and pulls the metadata off peers
    // (BEP 9) -- seconds of blocking work. It runs on a background thread, so
    // this guards against the result landing after the view is gone.
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::string pendingSource;
    void updateLoadingOverlay();
    void updateBufferIndicator();
    void updateInfoOverlay();
    void updateSeekBar();

    // X during playback: a modal popup to switch the current video's audio and
    // subtitle track. Pauses playback behind it (it stays paused on close, A
    // resumes), and reads the tracks straight off mpv.
    void openTrackMenu();

    // Dumps every engine counter to the log on a fixed interval, independently
    // of the ZR panel. Single snapshots hide the shape of the problem (a rate
    // read just before it collapses looks healthy); the trend is the evidence.
    void logStats();

    torrentfs* tfs = nullptr;
    mpv_handle* mpv = nullptr;
    mpv_render_context* renderCtx = nullptr;

    // Loading screen (borealis views drawn as children, hidden once ready).
    brls::Box* loadingOverlay = nullptr;
    brls::Label* statusLabel  = nullptr;
    brls::Label* statsLabel   = nullptr;
    brls::Label* percentLabel = nullptr;
    brls::Box* barFill        = nullptr;

    // Small "buffering" spinner shown mid-playback when the stream stalls.
    brls::Box* bufferOverlay = nullptr;
    bool buffering           = false;

    // Pause icon + seek bar (elapsed / total time) shown while paused with A.
    brls::Box* pauseOverlay  = nullptr;
    brls::Box* pauseTitleBox = nullptr;  // title, top-left, shown while paused
    std::string pauseTitle;              // what it says
    brls::Box* optionsHint   = nullptr;  // "X Options" hint, top-right while paused
    brls::Box* seekOverlay   = nullptr;
    brls::Box* seekFill      = nullptr;
    brls::Label* seekCur     = nullptr;
    brls::Label* seekTotal   = nullptr;
    bool userPaused          = false;

    // Scrubbing with the stick: left/right pause playback and move a target
    // along the seek bar; A commits the seek and resumes. The engine follows --
    // seeking moves the torrent playhead (source/stream.c seek_cb), so the
    // download window re-centres on wherever you land.
    bool seeking      = false;
    double seekTarget = 0.0;  // seconds, where the cursor currently points
    double seekDur    = 0.0;  // duration cached when scrubbing started
    brls::Box* seekCursor = nullptr;
    brls::Box* seekTrack  = nullptr;  // measured, to place fill+cursor in pixels

    // Analog scrub: driven per-frame from the stick axis, so it needs its own
    // clock and a hold timer to ramp the rate.
    void updateStickSeek();
    void beginSeek();
    double seekHeld = 0.0;  // seconds the stick has been held off-centre
    std::chrono::steady_clock::time_point seekLastFrame;
    bool seekFrameValid = false;

    // Semi-transparent network/torrent info panel, toggled with ZR.
    brls::Box* infoOverlay   = nullptr;
    brls::Label* infoLabel   = nullptr;  // left column
    brls::Label* infoLabel2  = nullptr;  // right column
    bool infoShown           = false;
    int64_t infoLastBytes    = 0;
    double infoSpeed         = 0.0;
    int64_t infoLastCache    = 0;    // cache bytes at the last sample (SD write rate)
    uint32_t infoLastLatN[5] = {};   // syscall counts at the last sample (rates)
    uint32_t statsLastLatN[5] = {};  // same, for the log's [stats] cadence

    // Observed mpv properties (async, updated in pumpEvents): the UI thread
    // must never call mpv_get_property in a per-frame path — a wedged mpv
    // core froze the render thread with it (and, through the engine getters'
    // lock, the whole download).
    double obsPos = 0.0, obsDur = 0.0, obsCacheSecs = 0.0;
    bool obsCacheIdle = false;
    std::chrono::steady_clock::time_point infoLastSample;

    // Periodic stats dump (see logStats).
    std::chrono::steady_clock::time_point statsLastSample;
    std::chrono::steady_clock::time_point statsStart;
    int64_t statsLastBytes = 0;
    bool statsStarted      = false;

    bool ready         = false;  // mpv has presented the first video frame
    bool fileLoaded    = false;  // mpv opened the stream (header downloaded)
    bool overlayHidden = false;  // loading screen has been taken down
    bool ended         = false;  // reached EOF; the auto-close is scheduled
    int shownPct       = -1;     // last buffering % pushed to the bar

    // Download-speed sampling.
    int64_t lastBytes = 0;
    double speedBps   = 0.0;
    std::chrono::steady_clock::time_point lastSample;

    // Stremio watch-state sync: the position is sampled alongside logStats and
    // pushed to the API every couple of minutes, plus once at teardown -- so
    // "continue watching" on other devices tracks what was watched here.
    void maybePushWatchState(bool force);
    WatchInfo watch;
    double lastPosSec = 0.0, lastDurSec = 0.0;
    std::chrono::steady_clock::time_point watchPushLast;
    bool watchPushValid = false;
};

// Full-screen activity hosting an MpvView. Pops (and tears mpv down) on B.
class PlayerActivity : public brls::Activity
{
  public:
    explicit PlayerActivity(std::string source, PlayerArt art = {},
                            std::string title = "", int fileIndex = -1,
                            WatchInfo watch = {});
    brls::View* createContentView() override;

    // Opaque: borealis stops the activity-stack draw here and does NOT composite
    // the browser behind, whose nanovg background/highlight would otherwise be
    // flushed over (and hide) the immediate mpv video render.
    bool isTranslucent() override { return false; }

  private:
    std::string source;
    PlayerArt art;
    std::string title;
    int fileIndex;
    WatchInfo watch;
};
