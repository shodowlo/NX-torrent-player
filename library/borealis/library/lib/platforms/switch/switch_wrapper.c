/*
    Copyright 2019 natinusala
    Copyright 2019 WerWolv
    Copyright 2019 p-sam

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <stdio.h>
#include <switch.h>
#include <unistd.h>

static int nxlink_sock = -1;

void userAppInit()
{
    printf("userAppInit\n");
    appletLockExit();

    // Init network
    SocketInitConfig cfg = *(socketGetDefaultInitConfig());
    AppletType at        = appletGetAppletType();
    if (at == AppletType_Application || at == AppletType_SystemApplication)
    {
        cfg.num_bsd_sessions = 12; // default is 3
        cfg.sb_efficiency    = 8; // default is 4

        // LOCAL PATCH (NX Torrent Player) -- see CLAUDE.md, re-apply if borealis
        // is re-cloned. Every TCP socket draws its buffers from one fixed pool,
        // sized sb_efficiency * (tcp_*_buf_max_size + udp_*), while each socket
        // consumes tcp_*_buf_size out of it. Stock defaults (32 KB tx / 64 KB rx
        // initial) are tuned for a few big streams; the torrent engine wants the
        // opposite -- many small sockets. With the defaults the pool ran dry at
        // ~20 sockets and socket() started returning ENOBUFS ("No buffer space
        // available"), capping concurrent peers no matter how many the swarm
        // offered. Small initial buffers (a peer sends us 16 KB blocks; we send
        // 17-byte requests) with a large max keeps the pool big and the
        // per-socket cost low.
        // Socket count is pool/per-socket, so shrink the per-socket cost rather
        // than growing the pool: the pool is transfer memory taken from the
        // app's heap, and enlarging it starved the nvtegra video decoder, which
        // then failed to create its device and silently fell back to software
        // H.264 -- unplayable for 60fps content. The max sizes below are stock,
        // so the footprint matches unpatched borealis exactly; only the initial
        // per-socket buffers are cut (a peer sends 16 KB blocks and we send
        // 17-byte requests, so 32 KB rx / 16 KB tx is ample).
        cfg.tcp_tx_buf_size     = 0x4000; // 16 KB initial (was 32 KB)
        cfg.tcp_rx_buf_size     = 0x8000; // 32 KB initial (was 64 KB)
        cfg.tcp_tx_buf_max_size = 0x40000; // 256 KB -- stock, sizes the pool
        cfg.tcp_rx_buf_max_size = 0x40000; // 256 KB -- stock

        if (R_FAILED(socketInitialize(&cfg)))
            socketInitializeDefault(); // fall back if the config is rejected
    }
    else
    {
        cfg.num_bsd_sessions = 2;
        cfg.sb_efficiency    = 1;
        socketInitialize(&cfg);
    }

#ifdef DEBUG
    nxlink_sock = nxlinkStdio();
#endif

    romfsInit();
    plInitialize(PlServiceType_User);
    setsysInitialize();
    setInitialize();
    psmInitialize();
    nifmInitialize(NifmServiceType_User);
    lblInitialize();
}

void userAppExit()
{
    printf("userAppExit\n");

    // backlight
    lblExit();
    // network state
    nifmExit();
    // power state
    psmExit();
    // system language...
    setExit();
    // system theme, system version...
    setsysExit();
    // system font
    plExit();

    romfsExit();

    if (nxlink_sock != -1)
        close(nxlink_sock);

    socketExit();

    appletUnlockExit();
}
