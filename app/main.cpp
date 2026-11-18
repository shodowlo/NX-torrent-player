/*
    SwitchTorrent — borealis front-end (official Horizon-style UI).
    Stage 1: file browser only. mpv playback + torrent engine wired in next.
*/

#include <borealis.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cstdlib>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <map>
#include <memory>
#include <set>
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
#include "theme.hpp"
#include "update.hpp"
#include "stremio.hpp"

extern "C" {
#include "torrent.h"
#include "torrentfs.h"
}

#include "i18n.hpp"

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
    mkdir(APPDATA_SUBS, 0777);     // subtitles pulled from Stremio addons

    // Seed magnet.txt so the user has a file to edit from a PC (and knows the
    // format). Only when it does not exist yet -- never clobber their list.
    if (FILE* f = std::fopen(APPDATA_MAGNETS, "rb"))
    {
        std::fclose(f);
    }
    else if (FILE* w = std::fopen(APPDATA_MAGNETS, "wb"))
    {
        std::fprintf(w,
                     "# One magnet link or info-hash per line -- these show up in "
                     "the Local tab.\n"
                     "# Lines starting with # are ignored. Examples:\n"
                     "# magnet:?xt=urn:btih:<hash>&dn=Some+Movie\n"
                     "# <40-character info hash>\n");
        std::fclose(w);
    }
}


struct TorrentEntry
{
    std::string name;      // display name (file name, or the magnet's dn=)
    std::string path;      // .torrent path on the SD card, or a magnet: URI
    std::string sizeText;  // whole-torrent size; "magnet" for magnets (size unknown)
    bool isMagnet     = false;
    bool needsResolve = false;  // a magnet whose name+files are not cached yet
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

// Trackers appended to a bare info-hash (same set as the Stremio path). Public
// UDP trackers most torrents announce to anyway; DHT fills the rest.
constexpr const char* kMagnetTrackers =
    "&tr=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce"
    "&tr=udp%3A%2F%2Fopen.demonii.com%3A1337%2Fannounce"
    "&tr=udp%3A%2F%2Ftracker.openbittorrent.com%3A6969%2Fannounce"
    "&tr=udp%3A%2F%2Fexodus.desync.com%3A6969%2Fannounce"
    "&tr=udp%3A%2F%2Ftracker.torrent.eu.org%3A451%2Fannounce"
    "&tr=udp%3A%2F%2Fopen.stealth.si%3A80%2Fannounce";

// Turn what the user pasted/typed into a playable magnet URI, or "" if it is not
// one. Accepts a full "magnet:?..." link OR a bare info-hash (40 hex / 32 base32)
// -- the latter is far less painful to type on the on-screen keyboard, and the
// engine only needs the hash plus some trackers to start.
std::string normalizeMagnet(std::string s)
{
    // Trim surrounding whitespace.
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    s = s.substr(a, b - a + 1);

    if (s.compare(0, 7, "magnet:") == 0)
    {
        // Already a magnet. Add trackers if it carries none (a trackerless magnet
        // otherwise leans entirely on DHT, which is slow to bootstrap on wifi).
        if (s.find("&tr=") == std::string::npos && s.find("?tr=") == std::string::npos)
            s += kMagnetTrackers;
        return s;
    }

    // A bare hash? 40 hex or 32 base32 characters, nothing else.
    auto isHex = [](const std::string& h) {
        if (h.size() != 40) return false;
        for (char c : h)
            if (!std::isxdigit((unsigned char)c)) return false;
        return true;
    };
    auto isB32 = [](const std::string& h) {
        if (h.size() != 32) return false;
        for (char c : h)
        {
            char u = (char)std::toupper((unsigned char)c);
            if (!((u >= 'A' && u <= 'Z') || (u >= '2' && u <= '7'))) return false;
        }
        return true;
    };
    if (isHex(s) || isB32(s))
        return "magnet:?xt=urn:btih:" + s + kMagnetTrackers;
    return "";
}

// A friendly name for a magnet: its dn= (display name) if present, else a short
// "Magnet <hash-prefix>".
std::string magnetName(const std::string& magnet)
{
    size_t dn = magnet.find("dn=");
    if (dn != std::string::npos)
    {
        dn += 3;
        size_t end = magnet.find('&', dn);
        std::string raw = magnet.substr(dn, end == std::string::npos ? end : end - dn);
        // Minimal percent-decode + '+' -> space, enough to read the name.
        std::string out;
        for (size_t i = 0; i < raw.size(); i++)
        {
            if (raw[i] == '+') out += ' ';
            else if (raw[i] == '%' && i + 2 < raw.size())
            {
                auto hex = [](char c) {
                    if (c >= '0' && c <= '9') return c - '0';
                    c = (char)std::tolower((unsigned char)c);
                    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0;
                };
                out += (char)(hex(raw[i + 1]) * 16 + hex(raw[i + 2]));
                i += 2;
            }
            else out += raw[i];
        }
        if (!out.empty()) return out;
    }
    size_t h = magnet.find("btih:");
    if (h != std::string::npos)
        return tr("Magnet ") + magnet.substr(h + 5, 8);
    return tr("Magnet");
}

// The lower-cased btih info-hash of a magnet, or "" if it has none.
std::string magnetHash(const std::string& magnet)
{
    size_t p = magnet.find("btih:");
    if (p == std::string::npos) return "";
    p += 5;
    size_t e = magnet.find('&', p);
    std::string h = magnet.substr(p, e == std::string::npos ? e : e - p);
    for (auto& c : h) c = (char)std::tolower((unsigned char)c);
    return h;
}

// Defined with the rest of the magnet-files cache below; needed up here by
// scanMagnets to label a row with the size it recorded.
int64_t magnetCachedTotal(const std::string& hash);

// Whether a magnet's file list has been resolved and cached (APPDATA_MAGNET_FILES
// holds a line for its hash, even if that line is the "no video" marker).
bool magnetFilesCached(const std::string& hash)
{
    if (hash.empty()) return false;
    FILE* f = std::fopen(APPDATA_MAGNET_FILES, "rb");
    if (!f) return false;
    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    std::fclose(f);
    // A whole-line prefix "<hash>\t" -- at the start or right after a newline.
    std::string key = hash + "\t";
    if (body.compare(0, key.size(), key) == 0) return true;
    return body.find("\n" + key) != std::string::npos;
}

// The magnet.txt list: one magnet or info-hash per line ('#' comments and blank
// lines skipped). Each becomes an entry shown next to the .torrents.
std::vector<TorrentEntry> scanMagnets(const std::string& path)
{
    std::vector<TorrentEntry> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    std::fclose(f);

    size_t i = 0;
    while (i < body.size())
    {
        size_t nl = body.find('\n', i);
        std::string line = body.substr(i, nl == std::string::npos ? nl : nl - i);
        i = nl == std::string::npos ? body.size() : nl + 1;
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos || line[s] == '#') continue;
        std::string magnet = normalizeMagnet(line);
        if (magnet.empty()) continue;
        // Resolve (name + file list) until the files are cached, even if a dn=
        // name is already there -- we still need the file list to stream.
        std::string hash = magnetHash(magnet);
        bool cached      = magnetFilesCached(hash);
        // Show the real size once it has been recorded, like a .torrent does.
        // "magnet" is the fallback: a magnet resolved before that was recorded
        // has no -2 line, and its files are cached so it never resolves again.
        std::string size = humanSize(magnetCachedTotal(hash));
        out.push_back({ magnetName(magnet), magnet,
                        size.empty() ? "magnet" : size, true, !cached });
    }
    return out;
}

// Minimal percent-encoding for a magnet's dn= value.
std::string urlEncodeName(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')
            out += c;
        else
        {
            char b[4];
            std::snprintf(b, sizeof(b), "%%%02X", (unsigned char)c);
            out += b;
        }
    }
    return out;
}

// Once a magnet's metadata resolves on device, write its real name into
// magnet.txt (as &dn=...) if that line did not have one -- so it lists with a
// readable name from then on. Matched by info-hash; other lines are untouched.
void updateMagnetName(const std::string& magnet, const std::string& name)
{
    std::string want = magnetHash(magnet);
    if (want.empty() || name.empty()) return;

    FILE* f = std::fopen(APPDATA_MAGNETS, "rb");
    if (!f) return;
    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    std::fclose(f);

    std::string out;
    bool changed = false;
    size_t i = 0;
    while (i < body.size())
    {
        size_t nl = body.find('\n', i);
        std::string line = body.substr(i, nl == std::string::npos ? nl : nl - i);
        i = nl == std::string::npos ? body.size() : nl + 1;

        size_t a = line.find_first_not_of(" \t\r");
        if (a != std::string::npos && line[a] != '#')
        {
            std::string mg = normalizeMagnet(line);
            if (!mg.empty() && magnetHash(mg) == want &&
                line.find("dn=") == std::string::npos)
            {
                size_t b = line.find_last_not_of(" \t\r");
                std::string base = line.substr(a, b - a + 1);
                std::string enc  = urlEncodeName(name);
                line = base.compare(0, 7, "magnet:") == 0
                           ? base + "&dn=" + enc
                           : "magnet:?xt=urn:btih:" + base + "&dn=" + enc;
                changed = true;
            }
        }
        out += line;
        if (nl != std::string::npos) out += '\n';
    }
    if (!changed) return;
    if (FILE* w = std::fopen(APPDATA_MAGNETS, "wb"))
    {
        std::fwrite(out.data(), 1, out.size(), w);
        std::fclose(w);
        brls::Logger::info("[magnet] saved name '{}' for {}", name, want);
    }
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

// Append a magnet's resolved video files to the cache, keyed by hash. An empty
// list writes a "no video" marker (index -1) so the magnet is still marked
// resolved and the player can say so instead of re-fetching every time. Only
// called once per magnet (when its files were not cached).
void cacheMagnetFiles(const std::string& hash, const std::vector<VideoFile>& vids,
                      int64_t totalLen)
{
    if (hash.empty()) return;
    FILE* f = std::fopen(APPDATA_MAGNET_FILES, "a");
    if (!f) return;
    // Index -2: the whole-torrent size, so the list can show it like a .torrent's
    // without re-fetching metadata. Written alongside the file lines and never on
    // its own -- magnetFilesCached() only looks for the hash, so a lone -2 line
    // would wrongly mark the magnet resolved. magnetCachedFiles() drops every
    // negative index, so it ignores this one for free.
    if (totalLen > 0)
        std::fprintf(f, "%s\t-2\t%lld\t-\n", hash.c_str(), (long long)totalLen);
    if (vids.empty())
    {
        std::fprintf(f, "%s\t-1\t0\t-\n", hash.c_str());
    }
    else
    {
        for (const auto& v : vids)
        {
            // Names never contain a tab or newline in practice; strip any to be safe.
            std::string nm = v.name;
            for (char& c : nm)
                if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            std::fprintf(f, "%s\t%d\t%lld\t%s\n", hash.c_str(), v.index,
                         (long long)v.length, nm.c_str());
        }
    }
    std::fclose(f);
}

// The cached whole-torrent size of a magnet (the -2 line cacheMagnetFiles wrote),
// or 0 when it is not known -- either the magnet is not resolved yet, or it was
// resolved by a build that did not record the total.
int64_t magnetCachedTotal(const std::string& hash)
{
    if (hash.empty()) return 0;
    FILE* f = std::fopen(APPDATA_MAGNET_FILES, "rb");
    if (!f) return 0;
    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    std::fclose(f);

    std::string key = hash + "\t-2\t";
    size_t k        = body.find(key);
    // Must start a line, or a hash that ends with another one's would match.
    while (k != std::string::npos && k != 0 && body[k - 1] != '\n')
        k = body.find(key, k + 1);
    if (k == std::string::npos) return 0;
    return std::atoll(body.c_str() + k + key.size());
}

// The cached video files for a magnet, largest first. Empty if not cached OR if
// the magnet has no video (use magnetFilesCached to tell the two apart).
std::vector<VideoFile> magnetCachedFiles(const std::string& hash)
{
    std::vector<VideoFile> out;
    if (hash.empty()) return out;
    FILE* f = std::fopen(APPDATA_MAGNET_FILES, "rb");
    if (!f) return out;
    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    std::fclose(f);

    size_t i = 0;
    std::string key = hash + "\t";
    while (i < body.size())
    {
        size_t nl = body.find('\n', i);
        std::string line = body.substr(i, nl == std::string::npos ? nl : nl - i);
        i = nl == std::string::npos ? body.size() : nl + 1;
        if (line.compare(0, key.size(), key) != 0) continue;
        // <hash>\t<index>\t<size>\t<name>
        size_t p1 = key.size();
        size_t p2 = line.find('\t', p1);
        size_t p3 = p2 == std::string::npos ? p2 : line.find('\t', p2 + 1);
        if (p2 == std::string::npos || p3 == std::string::npos) continue;
        int index      = std::atoi(line.substr(p1, p2 - p1).c_str());
        int64_t size   = std::atoll(line.substr(p2 + 1, p3 - p2 - 1).c_str());
        std::string nm = line.substr(p3 + 1);
        if (index < 0) continue;  // the "no video" marker
        out.push_back({ index, nm, size });
    }
    std::sort(out.begin(), out.end(), [](const VideoFile& a, const VideoFile& b) {
        return a.length > b.length;
    });
    return out;
}

// Root of the Local tab. Holds the `alive` flag the background name resolver
// checks: cleared when the tab is torn down so late callbacks are no-ops.
class LocalTabRoot : public brls::Box
{
  public:
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    ~LocalTabRoot() override { *alive = false; }
};

// Play `source` (a .torrent path or a magnet URI): no video -> an error; one ->
// play it; several -> ask which file first.
void pickAndPlay(const std::string& source, const std::string& title,
                 const std::vector<VideoFile>& vids)
{
    if (vids.empty())
    {
        auto* d = new brls::Dialog(tr("This torrent has no video file to stream."));
        d->addButton("OK", []() {});
        d->open();
        return;
    }
    if (vids.size() == 1)
    {
        brls::Application::pushActivity(
            new PlayerActivity(source, {}, vids[0].name, vids[0].index));
        return;
    }
    std::vector<Row> rows;
    for (const auto& v : vids)
    {
        Row row;
        row.label  = v.name;
        row.detail = humanSize(v.length);
        rows.push_back(row);
    }
    brls::Application::pushActivity(new ListActivity(
        title, tr("Pick a file"), rows, [source, vids](int i) {
            brls::Logger::info("Playing {} file {} ({})", source, vids[i].index,
                               vids[i].name);
            brls::Application::pushActivity(
                new PlayerActivity(source, {}, vids[i].name, vids[i].index));
        }));
}

// Open an entry. A magnet uses its cached file list (an empty-but-cached list is
// a torrent with no video -> error). A magnet still resolving cannot be played
// yet -- we do not know its files. A .torrent reads its files directly.
void playEntry(const TorrentEntry& e)
{
    if (e.isMagnet)
    {
        std::string h = magnetHash(e.path);
        if (!magnetFilesCached(h))
        {
            auto* d = new brls::Dialog(
                tr("Still fetching this magnet's files -- please wait."));
            d->addButton("OK", []() {});
            d->open();
            return;
        }
        pickAndPlay(e.path, e.name, magnetCachedFiles(h));
        return;
    }
    pickAndPlay(e.path, e.name, torrentVideoFiles(e.path));
}

// The widgets of one built row we may still update: its name label, and (for a
// magnet whose name is loading) its spinner.
struct LocalRow
{
    brls::Box* view               = nullptr;
    brls::Label* name             = nullptr;
    brls::ProgressSpinner* spinner = nullptr;
    brls::Label* size             = nullptr;  // null for a .torrent (fixed at scan)
};

// A list row: icon, name, and either the size or -- for a magnet still resolving
// its name -- a spinner. Clicking it plays the entry.
LocalRow makeLocalRow(const TorrentEntry& e)
{
    auto* row = new brls::Box();
    row->setFocusable(true);
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(64.0f);
    row->setPaddingLeft(24.0f);
    row->setPaddingRight(24.0f);
    row->setCornerRadius(6.0f);

    auto* icon = new brls::Image();
    icon->setImageFromRes(e.isMagnet ? "magnet-icon.png" : "video-icon.png");
    icon->setScalingType(brls::ImageScalingType::FIT);
    icon->setDimensions(38.0f, 38.0f);
    icon->setMarginRight(20.0f);
    row->addView(icon);

    auto* name = new brls::Label();
    name->setFontSize(22.0f);
    name->setGrow(1.0f);
    name->setSingleLine(true);  // truncates, and scrolls the full name on focus
    // Keep the placeholder "Magnet <hash>" name while resolving. Since only one
    // resolves at a time, the queued ones start with a "(waiting)" suffix; the
    // resolver's onStart drops it (and shows the spinner) when its turn comes.
    name->setText(e.needsResolve ? e.name + tr("  (waiting)") : e.name);
    row->addView(name);

    brls::ProgressSpinner* spinner = nullptr;
    if (e.needsResolve)
    {
        spinner = new brls::ProgressSpinner();
        spinner->setDimensions(26.0f, 26.0f);
        spinner->setMarginLeft(16.0f);
        spinner->animate(true);
        spinner->setVisibility(brls::Visibility::GONE);  // shown when its turn comes
        row->addView(spinner);
    }

    // A resolving magnet gets a size label too, not just a spinner: its size is
    // only known once the metadata lands, and the resolver fills this in then (see
    // onEach in buildLocalTab). It starts blank so the row does not jump.
    brls::Label* size = nullptr;
    if (e.needsResolve || !e.sizeText.empty())
    {
        size = new brls::Label();
        size->setFontSize(19.0f);
        size->setTextColor(brls::Application::getTheme().getColor(
            "brls/list/listItem_value_color"));
        size->setMarginLeft(16.0f);
        size->setSingleLine(true);
        size->setShrink(0.0f);
        size->setWidth(86.0f);
        size->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
        size->setText(e.needsResolve ? "" : e.sizeText);
        row->addView(size);
    }

    TorrentEntry cap = e;
    row->registerClickAction([cap](brls::View*) {
        playEntry(cap);
        return true;
    });
    return { row, name, spinner, size };
}

// Remove a magnet line (matched by info-hash) from magnet.txt.
void removeMagnetFromFile(const std::string& magnet)
{
    std::string want = magnetHash(magnet);
    if (want.empty()) return;
    FILE* f = std::fopen(APPDATA_MAGNETS, "rb");
    if (!f) return;
    std::string body;
    char buf[512];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) body.append(buf, n);
    std::fclose(f);

    std::string out;
    size_t i = 0;
    while (i < body.size())
    {
        size_t nl = body.find('\n', i);
        std::string line = body.substr(i, nl == std::string::npos ? nl : nl - i);
        i = nl == std::string::npos ? body.size() : nl + 1;
        bool drop = false;
        size_t a = line.find_first_not_of(" \t\r");
        if (a != std::string::npos && line[a] != '#')
        {
            std::string mg = normalizeMagnet(line);
            if (!mg.empty() && magnetHash(mg) == want) drop = true;
        }
        if (!drop)
        {
            out += line;
            if (nl != std::string::npos) out += '\n';
        }
    }
    if (FILE* w = std::fopen(APPDATA_MAGNETS, "wb"))
    {
        std::fwrite(out.data(), 1, out.size(), w);
        std::fclose(w);
    }
}

// "Add magnet": the on-screen keyboard captures a magnet link or a bare hash.
// On success it is appended to magnet.txt and the normalised URI returned (""
// on cancel / invalid, which also shows a dialog). The caller adds it to the
// list -- it does NOT auto-play.
std::string promptMagnetInput()
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return "";
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, tr("Magnet link or info hash"));
    swkbdConfigSetGuideText(&kbd, tr("magnet:?xt=... or a 40-character hash"));
    swkbdConfigSetStringLenMax(&kbd, 2000);
    char raw[2048] = { 0 };
    Result rc = swkbdShow(&kbd, raw, sizeof(raw));
    swkbdClose(&kbd);
    if (R_FAILED(rc) || !raw[0])
        return "";

    std::string magnet = normalizeMagnet(raw);
    if (magnet.empty())
    {
        auto* d = new brls::Dialog(
            tr("That does not look like a magnet link or an info hash."));
        d->addButton("OK", []() {});
        d->open();
        return "";
    }

    // Persist the trimmed input (readable) so it also lists on the next launch.
    std::string line = raw;
    size_t a = line.find_first_not_of(" \t\r\n");
    size_t b = line.find_last_not_of(" \t\r\n");
    line = a == std::string::npos ? "" : line.substr(a, b - a + 1);
    if (FILE* f = std::fopen(APPDATA_MAGNETS, "a"))
    {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }
    return magnet;
}

// Centered message shown when there are no torrents or magnets yet.
brls::View* buildEmptyState()
{
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setJustifyContent(brls::JustifyContent::CENTER);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setGrow(1.0f);
    box->setPadding(0, 80, 0, 80);

    auto* title = new brls::Label();
    title->setText(tr("No torrents found"));
    title->setFontSize(26);
    title->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    title->setMargins(0, 0, 18, 0);
    box->addView(title);

    auto* hint = new brls::Label();
    hint->setText(tr("Drop .torrent files in this folder on your SD card, or add "
                  "magnet links to magnet.txt inside it (one per line):"));
    hint->setFontSize(20);
    hint->setTextColor(theme::textDim());  // light gray (text_disabled is
                                                // too dark on the dark theme)
    hint->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    hint->setMargins(0, 0, 10, 0);
    box->addView(hint);

    auto* path = new theme::AccentLabel();
    path->setText("/switch/NX-torrent-player/torrents/");
    path->setFontSize(21);
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

// Resolve magnets in the background, ONE AT A TIME (no full download starts):
// (magnet, resolved name, whole-torrent size). Name "" / size 0 mean the attempt
// failed. Not the same thing as the player's setMagnetResolvedHook, which only
// ever needs the name.
using MagnetResolvedFn =
    std::function<void(const std::string&, const std::string&, int64_t)>;

// their name is written into magnet.txt and their video-file list into the files
// cache. `alive` stops the chain once the tab is gone. `onStart(magnet)` fires
// (UI thread) when a magnet's turn begins -- to move the spinner onto it; then
// `onEach` fires when its attempt finishes.
void resolveMagnetNames(std::shared_ptr<bool> alive,
                        std::shared_ptr<std::vector<std::string>> todo, size_t idx,
                        std::function<void(const std::string&)> onStart,
                        MagnetResolvedFn onEach)
{
    if (!alive || !*alive)
        return;
    if (idx >= todo->size())
        return;

    std::string magnet = (*todo)[idx];
    if (onStart)
        onStart(magnet);  // its turn: show the spinner, drop "(waiting)"

    brls::async([alive, todo, idx, magnet, onStart, onEach]() {
        std::string name;
        int64_t total = 0;
        torrent_meta* m = (torrent_meta*)std::calloc(1, sizeof(torrent_meta));
        if (m)
        {
            char err[128] = { 0 };
            if (torrent_load_magnet(m, magnet.c_str(), err, sizeof(err)) == 0)
            {
                name  = m->name;
                total = m->total_len;  // whole-torrent size, like torrentScanInfo
                // Extract + cache its video files (largest first) in the same
                // fetch, so playing it can offer a picker with no extra wait.
                std::vector<VideoFile> vids;
                for (int i = 0; i < m->file_count; i++)
                {
                    std::string p = m->files[i].path;
                    if (!isVideoFile(p)) continue;
                    size_t slash = p.find_last_of("/\\");
                    vids.push_back({ i,
                                     slash == std::string::npos ? p
                                                                : p.substr(slash + 1),
                                     m->files[i].length });
                }
                torrent_unload(m);
                if (!magnetFilesCached(magnetHash(magnet)))
                    cacheMagnetFiles(magnetHash(magnet), vids, total);
            }
            std::free(m);
        }
        brls::sync([alive, todo, idx, magnet, name, total, onStart, onEach]() {
            if (!*alive)
                return;
            if (!name.empty())
                updateMagnetName(magnet, name);
            if (onEach)
                onEach(magnet, name, total);
            resolveMagnetNames(alive, todo, idx + 1, onStart, onEach);
        });
    });
}

// hash -> (name label, spinner) for magnet rows still resolving their name.
struct MagnetRowWidgets
{
    brls::Label* name              = nullptr;
    brls::ProgressSpinner* spinner = nullptr;
    brls::Label* size              = nullptr;
};
using MagnetWidgets = std::map<std::string, MagnetRowWidgets>;

// Delete a row's backing (the .torrent file, or the magnet's magnet.txt line),
// move focus to a neighbour first, then drop the row from the list.
void deleteEntryRow(brls::Box* list, brls::Box* row, const TorrentEntry& e,
                    std::shared_ptr<MagnetWidgets> widgets)
{
    auto& kids = list->getChildren();
    int idx    = -1;
    for (size_t i = 0; i < kids.size(); i++)
        if (kids[i] == row) { idx = (int)i; break; }
    brls::View* neighbour = nullptr;
    if (idx >= 0)
    {
        if (idx + 1 < (int)kids.size()) neighbour = kids[idx + 1];
        else if (idx - 1 >= 0)          neighbour = kids[idx - 1];
    }
    if (neighbour) brls::Application::giveFocus(neighbour);

    if (e.isMagnet)
    {
        removeMagnetFromFile(e.path);
        if (widgets) widgets->erase(magnetHash(e.path));  // also cancels its name fill
    }
    else
    {
        std::remove(e.path.c_str());
    }
    list->removeView(row);  // frees it
}

// Y on a row: confirm, then delete it (removes the magnet, or the .torrent file).
void registerRowDelete(brls::Box* list, brls::Box* row, TorrentEntry e,
                       std::shared_ptr<MagnetWidgets> widgets)
{
    row->registerAction(
        tr("Delete"), brls::BUTTON_Y,
        [list, row, e, widgets](brls::View*) {
            std::string msg = e.isMagnet
                                  ? tr("Remove this magnet from the list?")
                                  : tr("Delete this .torrent file from the SD card?");
            auto* d = new brls::Dialog(msg + "\n\n" + e.name);
            d->addButton(tr("Cancel"), []() {});
            d->addButton(tr("Delete"), [list, row, e, widgets]() {
                deleteEntryRow(list, row, e, widgets);
            });
            d->open();
            return true;
        },
        false, false, brls::SOUND_NONE);
}

// The local list -- .torrent files AND magnet.txt entries, in one section.
// Magnets still resolving their name are shown first, with a spinner, and update
// in place once the name comes in (no relaunch needed).
brls::View* buildLocalTab()
{
    auto items   = scanTorrents(APPDATA_TORRENTS "/");
    auto magnets = scanMagnets(APPDATA_MAGNETS);
    items.insert(items.end(), magnets.begin(), magnets.end());
    std::sort(items.begin(), items.end(),
              [](const TorrentEntry& a, const TorrentEntry& b) {
                  if (a.needsResolve != b.needsResolve) return a.needsResolve;  // loading first
                  return a.name < b.name;
              });

    auto* root = new LocalTabRoot();
    root->setAxis(brls::Axis::COLUMN);
    root->setGrow(1.0f);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* list = new brls::Box();
    list->setAxis(brls::Axis::COLUMN);
    // setContentView forces the width and drops margins, so the inset is padding.
    list->setPadding(24.0f, 60.0f, 47.0f, 60.0f);
    scroll->setContentView(list);
    root->addView(scroll);

    auto widgets = std::make_shared<MagnetWidgets>();
    auto alive   = root->alive;

    // Fills a loading row in as its name and size resolve (shared by the initial
    // list and the "Add magnet" button). Its spinner goes away either way.
    MagnetResolvedFn onEach = [widgets](const std::string& magnet,
                                        const std::string& name, int64_t total) {
        auto it = widgets->find(magnetHash(magnet));
        if (it == widgets->end()) return;
        if (it->second.spinner)
            it->second.spinner->setVisibility(brls::Visibility::GONE);
        if (it->second.name && !name.empty())
            it->second.name->setText(name);
        // Blank on failure: the row keeps an empty size slot rather than claiming
        // a wrong one, and the next launch will try again.
        if (it->second.size)
            it->second.size->setText(humanSize(total));
    };

    // A magnet's turn begins: drop its "(waiting)" suffix and show its spinner.
    std::function<void(const std::string&)> onStart =
        [widgets](const std::string& magnet) {
            auto it = widgets->find(magnetHash(magnet));
            if (it == widgets->end()) return;
            if (it->second.name)
                it->second.name->setText(magnetName(magnet));
            if (it->second.spinner)
                it->second.spinner->setVisibility(brls::Visibility::VISIBLE);
        };

    // "Add magnet" at the top: adds it to the list (and magnet.txt) via the
    // on-screen keyboard, and resolves its name in the background. Does not play.
    auto* addBtn = new brls::Button();
    addBtn->setText(tr("+  Add magnet"));
    addBtn->setMarginBottom(14.0f);
    addBtn->registerClickAction([list, widgets, alive, onStart, onEach](brls::View*) {
        std::string magnet = promptMagnetInput();
        if (magnet.empty()) return true;
        TorrentEntry e;
        e.isMagnet  = true;
        e.path      = magnet;
        e.name        = magnetName(magnet);
        e.sizeText    = "magnet";
        e.needsResolve = true;  // freshly added -> its files are not cached yet
        LocalRow w  = makeLocalRow(e);
        registerRowDelete(list, w.view, e, widgets);
        list->addView(w.view, 1);  // just under the Add button
        brls::Application::giveFocus(w.view);
        if (e.needsResolve)
        {
            (*widgets)[magnetHash(magnet)] = { w.name, w.spinner, w.size };
            auto one = std::make_shared<std::vector<std::string>>();
            one->push_back(magnet);
            resolveMagnetNames(alive, one, 0, onStart, onEach);
        }
        return true;
    });
    list->addView(addBtn);

    if (items.empty())
    {
        list->addView(buildEmptyState());
        return root;
    }

    auto todo = std::make_shared<std::vector<std::string>>();
    for (const auto& e : items)
    {
        LocalRow w = makeLocalRow(e);
        registerRowDelete(list, w.view, e, widgets);
        list->addView(w.view);
        if (e.needsResolve)
        {
            (*widgets)[magnetHash(e.path)] = { w.name, w.spinner, w.size };
            todo->push_back(e.path);
        }
    }
    if (!todo->empty())
        resolveMagnetNames(alive, todo, 0, onStart, onEach);
    return root;
}

// Browser frame background for the Stremio tab: diagonal gradient from
// top-right (#1a173e) to bottom-left (#000000), painted behind header/content/footer.
class BrowserFrame : public brls::AppletFrame
{
  public:
    // On teardown -- app exit above all -- the header views this frame owns and
    // the live StremioTab beneath it are freed in an order we do not control.
    // Several global sinks bridge those two subtrees: viewTabSink captures the
    // header's view bar and its buttons, libraryUpTarget a header button, the
    // cyclers/selector the tab. As it is destroyed ~StremioTab calls
    // reportView(-1), which routes through viewTabSink into what may already be a
    // freed header view -- the crash on quitting from the signed-in Stremio tab.
    // A subclass destructor body runs BEFORE the base Box destroys our children,
    // so clearing every bridge here makes those late calls harmless no-ops.
    ~BrowserFrame() override
    {
        // Same reasoning for the repaint hook: it captures the header's two bars.
        theme::setRepaintHook(nullptr);
        stremio::setViewTabSink(nullptr);
        stremio::setViewSelector(nullptr);
        stremio::setViewCycler(nullptr);
        stremio::setLibraryUpTarget(nullptr);
        stremio::setLibraryCountSink(nullptr);
    }

    // Both the background gradient and the header mark are Stremio-tab-only.
    void setOnStremio(bool on) { this->onStremio = on; }

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override
    {
        if (this->onStremio)
        {
            NVGpaint gradient =
                nvgLinearGradient(vg, x + width, y, x, y + height,
                                  theme::gradTop(), theme::gradBottom());
            nvgBeginPath(vg);
            nvgRect(vg, x, y, width, height);
            nvgFillPaint(vg, gradient);
            nvgFill(vg);
        }
        brls::AppletFrame::draw(vg, x, y, width, height, style, ctx);

        // The Stremio mark, drawn on top of the header's icon slot rather than
        // loaded as a texture, so it follows the accent colour. applyTabIdentity
        // leaves that Image INVISIBLE (not GONE) on this tab: it still reserves
        // the same box, which is where we paint. AFTER the base draw, since the
        // header is one of its children -- and safe to batch normally, unlike the
        // player, because nothing here renders outside nanovg.
        if (this->onStremio)
        {
            if (!this->iconSlot)
                this->iconSlot = this->getView("brls/applet_frame/title_icon");
            if (this->iconSlot)
                theme::drawStremioMark(vg, this->iconSlot->getX(),
                                       this->iconSlot->getY(),
                                       this->iconSlot->getWidth());
        }
    }

  private:
    bool onStremio = false;
    // Resolved once: getView() walks the tree by id, which has no business
    // running every frame.
    brls::View* iconSlot = nullptr;
};

// Header identity on every tab switch.
void applyTabIdentity(brls::AppletFrame* frame, config::Tab tab)
{
    bool stremio = tab == config::Tab::STREMIO;

    if (auto* browserFrame = dynamic_cast<BrowserFrame*>(frame))
        browserFrame->setOnStremio(stremio);

    frame->setTitle("");
    // Local keeps its white PNG. Stremio's mark is drawn by BrowserFrame instead,
    // so no texture is loaded for it at all -- romfs:/stremio-icon.png is a fixed
    // violet that cannot follow the accent.
    if (!stremio) frame->setIcon("romfs:/local-icon.png");

    // The header icon is sized "auto", i.e. from the texture, and these two are
    // not the same pixel size -- pin both to the same box.
    if (auto* ic = dynamic_cast<brls::Image*>(
            frame->getView("brls/applet_frame/title_icon")))
    {
        ic->setDimensions(54.0f, 54.0f);
        ic->setScalingType(brls::ImageScalingType::FIT);
        // INVISIBLE, not GONE: it must still reserve its slot in the header row,
        // because that box is exactly where the mark gets painted.
        ic->setVisibility(stremio ? brls::Visibility::INVISIBLE
                                  : brls::Visibility::VISIBLE);
    }
}

brls::View* buildTab(config::Tab tab)
{
    return tab == config::Tab::STREMIO ? (brls::View*)new StremioTab()
                                       : buildLocalTab();
}

// A Label that renders with the Material icon font as its PRIMARY face. Reached
// through the regular font's fallback, a Material glyph is positioned on the
// text baseline and sits visibly too high; as the primary face, nanovg centres
// it with the icon font's own metrics, level with the text beside it.
class IconLabel : public brls::Label
{
  public:
    IconLabel()
    {
        int f = brls::Application::getFont(brls::FONT_MATERIAL_ICONS);
        if (f >= 0) this->font = f;
    }
};

// Categories as buttons in the header, on the left next to the icon.
// Hand-built: borealis' TabFrame is sidebar-only.
void attachTopTabBar(brls::AppletFrame* frame, brls::Box* content)
{
    // The accent drives the active tab (BUTTONSTYLE_PRIMARY reads brls/accent)
    // and the focus highlight; main() has already applied it, but the tab bar is
    // also rebuilt on a colour change, and re-applying costs nothing.
    theme::applyAccent();

    auto* bar = new brls::Box();
    bar->setAxis(brls::Axis::ROW);
    bar->setAlignItems(brls::AlignItems::CENTER);

    // Rebuilt on every switch: TabFrame frees the old tab before making the new
    // one, and so must we -- two live StremioTabs would each hold their own
    // library and in-flight requests.
    auto* localBtn   = new brls::Button();
    auto* stremioBtn = new brls::Button();

    // Which tab is up, held in a shared cell rather than a capture so the
    // repaint hook below can restyle without knowing how we got here.
    auto curTab = std::make_shared<config::Tab>(config::get().startupTab);

    // The style carries the accent: a Button copies it out of the theme when a
    // style is applied and keeps it, so this has to be re-run (not the tab
    // rebuilt) when the colour changes.
    auto restyleTabs = [localBtn, stremioBtn, curTab]() {
        localBtn->setStyle(*curTab == config::Tab::LOCAL
                               ? &brls::BUTTONSTYLE_PRIMARY
                               : &brls::BUTTONSTYLE_BORDERLESS);
        stremioBtn->setStyle(*curTab == config::Tab::STREMIO
                                 ? &brls::BUTTONSTYLE_PRIMARY
                                 : &brls::BUTTONSTYLE_BORDERLESS);
    };

    auto select = [content, localBtn, stremioBtn, frame, curTab,
                   restyleTabs](config::Tab tab) {
        *curTab = tab;
        restyleTabs();
        // L and R only cycle the Stremio views, so the "View" chip has no
        // business in the footer on the Local tab.
        bool stremioUp = tab == config::Tab::STREMIO;
        frame->setActionAvailable(brls::BUTTON_LB, stremioUp);
        frame->setActionAvailable(brls::BUTTON_RB, stremioUp);
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
            tr("Back"), brls::BUTTON_B,
            [active](brls::View*) {
                brls::Application::giveFocus(active);
                return true;
            },
            false, false, brls::SOUND_BACK);
    };

    stremioBtn->setText(tr("Stremio"));
    stremioBtn->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    stremioBtn->setMarginLeft(8.0f);
    stremioBtn->registerClickAction([select](brls::View*) {
        select(config::Tab::STREMIO);
        return true;
    });
    bar->addView(stremioBtn);


    localBtn->setText(tr("Local"));
    localBtn->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
    localBtn->registerClickAction([select](brls::View*) {
        select(config::Tab::LOCAL);
        return true;
    });
    bar->addView(localBtn);

    // The Stremio sub-view switcher, centred in the header: one button per view
    // (Continue / Movies / Shows / Library). It says where you are and clicking
    // one jumps straight there, alongside the L/R cycle. The live StremioTab
    // drives it -- setViewTabSink highlights the active view, or hides the whole
    // bar (index -1) on the Local tab and the sign-in screen.
    auto* viewBar = new brls::Box();
    viewBar->setAxis(brls::Axis::ROW);
    viewBar->setAlignItems(brls::AlignItems::CENTER);
    viewBar->setJustifyContent(brls::JustifyContent::CENTER);

    // The right-hand cluster: Search (the fifth view, pulled out of the group
    // and kept as its own icon), the account button and Options.
    auto* rightBar = new brls::Box();
    rightBar->setAxis(brls::Axis::ROW);
    rightBar->setAlignItems(brls::AlignItems::CENTER);

    std::vector<brls::Button*> viewBtns;
    std::vector<brls::Label*>  viewIcons;
    const float btnFont    = brls::Application::getStyle()["brls/button/text_size"];

    // A Label measured by its own font makes its button a different size: the
    // vertical bounds nanovg reports are the FONT's line height, and Material
    // Icons' is far taller than the text face's, so an icon button came out
    // taller than "Stremio" / "Local" beside it -- and wider, since the same
    // measure includes the glyph's side bearings. Pinning the icon to a box of
    // its own settles both: a height under the text label's leaves the button
    // sized by the text exactly as a plain one, and an exact width drops the
    // bearings. The glyph is then drawn centred in that box, whatever its size
    // -- which is what lets the bare icons below be bigger than the text.
    // withText: the glyph has to sit level with a text label beside it, which is
    // not the same thing as sitting in the middle of the button. "Middle"
    // anchors the ink halfway between the font's OWN ascender and descender, and
    // the text face puts its ink higher above that point than the icon face
    // does -- so an icon centred like its neighbour reads as low. Opposite
    // margins lift it by that difference without changing the box's outer height
    // (and so the button's). A bare icon has nothing to line up with and simply
    // stays centred, where its cursor and background are.
    auto sizeIcon = [btnFont](brls::Label* ic, float size, bool withText) {
        ic->setFontSize(size);
        ic->setWidth(size);
        ic->setHeight(btnFont);
        ic->setHorizontalAlign(brls::HorizontalAlign::CENTER);
        ic->setVerticalAlign(brls::VerticalAlign::CENTER);
        float nudge = withText ? size / 9.0f : 0.0f;
        ic->setMargins(-nudge, 0.0f, nudge, 0.0f);
    };

    // Standing alone, an icon reads as small at the buttons' text size -- so the
    // ones with no label beside them (Search, account, Options) are drawn half
    // again as big. Only the glyph grows: their box is the same height, so the
    // buttons stay level with the rest of the header.
    const float bareIcon = btnFont * 1.5f;

    const auto& viewLabels = stremio::viewLabels();
    // Search is last in the view cycle (and so in the labels, which are
    // index-matched to it). It is the one view shown as a bare icon.
    const size_t searchIdx = viewLabels.size() - 1;
    for (size_t i = 0; i < viewLabels.size(); i++)
    {
        // Each label is "<material-glyph> <text>". Rendered as one Button string
        // the glyph shares the text's baseline and sits visibly too high. Split
        // it out into its own Label: the Button is a row with alignItems=center,
        // so a separate icon Label is centred against the text instead.
        const std::string& full = viewLabels[i];
        size_t      sp    = full.find(' ');
        std::string glyph = sp == std::string::npos ? std::string() : full.substr(0, sp);
        std::string text  = sp == std::string::npos ? full : full.substr(sp + 1);
        bool iconOnly     = i == searchIdx;

        auto* b = new brls::Button();
        if (!iconOnly) b->setText(text);
        b->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
        if (i && !iconOnly) b->setMarginLeft(2.0f);

        auto* ic = new IconLabel();
        ic->setText(glyph);
        sizeIcon(ic, iconOnly ? bareIcon : btnFont, !iconOnly);
        if (!iconOnly) ic->setMarginRight(6.0f);
        b->addView(ic, 0);   // before the text label
        viewIcons.push_back(ic);

        int idx = (int)i;
        b->registerClickAction([idx](brls::View*) {
            stremio::selectActiveView(idx);
            return true;
        });
        (iconOnly ? rightBar : viewBar)->addView(b);
        viewBtns.push_back(b);
    }

    // An icon-only borderless button, built like the ones above: an IconLabel
    // in front of the Button's own (empty) label, which leaves the style's side
    // padding around the glyph.
    auto makeIconButton = [bareIcon, sizeIcon](const char* glyph) {
        auto* b = new brls::Button();
        b->setStyle(&brls::BUTTONSTYLE_BORDERLESS);
        auto* ic = new IconLabel();
        ic->setText(glyph);
        sizeIcon(ic, bareIcon, false);
        // A bare Label defaults to the app's text colour, not the button's --
        // which is a different value in the light variant.
        ic->setTextColor(
            brls::Application::getTheme()["brls/button/default_enabled_text"]);
        b->addView(ic, 0);
        return b;
    };

    // Material "account_circle": who is signed in to Stremio, and the way out.
    auto* profileBtn = makeIconButton("");
    profileBtn->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new AccountActivity());
        return true;
    });
    rightBar->addView(profileBtn);

    // Material "settings". The only header button that belongs to BOTH tabs --
    // it is X in the footer as well, but that hint is easy to miss.
    auto* settingsBtn = makeIconButton("");
    settingsBtn->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new SettingsActivity());
        return true;
    });
    rightBar->addView(settingsBtn);

    // active >= 0: show the bar and mark that view PRIMARY. active < 0: fold it
    // away and take its buttons out of the focus ring (a GONE box keeps its
    // children focusable otherwise -- the same trap as the sign-in form).
    // LEFT out of the view bar must land on "Local", not "Stremio". The category
    // bar reads "Stremio | Local", so coming from the right of the screen the
    // nearest button is Local -- but Box::getDefaultFocus() hands back whichever
    // of the two was focused last (box.cpp:357), and that is normally the active
    // tab's own button, i.e. Stremio. Only the leftmost view button can leave the
    // bar this way, so one explicit route is enough to beat that heuristic.
    if (!viewBtns.empty())
        viewBtns[0]->setCustomNavigationRoute(brls::FocusDirection::LEFT, localBtn);
    // ... and the two bars are separate absolute boxes now, so the step between
    // the group and the detached Search icon needs saying too.
    viewBtns[searchIdx - 1]->setCustomNavigationRoute(brls::FocusDirection::RIGHT,
                                                     viewBtns[searchIdx]);
    viewBtns[searchIdx]->setCustomNavigationRoute(brls::FocusDirection::LEFT,
                                                  viewBtns[searchIdx - 1]);

    // Where the centred group starts. At the 100% UI size the header is at its
    // most crowded and the group comes up against "Local"; inset it there so it
    // centres in the space beside the category bar instead. Every smaller UI has
    // room to spare, and true centring reads better, so they keep it. The size
    // is stored per mode, hence the docked check -- and hence recomputing this
    // when the console is docked or undocked.
    auto insetViewBar = [viewBar]() {
        bool docked = isDocked();
        int  w      = docked ? config::get().dockedUiWidth
                             : config::get().handheldUiWidth;
        viewBar->setPositionLeft(w <= config::kDefaultHandheldUiWidth ? 120.0f
                                                                     : 0.0f);
    };
    brls::Application::getWindowSizeChangedEvent()->subscribe(insetViewBar);
    // Docking fires the event above; editing the UI size in Options does not
    // (applyUiScale resizes the logical space itself), hence the second route.
    setUiScaleHook(insetViewBar);

    auto curView      = std::make_shared<int>(-1);
    auto applyViewBar = [viewBar, viewBtns, viewIcons, stremioBtn, localBtn,
                         profileBtn, settingsBtn, searchIdx, curView,
                         insetViewBar](int active) {
        *curView  = active;
        bool show = active >= 0;
        // Also here, not only on the window event: changing the UI size in
        // Options resizes the logical space without firing that event (see
        // applyUiScale), and this runs on the way back into a view.
        insetViewBar();
        viewBar->setVisibility(show ? brls::Visibility::VISIBLE
                                    : brls::Visibility::GONE);
        // The account button is Stremio's, so it folds away with the bar --
        // Options does not, it belongs to both tabs and stays.
        profileBtn->setVisibility(show ? brls::Visibility::VISIBLE
                                       : brls::Visibility::GONE);
        profileBtn->setFocusable(show);
        brls::Theme theme = brls::Application::getTheme();
        for (size_t i = 0; i < viewBtns.size(); i++)
        {
            bool isActive = (int)i == active;
            viewBtns[i]->setFocusable(show);
            viewBtns[i]->setStyle(isActive ? &brls::BUTTONSTYLE_PRIMARY
                                           : &brls::BUTTONSTYLE_BORDERLESS);
            // Search sits outside the bar that gets hidden, so it has to be
            // folded away by itself.
            if (i == searchIdx)
                viewBtns[i]->setVisibility(show ? brls::Visibility::VISIBLE
                                                : brls::Visibility::GONE);
            // The icon lives in its own Label, so it does not inherit the button's
            // per-state text colour on its own -- keep it in step here.
            viewIcons[i]->setTextColor(
                theme[isActive ? "brls/button/primary_enabled_text"
                               : "brls/button/default_enabled_text"]);
        }
        // What is left of the header depends on the tab, and a custom route to a
        // hidden view is a dead end (application.cpp only follows one to a
        // VISIBLE view, and does not fall back) -- so re-aim these two here.
        // Local: the only thing right of the category bar is Options.
        localBtn->setCustomNavigationRoute(brls::FocusDirection::RIGHT,
                                           show ? (brls::View*)viewBtns[0]
                                                : (brls::View*)settingsBtn);
        settingsBtn->setCustomNavigationRoute(brls::FocusDirection::LEFT,
                                              show ? (brls::View*)profileBtn
                                                   : (brls::View*)localBtn);
        // Up from the Stremio list lands on the ACTIVE view button (Continue /
        // Movies / ...), not the "Stremio" category tab. reportView runs before
        // the list is (re)built, so finishList picks this up. Falls back to the
        // category button when no view is shown.
        stremio::setLibraryUpTarget(show && active < (int)viewBtns.size()
                                        ? (brls::View*)viewBtns[active]
                                        : (brls::View*)stremioBtn);
    };
    applyViewBar(-1);
    stremio::setViewTabSink(applyViewBar);

    // Hard-anchor the three bars in the header: the category switcher top-left,
    // next to the icon; the Stremio view switcher centred; Search / account /
    // Options top-right.
    if (auto* header = frame->getHeader())
    {
        // Left AND right pinned: that is what gives an absolute box a width to
        // centre its buttons in -- there is no "centre me" for one. So this box
        // spans the whole header, and it goes in FIRST on purpose: Box::hitTest
        // walks its children in reverse and a box that contains the point
        // returns ITSELF when no child matches, so a full-width box added last
        // swallowed every touch in the header -- the category buttons under it
        // stopped responding to the touchscreen. Tested last, it only catches
        // the taps nothing else wanted. (insetViewBar owns its left edge.)
        viewBar->setPositionType(brls::PositionType::ABSOLUTE);
        viewBar->setPositionRight(0.0f);
        viewBar->setPositionTop(20.0f);
        header->addView(viewBar);

        bar->setPositionType(brls::PositionType::ABSOLUTE);
        bar->setPositionLeft(105.0f);
        bar->setPositionTop(20.0f);
        header->addView(bar);

        rightBar->setPositionType(brls::PositionType::ABSOLUTE);
        rightBar->setPositionRight(30.0f);
        rightBar->setPositionTop(20.0f);
        header->addView(rightBar);
    }

    // The library list cannot walk focus back out to the header on its own; hand
    // it the button to jump to (see stremio::setLibraryUpTarget).
    stremio::setLibraryUpTarget(stremioBtn);

    select(config::get().startupTab);

    // Both header bars hold their colours until restyled, so an accent change in
    // Options would otherwise only show up on the next tab switch. Restyle in
    // place -- rebuilding the tab from here would free views that are focused, or
    // on the focus stack while Options sits on top of us.
    theme::setRepaintHook([restyleTabs, applyViewBar, curView]() {
        restyleTabs();
        applyViewBar(*curView);
    });
}

brls::View* buildBrowser()
{
    auto* frame = new BrowserFrame();
    // Our content box is a plain Box with no header of its own, so it goes
    // inside the AppletFrame that carries the title/icon.
    //
    // IMPORTANT: everything touching the header has to happen AFTER
    // pushContentView -- it calls updateAppletFrameItem(), which empties
    // hint_box and resets title/icon to the content view's (blank) ones.
    auto* content = new brls::Box();
    content->setAxis(brls::Axis::COLUMN);
    content->setGrow(1.0f);
    frame->pushContentView(content);
    attachTopTabBar(frame, content);

    // Header text/category chip is disabled; only the left-side tab switcher
    // stays in the header.
    stremio::setLibraryCountSink([](const std::string&) {});

    // No X shortcut for Options: the header carries a gear now, and a chip in
    // the footer for something already on screen is noise.

    // R/L cycle the Stremio view. On the frame (not the tab) so they work with
    // focus on the header tab bar too; a no-op when the Stremio tab is not live.
    // One chip "L R  View": L is the auto icon, R is a glyph in the hint text.
    // (The text glyph renders a touch smaller than the icon -- a borealis hint
    // quirk -- but this keeps them side by side; the R action is hidden so it
    // does not add a second chip.)
    frame->registerAction(std::string("  ") + tr("View"), brls::BUTTON_LB, [](brls::View*) {
        stremio::cycleActiveView(-1);
        return true;
    }, false, false, brls::SOUND_NONE);
    frame->registerAction(tr("View"), brls::BUTTON_RB, [](brls::View*) {
        stremio::cycleActiveView(+1);
        return true;
    }, true, false, brls::SOUND_NONE);

    // select() keeps these in step from here on, but its first call ran inside
    // attachTopTabBar -- i.e. before the actions existed, where setActionAvailable
    // finds nothing to set. Seed them for the startup tab.
    bool stremioUp = config::get().startupTab == config::Tab::STREMIO;
    frame->setActionAvailable(brls::BUTTON_LB, stremioUp);
    frame->setActionAvailable(brls::BUTTON_RB, stremioUp);

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
    // Straight after the config it reads, and before anything can build a view:
    // tr() is resolved when a string is handed to a view, and the header, the
    // tab bar and the browser are built once and never again.
    i18n::load();
    // The engine default already matches the config default, but the config
    // may say otherwise: hand it over before any torrentfs can be opened.
    torrentfs_set_governor(config::get().rateGovernor ? 1 : 0);
    // argv[0] tells us which .nro to replace; nothing else can.
    update::init(argc, argv);

    // Fill in a magnet's name in magnet.txt once its metadata resolves on device.
    setMagnetResolvedHook(updateMagnetName);

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
    // Order matters: SwitchPlatform caches the console's ColorSetId at
    // construction and setThemeVariant() overwrites that same field, so the
    // console's own preference has to be read before applyVariant() runs -- it is
    // unrecoverable afterwards. ("Follow the console" depends on this.)
    theme::captureSystemVariant();
    theme::applyVariant();

    brls::Application::setGlobalQuit(true);

    // Before anything is built: plenty of views read the accent and the palette
    // once, as they are constructed, and keep the colours they got.
    theme::applyAccent();

    // The window was sized inside createWindow, before any activity exists, so
    // fix the logical size now -- the first layout then happens at the user's
    // scale instead of being redone. (config::load() has already run.)
    applyUiScale();
    // Docking/undocking mid-session swaps the framebuffer between 720p and
    // 1080p, and the two modes have their own UI size; follow it.
    brls::Application::getWindowSizeChangedEvent()->subscribe(applyUiScale);

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
