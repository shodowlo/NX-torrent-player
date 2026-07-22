# Vendored borealis

This directory is a copy of borealis, checked into this repository rather than
pulled as a submodule, so a clone builds without extra steps.

| | |
|---|---|
| Upstream | https://github.com/xfangfang/borealis.git |
| Commit | `5f08b286f3df737f3321d2247a6fe633fcead03c` |
| Date | 2026-04-25 |

Everything here is upstream's, under its own license (see `LICENSE`), **except**
the local patch below.

## Local patches

All are marked with a `LOCAL PATCH` comment in the source.

### 1. `library/include/borealis/core/application.hpp` — `setInputType` public

Moved `Application::setInputType` from the private section to public. The player
forces `InputType::GAMEPAD` each frame so a stray screen touch does not cost the
first A press (borealis otherwise consumes the first gamepad button after a touch
just to switch input type back, so pause/resume took two presses).

### 2. `library/lib/platforms/switch/switch_wrapper.c` — smaller TCP buffers

`userAppInit()` sets smaller initial TCP socket buffers.

Every TCP socket draws its buffers from one fixed pool, sized
`sb_efficiency * (tcp_*_buf_max_size + udp_*)`, while each socket consumes
`tcp_*_buf_size` out of it. Stock defaults (32 KB tx / 64 KB rx initial) assume
a few large streams; a torrent engine wants the opposite — many small sockets (a
peer sends 16 KB blocks, we send 17-byte requests). With the defaults the pool
ran dry at ~20 sockets and `socket()` returned ENOBUFS ("No buffer space
available"), capping concurrent peers regardless of swarm size.

The max sizes are left at stock, so the memory footprint matches unpatched
borealis: only the per-socket initial buffers are cut. Growing the pool instead
starved the nvtegra video decoder, which then fell back to software H.264.

The patch is marked with a `LOCAL PATCH` comment in the source.

### 3. `library/lib/views/hint.cpp` — hint icon font size

Lowered the footer hint's `icon` label `fontSize` from the upstream `25.5` to
`21.5`, matching the `hint` text label (left at `21.5`). The Stremio view hint
(main.cpp) packs a second controller-button glyph into the hint *text* so both
the L and R glyphs sit in one chip; the text glyph must be the same size as the
auto icon glyph next to it. Lowering the icon (rather than raising the text)
keeps every footer label at its original size -- only the button glyphs shrink
slightly, to sit level with their labels.

## Updating

Re-cloning upstream **drops the patch** — re-apply it, and check this file's
commit hash is updated. To see the patch as a diff against upstream:

```sh
git clone https://github.com/xfangfang/borealis.git /tmp/borealis-upstream
cd /tmp/borealis-upstream && git checkout 5f08b286f3df737f3321d2247a6fe633fcead03c
diff -u /tmp/borealis-upstream/library/lib/platforms/switch/switch_wrapper.c \
        library/lib/platforms/switch/switch_wrapper.c
```
