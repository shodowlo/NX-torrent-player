#include "player.hpp"

#include <cstdio>
#include <cstring>

#include <switch.h>  // appletSetMediaPlaybackState (keep the screen awake)

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <mpv/client.h>
#include <mpv/render.h>
#include <mpv/render_gl.h>

#include "appdata.hpp"
#include "config.hpp"
#include "stremio.hpp"  // cached artwork -> blurred background

extern "C" {
#include "torrentfs.h"
#include "stream.h"
#include "torrent.h"  // magnet metadata-fetch progress counters
}

namespace
{
void* getProcAddress(void*, const char* name)
{
    glfwGetCurrentContext();
    return (void*)glfwGetProcAddress(name);
}

// Seconds of video mpv must buffer before playback starts (and the loading
// screen reaches 100% / hands over to the video). Bump this for smoother
// playback on scarce swarms at the cost of a longer wait.
constexpr double kBufferSecs = 15.0;

// Seconds the cursor moves per D-pad press while scrubbing.
constexpr double kSeekStepSecs = 5.0;

// Analog scrubbing. Rate is (tilt^2) * base, so a light push creeps and a full
// push flies, then ramps by up to kSeekAccelMax the longer it's held -- an hour
// of video is unreachable at a fixed rate, but a fixed *fast* rate makes it
// impossible to land on a scene.
constexpr float kStickDeadzone   = 0.15f;
constexpr double kSeekBaseRate   = 30.0;  // seconds of video per second, full tilt
constexpr double kSeekAccelMax   = 12.0;  // held-down multiplier ceiling
constexpr double kSeekAccelSecs  = 4.0;   // seconds of holding to reach it

// Seconds -> "M:SS" (or "H:MM:SS" past an hour).
std::string fmtTime(double s)
{
    if (s < 0 || s != s)  // clamp negatives / NaN
        s = 0;
    int t = (int)s, h = t / 3600, m = (t % 3600) / 60, sec = t % 60;
    char buf[16];
    if (h > 0)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", m, sec);
    return buf;
}

// Pops `n` activities back-to-back. A free function, not a member: the first pop
// destroys the player view, so `this` cannot drive the rest. Each pop chains the
// next from its completion callback, so they run in order without animation.
void popActivities(int n)
{
    if (n <= 0) return;
    brls::Application::popActivity(brls::TransitionAnimation::NONE,
                                   [n]() { popActivities(n - 1); });
}

} // namespace

MpvView::MpvView(const std::string& source, const PlayerArt& art,
                 const std::string& titleOverride, int fileIndex,
                 WatchInfo watchInfo)
    : art(art)
    , watch(std::move(watchInfo))
{
    // Tell the OS a video is playing, so it does not dim the screen or sleep
    // during a long film -- there is no controller input while watching, which
    // is exactly what the idle timer counts. Cleared in the destructor.
    appletSetMediaPlaybackState(true);

    this->setGrow(1.0f);
    this->setBackgroundColor(nvgRGB(0, 0, 0));
    // Take focus off the browser (so its selection highlight stops drawing over
    // the video) but show no highlight box on the video itself.
    this->setFocusable(true);
    this->setHideHighlight(true);

    // Build the UI BEFORE touching the engine. torrentfs_open used to run right
    // here and return early on failure, leaving a view with no children at all
    // -- a plain black screen with nothing to say what went wrong.
    {
        // A caller that knows the real name (Stremio) passes it in: a magnet
        // carries none until its metadata lands, so this is the only way to show
        // it during the wait.
        std::string title = titleOverride;
        if (title.empty()) title = source;
        if (!titleOverride.empty())
        {
            // already the display name
        }
        else if (title.rfind("magnet:", 0) == 0)
        {
            title = "Torrent";
        }
        else
        {
            size_t slash = title.find_last_of("/\\");
            if (slash != std::string::npos) title = title.substr(slash + 1);
            if (title.size() > 8 &&
                title.compare(title.size() - 8, 8, ".torrent") == 0)
                title = title.substr(0, title.size() - 8);
        }
        lastSample = std::chrono::steady_clock::now();
        // What the pause overlay shows: the caller's episode/film title when it
        // gave one, otherwise the name derived above.
        pauseTitle = watch.displayTitle.empty() ? title : watch.displayTitle;
        buildLoadingOverlay(title);
    }
    registerPlayerActions();

    startEngine(source, fileIndex);
}

// Opens the torrent off the UI thread. For a magnet this announces to trackers
// and pulls the metadata from peers (BEP 9) -- seconds of blocking work. Doing
// it inline froze the whole app on a black screen until it finished or failed.
void MpvView::startEngine(const std::string& source, int fileIndex)
{
    if (statusLabel)
        statusLabel->setText(source.rfind("magnet:", 0) == 0
                                 ? "Fetching metadata..."
                                 : "Opening torrent...");

    auto liveFlag = this->alive;
    brls::async([this, liveFlag, source, fileIndex]() {
        char err[256] = { 0 };
        torrentfs* t = torrentfs_open_file(source.c_str(), APPDATA_CACHE,
                                           fileIndex, err, sizeof(err));
        std::string e = err;

        brls::sync([this, liveFlag, t, e]() {
            // B may have been pressed while we waited: `this` is gone and the
            // engine we just opened would leak.
            if (!*liveFlag)
            {
                if (t) torrentfs_close(t);
                return;
            }
            if (!t)
            {
                brls::Logger::error("torrentfs_open failed: {}", e);
                if (statusLabel) statusLabel->setText("Failed: " + e);
                return;
            }
            tfs = t;
            if (!startMpv() && statusLabel)
                statusLabel->setText("Player initialisation failed");
        });
    });
}

// Brings mpv up against the now-open engine. False if anything failed.
bool MpvView::startMpv()
{
    mpv = mpv_create();
    if (!mpv)
    {
        brls::Logger::error("mpv_create failed");
        return false;
    }

    mpv_set_option_string(mpv, "vo", "libmpv");
    mpv_set_option_string(mpv, "hwdec", "auto");
    mpv_set_option_string(mpv, "config", "no");
    mpv_set_option_string(mpv, "terminal", "no");
    // Switch audio output is stereo; force a proper downmix or 5.1 dialogue
    // (centre channel) is dropped.
    mpv_set_option_string(mpv, "audio-channels", "stereo");

    // Preferred track languages (Options). mpv falls back to the file's default
    // track when nothing matches, so a wrong guess costs nothing.
    std::string alang = config::mpvLangList(config::get().audioLang);
    if (!alang.empty()) mpv_set_option_string(mpv, "alang", alang.c_str());
    std::string slang = config::mpvLangList(config::get().subLang);
    if (!slang.empty()) mpv_set_option_string(mpv, "slang", slang.c_str());
    // "no" means: load no subtitle track at all, rather than pick one and hide
    // it -- which also saves decoding them.
    mpv_set_option_string(mpv, "sid", config::get().subtitles ? "auto" : "no");
    mpv_set_option_string(mpv, "cache", "yes");
    // Never let mpv auto-pause playback to rebuffer -- once we start, we keep
    // playing. We do the initial buffering ourselves: start paused, fill the
    // demuxer cache, then unpause on reveal (see updateLoadingOverlay/draw).
    mpv_set_option_string(mpv, "cache-pause", "no");
    mpv_set_option_string(mpv, "pause", "yes");
    // Give the demuxer enough headroom to buffer kBufferSecs before we unpause.
    mpv_set_option_string(mpv, "demuxer-max-bytes", "128MiB");
    mpv_set_option_string(mpv, "demuxer-readahead-secs", "30");
    // With cache=yes, mpv's cache-secs (default 10) *overrides*
    // demuxer-readahead-secs, so the demuxer stopped at exactly 10 s of
    // readahead. kBufferSecs is 15, so "buffered enough" could never become
    // true and the loading screen hung forever no matter how much was
    // downloaded. Must stay comfortably above kBufferSecs.
    mpv_set_option_string(mpv, "cache-secs", "60");
    // On Switch the hardware decoder's GPU work is async; without a glFinish
    // after mpv's render, glfwSwapBuffers can present before the video is drawn
    // (black frame). This is what the other Switch mpv players set too.
    mpv_set_option_string(mpv, "opengl-glfinish", "yes");
    mpv_set_option_string(mpv, "vd-lavc-dr", "yes");

    if (mpv_initialize(mpv) < 0)
    {
        brls::Logger::error("mpv_initialize failed");
        return false;
    }

    mpv_opengl_init_params glInit{ getProcAddress, nullptr };
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_OPENGL },
        { MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInit },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };
    if (mpv_render_context_create(&renderCtx, mpv, params) < 0)
    {
        brls::Logger::error("mpv_render_context_create failed");
        renderCtx = nullptr;
        return false;
    }
    brls::Logger::info("mpv render context created OK");

    // Capture mpv's internal log so we can see vo/render/hwdec state.
    mpv_request_log_messages(mpv, "v");

    // Resume where the Stremio account left off: mpv seeks there right after
    // the open, and the engine's playhead (stream.c seek_cb) follows, so the
    // download window re-centres on the resume point instead of the start.
    if (watch.resumeSec > 1.0)
    {
        char st[32];
        std::snprintf(st, sizeof(st), "%.1f", watch.resumeSec);
        mpv_set_option_string(mpv, "start", st);
        brls::Logger::info("[player] resuming at {}s (Stremio watch state)",
                           (int)watch.resumeSec);
    }

    // Buffered-seconds feed for the engine's calm mode, as an async observe:
    // a synchronous mpv_get_property from draw() waits on the mpv core, and
    // when the core is wedged behind a blocking stream read (playhead piece
    // not downloaded yet) that froze the RENDER thread with it -- the "small"
    // lag spikes. Property-change events arrive through pumpEvents instead.
    mpv_observe_property(mpv, 0, "demuxer-cache-duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "demuxer-cache-idle", MPV_FORMAT_FLAG);
    mpv_observe_property(mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(mpv, 0, "duration", MPV_FORMAT_DOUBLE);

    // Wire our torrent:// stream (source/stream.c) and start playback.
    stream_register(mpv, tfs);
    const char* cmd[] = { "loadfile", "torrent://stream", nullptr };
    mpv_command(mpv, cmd);
    brls::Logger::info("loadfile torrent://stream issued");
    return true;
}

// Controls. Registered from the constructor, not from startMpv: the engine may
// still be resolving a magnet (or may have failed outright), and B has to work
// the whole time -- that is the only way out of a stuck load.
void MpvView::registerPlayerActions()
{
    // B returns to the browser.
    this->registerAction(
        "Back", brls::BUTTON_B,
        [](brls::View*) {
            brls::Logger::info("[teardown] B pressed -> popActivity()");
            bool ok = brls::Application::popActivity();
            brls::Logger::info("[teardown] popActivity() returned {}", ok);
            return true;
        },
        false, false, brls::SOUND_BACK);

    // A toggles pause during playback (ignored while still buffering).
    this->registerAction(
        "Pause", brls::BUTTON_A,
        [this](brls::View*) {
            if (!ready || !mpv)
                return true;

            // Commit a scrub: jump there and resume. mpv's seek reaches our
            // stream's seek_cb, which moves the torrent playhead, so the
            // download window follows to the new position.
            if (seeking)
            {
                seeking = false;
                char t[32];
                std::snprintf(t, sizeof(t), "%.3f", seekTarget);
                const char* cmd[] = { "seek", t, "absolute", nullptr };
                mpv_command_async(mpv, 0, cmd);
                userPaused = false;
                mpv_set_property_string(mpv, "pause", "no");
                if (pauseOverlay) pauseOverlay->setVisibility(brls::Visibility::GONE);
                if (pauseTitleBox) pauseTitleBox->setVisibility(brls::Visibility::GONE);
                if (seekOverlay) seekOverlay->setVisibility(brls::Visibility::GONE);
                return true;
            }

            userPaused = !userPaused;
            mpv_set_property_string(mpv, "pause", userPaused ? "yes" : "no");
            brls::Visibility vis = userPaused ? brls::Visibility::VISIBLE
                                              : brls::Visibility::GONE;
            if (pauseOverlay)
                pauseOverlay->setVisibility(vis);
            if (pauseTitleBox)
                pauseTitleBox->setVisibility(vis);
            if (seekOverlay)
                seekOverlay->setVisibility(vis);
            return true;
        },
        false, false, brls::SOUND_CLICK);

    // Left/right scrub. The first nudge pauses and pins the cursor to the
    // current position; further nudges move it. allowRepeating so holding the
    // stick scrubs continuously. A commits (see the Pause action), B leaves.
    auto scrub = [this](double delta) {
        if (!ready || !mpv)
            return true;
        if (!seeking)
            beginSeek();
        seekTarget += delta;
        if (seekTarget < 0.0) seekTarget = 0.0;
        if (seekDur > 0.0 && seekTarget > seekDur) seekTarget = seekDur;
        return true;
    };

    this->registerAction(
        "Seek -", brls::BUTTON_LEFT,
        [scrub](brls::View*) { return scrub(-kSeekStepSecs); },
        false, true, brls::SOUND_NONE);
    this->registerAction(
        "Seek +", brls::BUTTON_RIGHT,
        [scrub](brls::View*) { return scrub(kSeekStepSecs); },
        false, true, brls::SOUND_NONE);

    // ZR toggles the network/torrent info panel.
    this->registerAction(
        "Info", brls::BUTTON_RT,
        [this](brls::View*) {
            infoShown = !infoShown;
            brls::Logger::info("[player] ZR info toggle -> {}", infoShown);
            if (infoOverlay)
                infoOverlay->setVisibility(infoShown
                                               ? brls::Visibility::VISIBLE
                                               : brls::Visibility::GONE);
            return true;
        },
        false, false, brls::SOUND_NONE);
}

MpvView::~MpvView()
{
    brls::Logger::info("[teardown] ~MpvView enter");

    // Playback is ending: let the OS dim/sleep on idle again.
    appletSetMediaPlaybackState(false);

    // Last chance to tell Stremio where playback stopped. Uses the position
    // sampled during playback (see logStats) -- mpv is about to be destroyed,
    // and the push itself runs on a background thread with no reference to us.
    maybePushWatchState(true);

    // Tell any in-flight startEngine() that we are gone, so it closes the engine
    // it opened instead of writing into a destroyed view.
    *alive = false;

    // Cancel first so the mpv demuxer's parked read returns (~20 ms); mpv's
    // shutdown touches GL, so it MUST stay on this (UI/GL) thread -- doing it on
    // a background thread crashed. This part is fast.
    if (tfs)
        torrentfs_cancel(tfs);
    if (renderCtx)
        mpv_render_context_free(renderCtx);
    if (mpv)
        mpv_terminate_destroy(mpv);

    // Close the engine synchronously (~1 s: it joins the acceptors/workers).
    //
    // This used to be handed to a detached std::thread so the menu came back
    // instantly, which is what crashed on B: std::thread's constructor throws
    // std::system_error when the thread can't be created, and a throw out of a
    // destructor (implicitly noexcept) is an immediate std::terminate/abort.
    // abort() then runs libnx's exit path -> userAppExit -> socketExit(), which
    // tears down the BSD socket layer while the engine's workers are still
    // live -- their next socket call dereferences a NULL devoptab and data
    // aborts. Joining here keeps the engine's lifetime strictly inside this
    // destructor, so nothing can outlive it.
    torrentfs* t = tfs;
    tfs          = nullptr;
    mpv          = nullptr;
    renderCtx    = nullptr;
    if (t)
        torrentfs_close(t);

    brls::Logger::info("[teardown] ~MpvView leave (engine closed)");
}

void MpvView::pumpEvents()
{
    if (!mpv)
        return;
    while (true)
    {
        mpv_event* ev = mpv_wait_event(mpv, 0);
        if (ev->event_id == MPV_EVENT_NONE)
            break;
        switch (ev->event_id)
        {
            case MPV_EVENT_FILE_LOADED:
                // Header downloaded and demuxed: we're now buffering, not
                // connecting.
                fileLoaded = true;
                brls::Logger::info("[mpv event] file loaded");
                break;
            case MPV_EVENT_END_FILE:
            {
                // Played to the end (not stop/error): close the player and
                // return. B is handled separately and pops only one level.
                auto* ef = (mpv_event_end_file*)ev->data;
                if (ef && ef->reason == MPV_END_FILE_REASON_EOF && !ended)
                {
                    ended   = true;
                    int pops = watch.endPop;
                    brls::Logger::info("[mpv event] EOF -> pop {}", pops);
                    // Deferred: we are inside draw()'s pumpEvents, and popping
                    // (which frees this view) mid-draw is unsafe.
                    brls::sync([pops]() { popActivities(pops); });
                }
                break;
            }
            case MPV_EVENT_PLAYBACK_RESTART:
                // First frame is decoded, but with cache-pause-initial mpv is
                // still buffering-paused here; readiness is driven off buffered
                // seconds in updateLoadingOverlay(), not this event.
                brls::Logger::info("[mpv event] playback restart (first frame)");
                break;
            case MPV_EVENT_LOG_MESSAGE:
            {
                auto* m = (mpv_event_log_message*)ev->data;
                brls::Logger::debug("[mpv:{}] {}: {}", m->level, m->prefix,
                                    m->text);
                break;
            }
            case MPV_EVENT_PROPERTY_CHANGE:
            {
                auto* p = (mpv_event_property*)ev->data;
                if (!p || !p->data)
                    break;
                if (p->format == MPV_FORMAT_DOUBLE)
                {
                    double v = *(double*)p->data;
                    if (std::strcmp(p->name, "demuxer-cache-duration") == 0)
                    {
                        obsCacheSecs = v;
                        if (tfs)
                            torrentfs_set_backlog(tfs, (int)(v * 1000.0));
                    }
                    else if (std::strcmp(p->name, "time-pos") == 0)
                        obsPos = v;
                    else if (std::strcmp(p->name, "duration") == 0)
                        obsDur = v;
                }
                else if (p->format == MPV_FORMAT_FLAG &&
                         std::strcmp(p->name, "demuxer-cache-idle") == 0)
                    obsCacheIdle = *(int*)p->data != 0;
                break;
            }
            default:
                brls::Logger::info("[mpv event] {}",
                                   mpv_event_name(ev->event_id));
                break;
        }
    }
}

// Paints `path` as the background, blurring it first when the artwork is a
// poster (see PlayerArt::blurBg).
void MpvView::setBackgroundArt(const std::string& path)
{
    if (!bgImage || path.empty()) return;
    std::string use = art.blurBg ? stremio::blurredPosterPath(path) : "";
    bgImage->setImageFromFile(use.empty() ? path : use);
}

void MpvView::buildLoadingOverlay(const std::string& title)
{
    brls::Theme theme = brls::Application::getTheme();

    // The dark theme's text_disabled is RGB(80,80,80) -- it is meant for a
    // disabled control on a flat background, and over the artwork it is barely
    // there. Secondary text here is dimmer than the title but still readable.
    const NVGcolor dimText = nvgRGB(190, 190, 195);

    // Full-screen centred column over the (black) video.
    loadingOverlay = new brls::Box();
    // Carries NO padding of its own: a percentage-sized child resolves against
    // the content box while an absolute one is placed against the padding box,
    // so the full-bleed background below came out 120px short (a black strip
    // down the right edge). The padding lives on the inner column instead.
    loadingOverlay->setAxis(brls::Axis::COLUMN);
    loadingOverlay->setGrow(1.0f);
    loadingOverlay->setBackgroundColor(theme.getColor("brls/background"));

    // Full-screen artwork behind everything else. Absolute so it is out of the
    // column's flow, and added first so the column draws on top of it.
    if (!art.bgId.empty() || !art.posterPath.empty())
    {
        bgImage = new brls::Image();
        bgImage->setPositionType(brls::PositionType::ABSOLUTE);
        bgImage->setPositionTop(0.0f);
        bgImage->setPositionLeft(0.0f);
        bgImage->setWidthPercentage(100.0f);
        bgImage->setHeightPercentage(100.0f);
        // FILL, not FIT: a poster is 2:3 and the screen is 16:9, so fitting it
        // would letterbox the "background" into a strip down the middle.
        bgImage->setScalingType(brls::ImageScalingType::FILL);
        // Faint enough that the text over it stays readable.
        bgImage->setAlpha(0.18f);
        loadingOverlay->addView(bgImage);

        // Show the thumbnail we already have right away, then swap in the
        // full-size art when it lands -- rather than a black screen for as long
        // as the download takes.
        setBackgroundArt(art.posterPath);
        if (!art.bgId.empty())
        {
            auto liveFlag = this->alive;
            stremio::fetchHqArtAsync(art.bgId, art.bgUrl,
                                     [this, liveFlag](std::string path) {
                                         if (!*liveFlag || path.empty()) return;
                                         setBackgroundArt(path);
                                     });
        }
    }

    // Everything the user actually reads, centred over the background.
    auto* column = new brls::Box();
    column->setAxis(brls::Axis::COLUMN);
    column->setJustifyContent(brls::JustifyContent::CENTER);
    column->setAlignItems(brls::AlignItems::CENTER);
    column->setGrow(1.0f);
    column->setPadding(0, 60, 0, 60);
    loadingOverlay->addView(column);

    // The poster when the caller knows what we're playing, otherwise the app
    // logo (bundled at romfs:/logo.png).
    auto* logo = new brls::Image();
    if (!art.posterPath.empty())
    {
        logo->setImageFromFile(art.posterPath);
        logo->setDimensions(148.0f, 222.0f);  // posters are 2:3
    }
    else
    {
        logo->setImageFromRes("logo.png");
        logo->setDimensions(148.0f, 148.0f);
    }
    logo->setScalingType(brls::ImageScalingType::FIT);
    logo->setMargins(0, 0, 28, 0);
    column->addView(logo);

    auto* titleLabel = new brls::Label();
    titleLabel->setText(title);
    titleLabel->setFontSize(28);
    titleLabel->setTextColor(theme.getColor("brls/text"));
    titleLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    titleLabel->setMargins(0, 0, 44, 0);
    column->addView(titleLabel);

    statusLabel = new brls::Label();
    statusLabel->setText("Connecting to peers...");
    statusLabel->setFontSize(21);
    statusLabel->setTextColor(theme.getColor("brls/text"));
    statusLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    statusLabel->setMargins(0, 0, 22, 0);
    column->addView(statusLabel);

    // Progress bar: a rounded track (spanning the screen width) holding an
    // accent-coloured fill.
    auto* barTrack = new brls::Box();
    barTrack->setAxis(brls::Axis::ROW);
    barTrack->setAlignItems(brls::AlignItems::CENTER);
    barTrack->setWidthPercentage(100.0f);
    barTrack->setHeight(10.0f);
    barTrack->setCornerRadius(5.0f);
    barTrack->setBackgroundColor(nvgRGBA(255, 255, 255, 38));

    barFill = new brls::Box();
    barFill->setHeight(10.0f);
    barFill->setWidthPercentage(0.0f);
    barFill->setCornerRadius(5.0f);
    barFill->setBackgroundColor(theme.getColor("brls/accent"));
    barTrack->addView(barFill);
    column->addView(barTrack);

    percentLabel = new brls::Label();
    percentLabel->setText("0%");
    percentLabel->setFontSize(18);
    percentLabel->setTextColor(theme.getColor("brls/text"));
    percentLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    percentLabel->setMargins(16, 0, 0, 0);
    column->addView(percentLabel);

    statsLabel = new brls::Label();
    statsLabel->setText("");
    statsLabel->setFontSize(16);
    statsLabel->setTextColor(dimText);
    statsLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    statsLabel->setMargins(30, 0, 0, 0);
    column->addView(statsLabel);

    // Animated spinner so it's clear the app is working even when the swarm is
    // slow to feed us.
    auto* spinner = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
    spinner->setDimensions(52.0f, 52.0f);
    spinner->setMargins(34, 0, 0, 0);
    spinner->animate(true);
    column->addView(spinner);

    // Hint that B cancels and returns to the list.
    auto* backHint = new brls::Label();
    backHint->setText("Press B to go back");
    backHint->setFontSize(18);
    backHint->setTextColor(dimText);
    backHint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    backHint->setMargins(40, 0, 0, 0);
    column->addView(backHint);

    this->addView(loadingOverlay);

    // Separate, small buffering badge shown over the video when playback stalls
    // (the swarm can't keep up). Hidden by default.
    bufferOverlay = new brls::Box();
    bufferOverlay->setAxis(brls::Axis::ROW);
    bufferOverlay->setAlignItems(brls::AlignItems::CENTER);
    bufferOverlay->setPositionType(brls::PositionType::ABSOLUTE);
    bufferOverlay->setPositionTop(40.0f);
    bufferOverlay->setPositionRight(40.0f);
    bufferOverlay->setPadding(12.0f, 20.0f, 12.0f, 20.0f);
    bufferOverlay->setCornerRadius(8.0f);
    bufferOverlay->setBackgroundColor(nvgRGBA(0, 0, 0, 170));
    bufferOverlay->setVisibility(brls::Visibility::GONE);

    auto* bufSpinner =
        new brls::ProgressSpinner(brls::ProgressSpinnerSize::NORMAL);
    bufSpinner->setDimensions(28.0f, 28.0f);
    bufSpinner->animate(true);
    bufferOverlay->addView(bufSpinner);

    auto* bufLabel = new brls::Label();
    bufLabel->setText("Buffering...");
    bufLabel->setFontSize(18);
    bufLabel->setTextColor(nvgRGB(255, 255, 255));
    bufLabel->setMargins(0, 0, 0, 12);
    bufferOverlay->addView(bufLabel);

    this->addView(bufferOverlay);

    // Centered pause icon (two bars) shown while the user has paused with A.
    pauseOverlay = new brls::Box();
    pauseOverlay->setAxis(brls::Axis::ROW);
    pauseOverlay->setJustifyContent(brls::JustifyContent::CENTER);
    pauseOverlay->setAlignItems(brls::AlignItems::CENTER);
    pauseOverlay->setGrow(1.0f);
    pauseOverlay->setPositionType(brls::PositionType::ABSOLUTE);
    pauseOverlay->setPositionTop(0.0f);
    pauseOverlay->setPositionLeft(0.0f);
    pauseOverlay->setWidthPercentage(100.0f);
    pauseOverlay->setHeightPercentage(100.0f);
    pauseOverlay->setVisibility(brls::Visibility::GONE);
    for (int i = 0; i < 2; i++)
    {
        auto* bar = new brls::Box();
        bar->setDimensions(26.0f, 92.0f);
        bar->setCornerRadius(6.0f);
        bar->setBackgroundColor(nvgRGBA(255, 255, 255, 235));
        bar->setMargins(0, 11, 0, 11);
        pauseOverlay->addView(bar);
    }
    this->addView(pauseOverlay);

    // Title, top-left, shown alongside the pause icon. Semi-transparent pill so
    // it reads over any frame.
    pauseTitleBox = new brls::Box();
    pauseTitleBox->setPositionType(brls::PositionType::ABSOLUTE);
    pauseTitleBox->setPositionTop(48.0f);
    pauseTitleBox->setPositionLeft(60.0f);
    pauseTitleBox->setPadding(12.0f, 22.0f, 12.0f, 22.0f);
    pauseTitleBox->setCornerRadius(8.0f);
    pauseTitleBox->setBackgroundColor(nvgRGBA(0, 0, 0, 140));
    pauseTitleBox->setVisibility(brls::Visibility::GONE);
    {
        auto* tl = new brls::Label();
        tl->setText(pauseTitle);
        tl->setFontSize(26.0f);
        tl->setTextColor(nvgRGB(255, 255, 255));
        tl->setSingleLine(true);
        pauseTitleBox->addView(tl);
    }
    this->addView(pauseTitleBox);

    // Seek bar at the bottom while paused: elapsed | progress | total.
    seekOverlay = new brls::Box();
    seekOverlay->setAxis(brls::Axis::ROW);
    seekOverlay->setAlignItems(brls::AlignItems::CENTER);
    seekOverlay->setPositionType(brls::PositionType::ABSOLUTE);
    seekOverlay->setPositionLeft(70.0f);
    seekOverlay->setPositionRight(70.0f);
    seekOverlay->setPositionBottom(56.0f);
    seekOverlay->setHeight(64.0f);
    seekOverlay->setPadding(0, 26, 0, 26);
    seekOverlay->setCornerRadius(10.0f);
    seekOverlay->setBackgroundColor(nvgRGBA(0, 0, 0, 165));
    seekOverlay->setVisibility(brls::Visibility::GONE);

    // Fixed width, or the bar jitters: a Label sizes to its text, the track
    // grows into whatever is left, so every "9:59" -> "10:00" widened the label
    // and shoved the track a few pixels. While scrubbing the timer changes
    // constantly, which made the whole bar shiver. Wide enough for "H:MM:SS".
    seekCur = new brls::Label();
    seekCur->setText("0:00");
    seekCur->setFontSize(22);
    seekCur->setTextColor(nvgRGB(255, 255, 255));
    seekCur->setWidth(104.0f);
    seekCur->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    seekCur->setMargins(0, 20, 0, 0);
    seekOverlay->addView(seekCur);

    seekTrack = new brls::Box();
    seekTrack->setAxis(brls::Axis::ROW);
    seekTrack->setAlignItems(brls::AlignItems::CENTER);
    seekTrack->setGrow(1.0f);
    seekTrack->setHeight(8.0f);
    seekTrack->setCornerRadius(4.0f);
    seekTrack->setBackgroundColor(nvgRGBA(255, 255, 255, 45));

    seekFill = new brls::Box();
    seekFill->setHeight(8.0f);
    seekFill->setWidth(0.0f);  // driven in pixels from the measured track width
    seekFill->setCornerRadius(4.0f);
    seekFill->setBackgroundColor(theme.getColor("brls/accent"));
    seekTrack->addView(seekFill);

    // Cursor: absolutely positioned, so it is NOT part of the row's flex flow.
    // As a normal child it sat next to the fill and competed for width, so the
    // fill got squeezed by the cursor's 6 px and could never reach 100% -- the
    // cursor visibly pushed the bar instead of riding on it. Its left offset is
    // driven from the same percentage as the fill (see updateSeekBar).
    seekCursor = new brls::Box();
    seekCursor->setPositionType(brls::PositionType::ABSOLUTE);
    seekCursor->setPositionLeft(0.0f);
    seekCursor->setDimensions(6.0f, 26.0f);
    seekCursor->setCornerRadius(3.0f);
    seekCursor->setBackgroundColor(nvgRGB(255, 255, 255));
    seekTrack->addView(seekCursor);

    seekOverlay->addView(seekTrack);

    seekTotal = new brls::Label();
    seekTotal->setText("0:00");
    seekTotal->setFontSize(22);
    seekTotal->setTextColor(nvgRGB(255, 255, 255));
    seekTotal->setWidth(104.0f);  // fixed for the same reason as seekCur
    seekTotal->setHorizontalAlign(brls::HorizontalAlign::LEFT);
    seekTotal->setMargins(0, 0, 0, 20);
    seekOverlay->addView(seekTotal);
    this->addView(seekOverlay);

    // Semi-transparent network/torrent info panel, toggled with ZR. Two columns:
    // the network/worker counters alone are longer than the screen is tall, so a
    // single column pushed the video section off the bottom where it could never
    // be read.
    infoOverlay = new brls::Box();
    infoOverlay->setAxis(brls::Axis::ROW);
    infoOverlay->setPositionType(brls::PositionType::ABSOLUTE);
    infoOverlay->setPositionTop(40.0f);
    infoOverlay->setPositionLeft(40.0f);
    infoOverlay->setWidth(960.0f);
    // An absolute-positioned box with auto height can lay out to zero size and
    // never render, so pin an explicit height big enough for all the lines.
    infoOverlay->setHeight(660.0f);
    infoOverlay->setPadding(16.0f, 20.0f, 16.0f, 20.0f);
    infoOverlay->setCornerRadius(8.0f);
    infoOverlay->setBackgroundColor(nvgRGBA(0, 0, 0, 150));
    infoOverlay->setVisibility(brls::Visibility::GONE);

    auto makeColumn = [&](float w) {
        auto* l = new brls::Label();
        l->setText("");
        l->setFontSize(18);
        l->setTextColor(nvgRGB(235, 235, 235));
        l->setIsWrapping(true);
        l->setWidth(w);
        infoOverlay->addView(l);
        return l;
    };
    infoLabel  = makeColumn(520.0f);  // network + workers
    infoLabel2 = makeColumn(380.0f);  // video + source
    infoLabel2->setMarginLeft(20.0f);
    this->addView(infoOverlay);
}

void MpvView::updateLoadingOverlay()
{
    if (!loadingOverlay)
        return;

    // The engine isn't open yet: for a magnet that means we're walking the
    // swarm for whoever will serve the metadata, one peer at a time. Show that
    // count -- without it the screen looks frozen for a minute and there is no
    // way to tell a slow fetch from a hung one.
    if (!tfs)
    {
        int tried = torrent_meta_peers_tried;
        int total = torrent_meta_peers_total;
        if (statusLabel && total > 0)
        {
            char b[96];
            std::snprintf(b, sizeof(b), "Metadonnees : peer %d / %d", tried,
                          total);
            statusLabel->setText(b);
        }
        return;
    }

    // Buffering percent (cheap; refresh every frame so the bar is smooth).
    // The bar tracks how much video mpv has buffered toward the kBufferSecs
    // target; 100% is reached exactly when playback is allowed to start.
    int pct = 0;
    if (fileLoaded && mpv)
    {
        // Async-observed values (pumpEvents): no sync mpv call on this thread.
        double secs = obsCacheSecs;
        if (secs >= 0.0)
        {
            pct = (int)(secs / kBufferSecs * 100.0);
            if (secs >= kBufferSecs)
                ready = true;  // buffered enough -> unpause and show video
        }

        // Safety net for short files / a full demuxer cache: if there's nothing
        // left to read, don't wait for the full kBufferSecs.
        if (obsCacheIdle && pct > 0)
            ready = true;
    }
    if (pct > 100)
        pct = 100;
    if (pct < 0)
        pct = 0;

    if (pct != shownPct)
    {
        shownPct = pct;
        barFill->setWidthPercentage((float)pct);
        char pb[16];
        std::snprintf(pb, sizeof(pb), "%d%%", pct);
        percentLabel->setText(pb);
    }

    // Text stats: refresh ~twice a second (each setText relayouts).
    auto now  = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - lastSample).count();
    if (dt >= 0.5)
    {
        int64_t bytes = tfs ? torrentfs_bytes_recv(tfs) : 0;
        speedBps      = (double)(bytes - lastBytes) / dt;
        if (speedBps < 0)
            speedBps = 0;
        lastBytes  = bytes;
        lastSample = now;

        int peers = tfs ? torrentfs_peer_count(tfs) : 0;
        int64_t piecesDone = 0, piecesTotal = 0, playhead = 0;
        if (tfs)
            torrentfs_stats(tfs, &piecesDone, &piecesTotal, &playhead);

        const char* status;
        if (!fileLoaded)
            status = (peers > 0) ? "Downloading header..." : "Connecting to peers...";
        else
            status = "Buffering...";
        statusLabel->setText(status);

        char sbuf[192];
        double spd = speedBps / (1024.0 * 1024.0);
        std::snprintf(sbuf, sizeof(sbuf),
                      "%d peer%s · %.1f MB/s · %lld / %lld pieces",
                      peers, peers == 1 ? "" : "s", spd, (long long)piecesDone,
                      (long long)piecesTotal);
        statsLabel->setText(sbuf);
    }
}

void MpvView::updateBufferIndicator()
{
    if (!bufferOverlay || !mpv)
        return;

    // Show the badge when the demuxer has almost nothing buffered ahead (the
    // stream is struggling); hide it once it recovers. Small hysteresis so it
    // doesn't flicker.
    double secs = obsCacheSecs;  // async-observed; no sync mpv call per frame
    bool stalling = buffering ? (secs < 1.0) : (secs < 0.25);
    if (stalling != buffering)
    {
        buffering = stalling;
        bufferOverlay->setVisibility(stalling ? brls::Visibility::VISIBLE
                                              : brls::Visibility::GONE);
    }
}

void MpvView::updateInfoOverlay()
{
    if (!infoOverlay || !infoShown)
        return;

    // Refresh ~twice a second (each setText relayouts).
    auto now  = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - infoLastSample).count();
    if (dt < 0.5)
        return;

    // Before torrentfs_open returns there is no engine to report on -- but that
    // is exactly the phase (a magnet's metadata fetch) that used to look hung,
    // so show what IS happening rather than an empty panel.
    if (!tfs)
    {
        infoLastSample = now;
        int tried = torrent_meta_peers_tried;
        int total = torrent_meta_peers_total;
        char m[512];
        std::snprintf(m, sizeof(m),
                      "METADATA\n"
                      "Fetching from peers (BEP 9)\n"
                      "Peers tried: %d / %d\n"
                      "%s",
                      tried, total,
                      total == 0 ? "Announcing to trackers..."
                                 : "First peer to answer wins; 8 are asked at "
                                   "once.");
        infoLabel->setText(m);
        if (infoLabel2) infoLabel2->setText("");
        return;
    }

    int64_t bytes = torrentfs_bytes_recv(tfs);
    infoSpeed     = (double)(bytes - infoLastBytes) / dt;
    if (infoSpeed < 0)
        infoSpeed = 0;
    infoLastBytes  = bytes;
    infoLastSample = now;

    int peers    = torrentfs_peer_count(tfs);
    int incoming = torrentfs_incoming_count(tfs);
    int64_t done = 0, total = 0, ph = 0;
    torrentfs_stats(tfs, &done, &total, &ph);
    int pct = total > 0 ? (int)(done * 100 / total) : 0;

    // Worker diagnostics. These are what tell apart "bytes arrive but never
    // become a piece": choked = peers never let us request, sha_fail = pieces
    // complete but fail verification, fetch_fail = sessions die mid-piece.
    int c[10] = { 0 };
    torrentfs_debug_counts(tfs, c);
    int live = 0, peak = 0, connecting = 0;
    torrentfs_live_peers(tfs, &live, &peak, &connecting);
    int sockFail = 0, connTimeouts = 0;
    torrentfs_fail_kinds(tfs, &sockFail, &connTimeouts);
    int claiming = 0, idleUnchoked = 0;
    torrentfs_claim_stats(tfs, &claiming, &idleUnchoked);
    int bfEmpty = 0, bfOk = 0, bfBad = 0;
    torrentfs_bitfield_stats(tfs, &bfEmpty, &bfOk, &bfBad);
    int64_t wph = 0, wlo = 0, whi = 0;
    int claimFail = 0, claimOk = 0, inflight = 0;
    torrentfs_claim_debug(tfs, &wph, &wlo, &whi, &claimFail, &claimOk, &inflight);
    int cacheWrFail = 0, cacheRdShort = 0;
    int64_t cacheTotal = 0;
    torrentfs_cache_stats(tfs, &cacheWrFail, &cacheRdShort, &cacheTotal);
    char lastErr[128] = { 0 };
    torrentfs_last_err(tfs, lastErr, sizeof(lastErr));
    int64_t plen = torrentfs_piece_len(tfs);

    // Playback / decode stats. If "Dropped" climbs during the stutters it's the
    // decoder/CPU that can't keep up; if it stays flat with a full buffer, the
    // hitch is frame pacing/judder, not dropped frames.
    double buf = 0.0, fps = 0.0;
    int64_t w = 0, h = 0, drop = 0, decDrop = 0;
    char* hwdec = nullptr;
    if (mpv)
    {
        mpv_get_property(mpv, "demuxer-cache-duration", MPV_FORMAT_DOUBLE, &buf);
        mpv_get_property(mpv, "width", MPV_FORMAT_INT64, &w);
        mpv_get_property(mpv, "height", MPV_FORMAT_INT64, &h);
        mpv_get_property(mpv, "container-fps", MPV_FORMAT_DOUBLE, &fps);
        mpv_get_property(mpv, "frame-drop-count", MPV_FORMAT_INT64, &drop);
        mpv_get_property(mpv, "decoder-frame-drop-count", MPV_FORMAT_INT64,
                        &decDrop);
        mpv_get_property(mpv, "hwdec-current", MPV_FORMAT_STRING, &hwdec);
    }
    const char* hw =
        (hwdec && hwdec[0] && std::strcmp(hwdec, "no") != 0) ? hwdec : "software";

    // Video buffered in RAM: mpv's demuxer forward cache. Read out of the
    // demuxer-cache-state node's total-bytes field.
    int64_t ramBytes = 0;
    if (mpv)
    {
        mpv_node node;
        if (mpv_get_property(mpv, "demuxer-cache-state", MPV_FORMAT_NODE, &node) >= 0)
        {
            if (node.format == MPV_FORMAT_NODE_MAP && node.u.list)
                for (int i = 0; i < node.u.list->num; i++)
                    if (std::strcmp(node.u.list->keys[i], "total-bytes") == 0 &&
                        node.u.list->values[i].format == MPV_FORMAT_INT64)
                        ramBytes = node.u.list->values[i].u.int64;
            mpv_free_node_contents(&node);
        }
    }

    // SD-card write rate: how fast verified pieces are landing in the cache.
    // Differentiates the engine's written-bytes counter -- NOT cacheTotal,
    // which is the (constant) size the cache must reach, whose delta showed a
    // permanent 0 here.
    int64_t cacheWritten = torrentfs_cache_written(tfs);
    double sdWrite = (double)(cacheWritten - infoLastCache) / dt;
    if (sdWrite < 0) sdWrite = 0;
    infoLastCache  = cacheWritten;

    // Syscall probes: calls/s (delta of the cumulative counts) plus the worst
    // single call since the last refresh. During a console-wide freeze the
    // panel cannot redraw, but the engine keeps recording -- the first refresh
    // after the freeze shows which call class was stuck, i.e. which OS service
    // (bsd or fs) the console was waiting on.
    uint32_t latN[5];
    uint64_t latUs[5];
    torrentfs_lat_stats(tfs, latN, latUs);
    unsigned latRate[5];
    unsigned long long latMs[5];
    for (int i = 0; i < 5; i++)
    {
        latRate[i]        = (unsigned)((latN[i] - infoLastLatN[i]) / (dt > 0 ? dt : 1));
        infoLastLatN[i]   = latN[i];
        latMs[i]          = latUs[i] / 1000;
    }

    // Left column: everything about the swarm. Right column: what came out of
    // it -- the video, and the source it is being read from.
    char text[1536];
    std::snprintf(text, sizeof(text),
                  "NETWORK\n"
                  "Peers: %d (+%d incoming)\n"
                  "Speed: %.2f MB/s\n"
                  "Downloaded: %.1f MB\n"
                  "Pieces: %lld / %lld (%d%%)  piece=%lld KB\n"
                  "Buffer: %.1f s\n"
                  "\n"
                  "WORKERS\n"
                  "live now: %d   peak: %d   connecting: %d\n"
                  "downloading: %d   idle unchoked: %d\n"
                  "bitfield: %d empty   %d ok   %d bad\n"
                  "claim: ph=%lld win=[%lld,%lld)  ok=%d fail=%d\n"
                  "inflight pieces: %d\n"
                  "cache: %.2f GB  wr fail: %d  rd short: %d\n"
                  "SD write: %.2f MB/s\n"
                  "sys calls/s | max ms (calm=%d):\n"
                  "poll %u|%llu  recv %u|%llu  send %u|%llu\n"
                  "sd wr %u|%llu  sd rd %u|%llu\n"
                  "conn ok/fail: %d / %d\n"
                  "fail: %d sock/local   %d timeout\n"
                  "unchoked: %d   choked: %d\n"
                  "piece ok: %d   fetch fail: %d   sha fail: %d\n"
                  "up: %d blocks (%d interested, %d req)\n"
                  "last err: %s",
                  peers, incoming, infoSpeed / (1024.0 * 1024.0),
                  (double)bytes / (1024.0 * 1024.0), (long long)done,
                  (long long)total, pct, (long long)(plen / 1024), buf,
                  live, peak, connecting,
                  claiming, idleUnchoked,
                  bfEmpty, bfOk, bfBad,
                  (long long)wph, (long long)wlo, (long long)whi, claimOk, claimFail,
                  inflight,
                  (double)cacheTotal / (1024.0 * 1024.0 * 1024.0), cacheWrFail,
                  cacheRdShort,
                  sdWrite / (1024.0 * 1024.0),
                  torrentfs_calm(tfs),
                  latRate[0], latMs[0], latRate[1], latMs[1],
                  latRate[2], latMs[2],
                  latRate[3], latMs[3], latRate[4], latMs[4],
                  c[0], c[1],
                  sockFail, connTimeouts,
                  c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9],
                  lastErr[0] ? lastErr : "-");
    infoLabel->setText(text);

    char right[768];
    std::snprintf(right, sizeof(right),
                  "VIDEO\n"
                  "%lldx%lld @ %.3f fps\n"
                  "Decode: %s\n"
                  "Dropped: %lld (decoder %lld)\n"
                  "RAM buffer: %.0f MB\n"
                  "\n"
                  "SOURCE\n"
                  "%s\n"
                  "%.2f GB  %lld pieces of %lld KB\n"
                  "\n"
                  "METADATA\n"
                  "Peers tried: %d / %d",
                  (long long)w, (long long)h, fps,
                  hw, (long long)drop, (long long)decDrop,
                  (double)ramBytes / (1024.0 * 1024.0),
                  torrentfs_name(tfs),
                  (double)torrentfs_size(tfs) / (1024.0 * 1024.0 * 1024.0),
                  (long long)total, (long long)(plen / 1024),
                  torrent_meta_peers_tried, torrent_meta_peers_total);
    if (infoLabel2) infoLabel2->setText(right);

    if (hwdec)
        mpv_free(hwdec);
}

void MpvView::logStats()
{
    if (!tfs)
        return;

    auto now = std::chrono::steady_clock::now();
    if (!statsStarted)
    {
        statsStarted    = true;
        statsStart      = now;
        statsLastSample = now;
        statsLastBytes  = torrentfs_bytes_recv(tfs);
        return;
    }
    double dt = std::chrono::duration<double>(now - statsLastSample).count();
    if (dt < 2.0)
        return;

    // Piggyback on this 2 s cadence: keep the last known playback position for
    // the Stremio watch-state sync, and push it out every couple of minutes.
    if (ready && mpv)
    {
        // Async-observed (pumpEvents): this runs on the UI thread, which must
        // not block on the mpv core.
        if (obsPos > 0) lastPosSec = obsPos;
        if (obsDur > 0) lastDurSec = obsDur;
        maybePushWatchState(false);
    }

    int64_t bytes  = torrentfs_bytes_recv(tfs);
    int64_t stored = torrentfs_stored_bytes(tfs);
    double kbps    = (double)(bytes - statsLastBytes) / dt / 1024.0;
    statsLastBytes = bytes;
    statsLastSample = now;
    int elapsed = (int)std::chrono::duration<double>(now - statsStart).count();

    int64_t done = 0, total = 0, ph = 0;
    torrentfs_stats(tfs, &done, &total, &ph);
    int live = 0, peak = 0, connecting = 0;
    torrentfs_live_peers(tfs, &live, &peak, &connecting);
    int claiming = 0, idle = 0;
    torrentfs_claim_stats(tfs, &claiming, &idle);
    int bfEmpty = 0, bfOk = 0, bfBad = 0;
    torrentfs_bitfield_stats(tfs, &bfEmpty, &bfOk, &bfBad);
    int64_t wph = 0, wlo = 0, whi = 0;
    int cFail = 0, cOk = 0, infl = 0;
    torrentfs_claim_debug(tfs, &wph, &wlo, &whi, &cFail, &cOk, &infl);
    int wrFail = 0, rdShort = 0;
    int64_t cacheTotal = 0;
    torrentfs_cache_stats(tfs, &wrFail, &rdShort, &cacheTotal);
    int sockFail = 0, tmo = 0;
    torrentfs_fail_kinds(tfs, &sockFail, &tmo);
    int c[10] = { 0 };
    torrentfs_debug_counts(tfs, c);
    char lastErr[128] = { 0 };
    torrentfs_last_err(tfs, lastErr, sizeof(lastErr));

    double buf = obsCacheSecs;  // async-observed; no sync mpv call here

    // The piece the player is blocked on: when ph freezes, this says why.
    int pst = -1, phv = 0, prq = 0, ptot = 0;
    torrentfs_piece_debug(tfs, ph, &pst, &phv, &prq, &ptot);
    static const char* kSt[] = { "NEEDED", "INFLIGHT", "DONE", "VERIFYING" };
    const char* pstName = (pst >= 0 && pst <= 3) ? kSt[pst] : "?";

    // Syscall-latency peaks since the last sample: during a console freeze
    // these say which OS service stalled (bsd for poll/recv/send, fs for
    // w/rd) and for how long. Reading clears the peaks, which the ZR panel
    // also does -- with the panel open the two readers split the values, so
    // trust the log only when the panel is closed.
    uint32_t latN[5];
    uint64_t latUs[5];
    torrentfs_lat_stats(tfs, latN, latUs);
    // Calls per interval too (cumulative counts, so panel reads don't skew
    // them): distinguishes "netloop starved" (few calls) from "idle" (few
    // calls, but nothing pending) vs "busy and fast" (many calls, low max).
    uint32_t latC[5];
    for (int i = 0; i < 5; i++)
    {
        latC[i]          = latN[i] - statsLastLatN[i];
        statsLastLatN[i] = latN[i];
    }

    // Thread heartbeats: age (ms since the thread last ran) @ the core it
    // last ran on. During a freeze the stalled threads' ages balloon while
    // the survivors stay near 0 -- and the cores say who shares a starving
    // core with whom. Note: the stats thread itself must be alive to log, so
    // the first line AFTER a freeze carries the peak ages.
    uint32_t hbAge[4];
    int hbCore[4];
    torrentfs_heartbeats(tfs, hbAge, hbCore);

    // Wifi bars (0..3): if this dips exactly when an episode starts, the
    // freeze is the wifi driver's environment (scan/roam/interference), not
    // anything the engine did. 9 = query failed / not wifi.
    u32 wifi = 9;
    {
        NifmInternetConnectionType ictype;
        NifmInternetConnectionStatus icstat;
        if (R_FAILED(nifmGetInternetConnectionStatus(&ictype, &wifi, &icstat)))
            wifi = 9;
    }

    // One line per sample, fixed field order, so the whole run can be read as a
    // trend (and grepped) instead of guessed at from a screenshot.
    brls::Logger::info(
        "[stats] t={}s spd={:.0f}KB/s dl={:.1f}MB stored={:.1f}MB dup={:.1f}MB "
        "pc={}/{} buf={:.1f}s | "
        "live={} peak={} conn={} dling={} idle={} | bf={}/{}/{} | "
        "claim ph={} win=[{},{}) ok={} fail={} infl={} | "
        "PH-PIECE {} have={}/{} req={} | "
        "cache={:.2f}GB wr_fail={} rd_short={} | "
        "conn_ok={} conn_fail={} sock={} tmo={} | "
        "unchoked={} pok={} ffail={} sha={} | up={} int={} req={} | "
        "lat p={}:{} r={}:{} s={}:{} w={}:{} rd={}:{} maxms:calls | "
        "hb net={}@{} wr={}@{} rd={}@{} ui={}@{} ms@core | "
        "calm={} wifi={} | err={}",
        elapsed, kbps, (double)bytes / (1024.0 * 1024.0),
        (double)stored / (1024.0 * 1024.0),
        (double)(bytes - stored) / (1024.0 * 1024.0), (long long)done,
        (long long)total, buf,
        live, peak, connecting, claiming, idle,
        bfEmpty, bfOk, bfBad,
        (long long)wph, (long long)wlo, (long long)whi, cOk, cFail, infl,
        pstName, phv, ptot, prq,
        (double)cacheTotal / (1024.0 * 1024.0 * 1024.0), wrFail, rdShort,
        c[0], c[1], sockFail, tmo,
        c[2], c[4], c[5], c[6], c[7], c[8], c[9],
        (unsigned long long)(latUs[0] / 1000), latC[0],
        (unsigned long long)(latUs[1] / 1000), latC[1],
        (unsigned long long)(latUs[2] / 1000), latC[2],
        (unsigned long long)(latUs[3] / 1000), latC[3],
        (unsigned long long)(latUs[4] / 1000), latC[4],
        hbAge[0], hbCore[0], hbAge[1], hbCore[1],
        hbAge[2], hbCore[2], hbAge[3], hbCore[3],
        torrentfs_calm(tfs), (unsigned)wifi,
        lastErr[0] ? lastErr : "-");
}

// Reports the watched position to the Stremio API. Rate-limited to one push
// every 2 minutes unless `force` (teardown). Below ~30 s of playback nothing is
// sent: opening a stream and backing out should not rewrite the account's
// "continue watching" position.
void MpvView::maybePushWatchState(bool force)
{
    if (watch.authKey.empty() || watch.itemId.empty())
        return;
    if (lastPosSec < 30.0)
        return;
    auto now = std::chrono::steady_clock::now();
    if (!force && watchPushValid &&
        std::chrono::duration<double>(now - watchPushLast).count() < 120.0)
        return;
    watchPushLast  = now;
    watchPushValid = true;
    stremio::pushWatchStateAsync(watch.authKey, watch.itemId, watch.videoId,
                                 lastPosSec, lastDurSec);
}

// Enter scrub mode: pause and pin the cursor to where playback currently is.
void MpvView::beginSeek()
{
    seeking = true;
    // Async-observed values: even this one-shot must not block the UI thread
    // if the user scrubs during an mpv hiccup.
    seekDur    = obsDur;
    seekTarget = obsPos;
    mpv_set_property_string(mpv, "pause", "yes");
    if (pauseOverlay) pauseOverlay->setVisibility(brls::Visibility::VISIBLE);
    if (seekOverlay) seekOverlay->setVisibility(brls::Visibility::VISIBLE);
    seekHeld       = 0.0;
    seekFrameValid = false;
}

// Analog scrubbing, driven every frame from the stick's X axis rather than from
// button events: only the axis carries how far it is pushed, which is what makes
// a slow nudge and a fast sweep the same gesture.
void MpvView::updateStickSeek()
{
    if (!ready || !mpv)
        return;

    brls::ControllerState st {};
    brls::Application::getPlatform()->getInputManager()->updateUnifiedControllerState(&st);
    float x = st.axes[brls::ControllerAxis::LEFT_X];

    auto now = std::chrono::steady_clock::now();
    double dt = seekFrameValid
                    ? std::chrono::duration<double>(now - seekLastFrame).count()
                    : 0.0;
    seekLastFrame  = now;
    seekFrameValid = true;
    if (dt > 0.25) dt = 0.25;  // a hitch must not teleport the cursor

    if (x > -kStickDeadzone && x < kStickDeadzone)
    {
        seekHeld = 0.0;  // released: next push starts slow again
        return;
    }

    if (!seeking)
        beginSeek();

    seekHeld += dt;
    double ramp = 1.0 + (kSeekAccelMax - 1.0) *
                            (seekHeld < kSeekAccelSecs ? seekHeld / kSeekAccelSecs : 1.0);
    // Square the tilt: fine control near centre, full speed at the edge.
    double tilt = (double)x;
    double rate = tilt * (tilt < 0 ? -tilt : tilt) * kSeekBaseRate * ramp;

    seekTarget += rate * dt;
    if (seekTarget < 0.0) seekTarget = 0.0;
    if (seekDur > 0.0 && seekTarget > seekDur) seekTarget = seekDur;
}

void MpvView::updateSeekBar()
{
    if (!seekOverlay || !mpv)
        return;
    // Async-observed (pumpEvents): the seek bar refreshes every frame and
    // must never block on the mpv core.
    double pos = obsPos, dur = obsDur;

    if (seeking && seekDur > 0.0)
        dur = seekDur;

    // Two different things, so two different values -- driving both from one
    // number made the played-progress bar jump around with the stick, claiming
    // we had watched up to wherever the cursor happened to point.
    //   fill   = what has actually been played (time-pos). Paused while
    //            scrubbing, so it correctly stays put.
    //   cursor = where the scrub is aiming; equals playback when not scrubbing.
    double cursorAt = seeking ? seekTarget : pos;

    auto frac = [dur](double t) {
        if (dur <= 0.0) return 0.0f;
        float f = (float)(t / dur);
        return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
    };

    // The timer follows the cursor: while scrubbing it is the target time that
    // matters, not the frozen playback clock.
    seekCur->setText(fmtTime(cursorAt));
    seekTotal->setText(fmtTime(dur));

    // Place both in pixels off the same measured width. As percentages they
    // drifted apart the further along the bar you got: yoga resolves a child's
    // width% against the parent's content box but an absolute child's left%
    // against its padding box, so the two scales diverge and the fill ran past
    // the cursor. One width, one unit, no room to disagree.
    float trackW = seekTrack ? seekTrack->getWidth() : 0.0f;
    if (trackW <= 0.0f)
        return;  // not laid out yet; next frame will have it

    float cw = seekCursor ? seekCursor->getWidth() : 0.0f;
    seekFill->setWidth(trackW * frac(pos));
    if (seekCursor)
    {
        // Keep the cursor inside the track at both ends rather than hanging off.
        float x = trackW * frac(cursorAt) - cw / 2.0f;
        if (x < 0.0f) x = 0.0f;
        if (x > trackW - cw) x = trackW - cw;
        seekCursor->setPositionLeft(x);
    }
}

void MpvView::draw(NVGcontext* vg, float x, float y, float width, float height,
                   brls::Style style, brls::FrameContext* ctx)
{
    pumpEvents();  // also feeds the engine's backlog (async property observe)
    if (tfs)
        torrentfs_hb_ui(tfs);  // render-thread heartbeat for the freeze probes

    // A touch on the screen flips borealis into TOUCH input mode, and the first
    // gamepad press after that is consumed just to switch back -- so A took two
    // presses to pause/resume once the screen had been tapped. Nothing here uses
    // touch, so keep it in GAMEPAD mode; the next A then acts on the first press.
    if (brls::Application::getInputType() == brls::InputType::TOUCH)
        brls::Application::setInputType(brls::InputType::GAMEPAD);

    // No render context yet: the engine is still opening (a magnet spends up to
    // a minute finding a peer that will serve its metadata). There is no video
    // to composite, but the loading screen still has to be drawn -- returning
    // here left the whole view blank, which is exactly the black screen the
    // async open was supposed to fix.
    if (!renderCtx)
    {
        updateLoadingOverlay();
        brls::Box::draw(vg, x, y, width, height, style, ctx);
        return;
    }

    // nanovg is a *batched* renderer: every nvg* call in this frame (including
    // this view's opaque black background drawn by View::frame just before us,
    // and any lower view) is only submitted to GL at the outer nvgEndFrame --
    // which runs AFTER this draw(). mpv_render_context_render() below draws
    // *immediately*, so without this split the queued black background gets
    // flushed on top of the video -> black screen with working audio, and the
    // video only flashes through on the pop transition when that fill is gone.
    //
    // Flush everything queued so far now (paints the black letterbox UNDER the
    // video), render mpv, then restart the nanovg frame so the red overlay and
    // borealis' outer nvgEndFrame composite ON TOP of the video.
    nvgEndFrame(vg);

    GLint fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);

    mpv_opengl_fbo mfbo;
    mfbo.fbo              = fbo;
    mfbo.w               = (int)brls::Application::windowWidth;
    mfbo.h               = (int)brls::Application::windowHeight;
    mfbo.internal_format = 0;
    int flip             = 1;
    mpv_render_param params[] = {
        { MPV_RENDER_PARAM_OPENGL_FBO, &mfbo },
        { MPV_RENDER_PARAM_FLIP_Y, &flip },
        { MPV_RENDER_PARAM_INVALID, nullptr },
    };

    // borealis/nanovg leaves scissor/stencil/blend/colormask enabled from the
    // UI pass; mpv's render honors them and its output gets clipped away (black).
    // Reset to a clean full-screen state before handing the framebuffer to mpv.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, brls::Application::windowWidth, brls::Application::windowHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    mpv_render_context_render(renderCtx, params);

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, brls::Application::windowWidth, brls::Application::windowHeight);

    // The Switch compositor honors the framebuffer's alpha channel. mpv leaves
    // the video pixels with alpha 0 -> they composite as transparent (black).
    // Force the whole framebuffer opaque by clearing only the alpha channel to 1
    // (RGB / the video is left untouched via the color mask).
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    mpv_render_context_report_swap(renderCtx);

    // Resume the nanovg frame that we flushed above, matching the parameters
    // Application::frame() opened it with, so the loading overlay (and the outer
    // nvgEndFrame) draw on top of the video instead of under it.
    double scaleFactor =
        brls::Application::getPlatform()->getVideoContext()->getScaleFactor();
    nvgBeginFrame(vg, brls::Application::windowWidth,
                  brls::Application::windowHeight, (float)scaleFactor);
    nvgScale(vg, brls::Application::windowScale, brls::Application::windowScale);

    // Loading screen: keep it fed until the first frame is up, then take it down
    // once so it stops laying out. Drawn via Box::draw (our child views) on top
    // of the freshly rendered video.
    if (!ready)
        updateLoadingOverlay();
    if (ready && !overlayHidden && loadingOverlay)
    {
        // Buffered enough: start playback and take the loading screen down.
        if (mpv)
            mpv_set_property_string(mpv, "pause", "no");
        loadingOverlay->setVisibility(brls::Visibility::GONE);
        overlayHidden = true;
    }
    if (overlayHidden)
        updateBufferIndicator();
    if (ready)
        updateStickSeek();  // must run every frame: the stick is an axis, not an event
    if (userPaused || seeking)
        updateSeekBar();
    updateInfoOverlay();
    logStats();  // always, even with the ZR panel closed

    brls::Box::draw(vg, x, y, width, height, style, ctx);
}

PlayerActivity::PlayerActivity(std::string source, PlayerArt art,
                               std::string title, int fileIndex,
                               WatchInfo watch)
    : source(std::move(source))
    , art(std::move(art))
    , title(std::move(title))
    , fileIndex(fileIndex)
    , watch(std::move(watch))
{
}

brls::View* PlayerActivity::createContentView()
{
    return new MpvView(source, art, title, fileIndex, watch);
}
