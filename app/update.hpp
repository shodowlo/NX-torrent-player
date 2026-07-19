#pragma once

#include <functional>
#include <string>

// Self-update against GitHub releases.
//
// The .nro cannot be overwritten while it runs: hbloader keeps the file open and
// the app's own romfs (icons, borealis resources) is read from it on demand. So
// a download lands next to it as "<name>.nro.new", and applyPending() swaps it
// in at exit, once romfs is unmounted. The update takes effect on the next
// launch, and there is never a moment where the installed .nro is half-written.
namespace update
{

struct Release
{
    bool ok = false;       // the check completed (not: an update exists)
    bool newer = false;    // ok, and the release is newer than APP_VERSION
    std::string version;   // "0.2.0" (the tag's leading 'v' stripped)
    std::string notes;     // release body, shown in the prompt
    // The .nro asset -- empty when the release ships without one (only the zip,
    // say). Kept apart from `newer` on purpose: "a new version exists" and "we
    // can install it" are different facts, and folding them together turned a
    // missing asset into a cheerful "you are on the latest version".
    std::string url;
    int64_t size = 0;      // its size in bytes, 0 if the API did not say
    std::string error;     // set when !ok

    // Both true = we can offer the update.
    bool installable() const { return newer && !url.empty(); }
};

// Records where we are running from (argv[0]). Call once, from main, before
// anything else here: without it updating is disabled rather than guessing.
void init(int argc, char** argv);

// Asks GitHub for the latest release. Calls back on the UI thread. A failure --
// no wifi, rate limit, no release yet -- is not worth a popup: check `ok` and
// stay quiet.
void checkAsync(std::function<void(Release)> done);

// Downloads `r`'s .nro next to the running one, as "<name>.nro.new".
// `progress` is called on the UI thread with 0..1 (-1 while the size is
// unknown); `done` with "" on success or a reason on failure.
void downloadAsync(const Release& r, std::function<void(float)> progress,
                   std::function<void(std::string)> done);

// Shows the "version X is available" prompt, and drives the download from it:
// progress, then a note that it applies on the next launch. The UI lives here so
// both the startup check and the Options button behave identically.
void promptInstall(const Release& r);

// Quits, asking the loader to start us again -- which, after applyPending() has
// run on the way out, means the new version. False if the loader cannot do it
// (then the user has to relaunch by hand).
bool restartNow();

// Renames a completed download over the running .nro. Call at exit ONLY, after
// romfs is gone. No-op when nothing is pending.
void applyPending();

// True if a download is waiting to be applied at exit.
bool hasPending();

// Where the running .nro lives. "" if it cannot be determined (which disables
// updating rather than guessing at a path to overwrite).
std::string selfPath();

} // namespace update
