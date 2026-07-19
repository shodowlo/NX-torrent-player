#include "http.hpp"

#include <curl/curl.h>

#include <cstdio>

#include <borealis.hpp>

namespace http
{

static size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    ((std::string*)userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

// Shared POST-JSON helper. Returns the body, or sets err.
bool postJson(const char* url, const std::string& body, std::string& resp,
              std::string& err)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        err = "cannot initialise the network";
        return false;
    }

    char errbuf[CURL_ERROR_SIZE] = { 0 };
    struct curl_slist* hdrs      = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    // GitHub answers 403 to a request with no User-Agent.
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NX-torrent-player");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // The Switch has no CA bundle mounted, so libcurl cannot verify the chain.
    // Explicit rather than silently inherited -- a password goes through here.
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
    {
        err = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        return false;
    }
    return true;
}

// Addon resources are plain GETs off the addon's own host. `accept` is opt-in:
// image hosts content-negotiate, and with no Accept header at all they are free
// to answer with WebP -- which stb_image (nanovg's decoder) cannot read, so the
// poster silently never appeared.
bool get(const std::string& url, std::string& resp, std::string& err,
         const char* accept)
{
    CURL* curl = curl_easy_init();
    if (!curl)
    {
        err = "cannot initialise the network";
        return false;
    }
    char errbuf[CURL_ERROR_SIZE] = { 0 };
    struct curl_slist* hdrs      = nullptr;
    if (accept)
    {
        hdrs = curl_slist_append(hdrs, accept);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    // GitHub answers 403 to a request with no User-Agent.
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NX-torrent-player");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    CURLcode rc = curl_easy_perform(curl);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK)
    {
        err = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        return false;
    }
    return true;
}

// Percent-encodes an id for use in a URL path. Episode ids carry ':' separators
// ("tt123:1:3") which some addon hosts reject unencoded.
// Streams the body straight to disk: an .nro is tens of MB, and holding that in
// RAM next to a running mpv is asking for trouble.
struct DlCtx
{
    FILE* f = nullptr;
    std::function<bool(int64_t, int64_t)> progress;
    bool aborted = false;
};

static size_t fileWriteCb(char* ptr, size_t size, size_t nmemb, void* userdata)
{
    auto* c = (DlCtx*)userdata;
    return std::fwrite(ptr, size, nmemb, c->f);
}

static int progressCb(void* userdata, curl_off_t total, curl_off_t now,
                      curl_off_t, curl_off_t)
{
    auto* c = (DlCtx*)userdata;
    if (!c->progress) return 0;
    if (!c->progress((int64_t)now, (int64_t)total))
    {
        c->aborted = true;
        return 1;  // non-zero aborts the transfer
    }
    return 0;
}

bool download(const std::string& url, const std::string& path, std::string& err,
              std::function<bool(int64_t, int64_t)> progress)
{
    DlCtx ctx;
    ctx.progress = std::move(progress);
    ctx.f        = std::fopen(path.c_str(), "wb");
    if (!ctx.f)
    {
        err = "cannot write " + path;
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        std::fclose(ctx.f);
        std::remove(path.c_str());
        err = "cannot initialise the network";
        return false;
    }

    char errbuf[CURL_ERROR_SIZE] = { 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    // GitHub answers 403 to a request with no User-Agent.
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "NX-torrent-player");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fileWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    // No CURLOPT_TIMEOUT here, unlike the other calls: this transfer is tens of
    // MB over hotel wifi. LOW_SPEED_* kills it when it actually stalls instead.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);  // GitHub redirects to a CDN
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

    CURLcode rc = curl_easy_perform(curl);
    long code   = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    std::fclose(ctx.f);

    if (rc != CURLE_OK)
    {
        std::remove(path.c_str());
        err = ctx.aborted ? "cancelled"
                          : (errbuf[0] ? errbuf : curl_easy_strerror(rc));
        return false;
    }
    // A 404 is a perfectly successful transfer of an error page, and would land
    // on disk as a "download" -- check what we were actually served.
    if (code >= 400)
    {
        std::remove(path.c_str());
        err = "HTTP " + std::to_string(code);
        return false;
    }
    return true;
}

std::string urlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

} // namespace http
