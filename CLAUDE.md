# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

NX Torrent Player — Nintendo Switch homebrew that *streams* torrents: an
on-device BitTorrent engine feeds mpv through a custom devoptab, so playback
starts on the first pieces instead of waiting for a download. The UI is borealis
(Horizon style), and a Stremio account can be signed in to browse its library,
addons and sources.

## Build

devkitPro is not installed locally: build through the Docker image, with the
build tree kept in a named volume (`st_build`) so it survives between runs.

```sh
docker run --rm -v st_build:/build -v "$PWD":/project -w /build \
    devkitpro/devkita64:latest \
    sh -c "cmake --build . --target NX-torrent-player.nro"

# copy the result out of the volume
docker run --rm -v st_build:/build -v "$PWD":/project \
    devkitpro/devkita64:latest sh -c "cp /build/NX-torrent-player.nro /project/"
```

Adding or removing a source file needs a configure pass (the sources are
globbed): `cmake .` in the same container, before the build. A first build in a
fresh volume needs `cmake /project` instead.

Produces `NX-torrent-player.nro`, which runs from the Homebrew Menu on a CFW
Switch. On Windows, prefix docker commands with `MSYS_NO_PATHCONV=1` or Git Bash
rewrites the container paths.

## Architecture

- `app/` — the UI (C++, borealis): `main` (browser/tabs), `player` (mpv view +
  loading screen + seek), `stremio` (API, library, artwork), `browse` (the
  Stremio drill-down), `settings`, `config` (config.json)
- `engine/` — the torrent engine (C): `torrentfs` (piece picking, cache, the
  streaming read), `peer`, `swarm`, `torrent`, `udp_tracker`, `dht`, `magnet`,
  `stream` (the devoptab mpv reads through), plus vendored libutp (`utp*.cpp`)
- `include/` — libutp headers, paired with `engine/utp*.cpp`
- `assets/` — app icon, logos, list icons (copied into the nro's romfs)
- `library/borealis/` — vendored UI framework, locally patched: see its
  `VENDORED.md` (upstream commit + the ENOBUFS patch, re-apply if re-cloned)
- `pctest/` — builds the engine for a PC, without the Switch or the UI

The entry point is `int main()` in `app/main.cpp`.

## Constraints worth knowing before changing things

- **nanovg is batched**: every `nvg*` call flushes at `nvgEndFrame`, *after*
  `MpvView::draw()` runs — mpv renders immediately. Drawing anything opaque in
  the player without splitting the frame (`nvgEndFrame` / `nvgBeginFrame` around
  the mpv render) paints over the video. That was the "black video, working
  audio" bug.
- **libnx caps concurrent *blocking* socket calls at 16** (`num_bsd_sessions`).
  The peer loop is a single `poll()`, so it needs ~5 — this is *not* what limits
  peer count (see the ENOBUFS patch in `library/borealis/VENDORED.md`).
- **FAT32 caps a file at 4 GB**: the piece cache is chunked for this reason.
- The Switch has **no CA bundle**: curl runs with `SSL_VERIFYPEER`/`VERIFYHOST`
  off. A password and a persistent token cross that connection — worth fixing
  (bundle a CA in the romfs) before doing more with the Stremio account.
- `ScrollingFrame::setContentView()` detaches the view and forces its width, so
  **margins on a content view are ignored** — put the inset on the rows.
- A **GONE box's children stay focusable** (`Box::getDefaultFocus()` recurses
  without checking the box's own visibility), and `giveFocus()` on a
  non-focusable view is a **no-op** — which strands `Application::currentFocus`
  on whatever it was, including freed views. See `setSubtreeFocusable()` and the
  focus parking in `StremioTab::onAuthenticated`.
- The Switch outputs stereo: mpv is forced to `audio-channels=stereo` or 5.1
  dialogue (centre channel) is lost.

## Debugging on device

The log is **off by default** (Options → Log file) because it is unbuffered and
the engine dumps a `[stats]` line every 2s. Turn it on, restart, reproduce.

ZR in the player opens a live panel of every engine counter. `fail: N sock/local`
climbing with `last err: socket(): No buffer space available` means the socket
buffer pool is exhausted — see `library/borealis/VENDORED.md`.

For a crash, Atmosphère writes a report to `sdmc:/atmosphere/crash_reports/`.
The ELF is linked at vaddr 0, so the offsets map directly:

```sh
aarch64-none-elf-addr2line -Cfe /build/NX-torrent-player.elf <offset>
```
