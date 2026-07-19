#pragma once

#include <functional>
#include <string>

// The app's HTTP calls. Blocking: every caller here already runs them off the
// UI thread (brls::async) and syncs the result back.
//
// TLS certificates are NOT verified: the Switch ships no CA bundle, so there is
// nothing to check a chain against. Anything sensitive crossing these calls (the
// Stremio sign-in) is exposed to a MITM on the same network. Fixing this means
// bundling a CA store in the romfs -- see the README's known limitations.
namespace http
{

// GET `url` into `resp`. False on failure, with a readable reason in `err`.
// `accept` is an optional full header line ("Accept: image/jpeg").
bool get(const std::string& url, std::string& resp, std::string& err,
         const char* accept = nullptr);

// POST `body` as application/json.
bool postJson(const char* url, const std::string& body, std::string& resp,
              std::string& err);

// GET `url` straight to `path`, so a large file never sits in RAM. `progress`
// is called from the transfer thread with (bytes so far, total or 0 if the
// server did not say); returning false from it aborts the download. The file is
// removed on failure, so a half-written one is never left behind.
bool download(const std::string& url, const std::string& path, std::string& err,
              std::function<bool(int64_t, int64_t)> progress = nullptr);

// Percent-encodes a URL path segment.
std::string urlEncode(const std::string& s);

} // namespace http
