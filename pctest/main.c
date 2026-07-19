// PC engine test harness: runs the exact Switch torrent engine (torrentfs) on a
// normal PC to measure download behaviour where DHT/µTP/UDP all work, so we can
// tell whether slowness is our algorithm or the Switch's environment.
//
//   Usage: ./enginetest <file.torrent | magnet:?...> [seconds]

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

#include "torrentfs.h"

static volatile int g_stop = 0;
static void on_sigint(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.torrent|magnet:...> [seconds]\n", argv[0]);
        return 2;
    }
    int max_secs = argc >= 3 ? atoi(argv[2]) : 60;
    signal(SIGINT, on_sigint);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    char err[256] = {0};
    printf("Ouverture de %s ...\n", argv[1]);
    torrentfs *tfs = torrentfs_open(argv[1], "/tmp/enginetest_cache.bin",
                                    err, sizeof(err));
    if (!tfs) {
        fprintf(stderr, "torrentfs_open a echoue: %s\n", err);
        curl_global_cleanup();
        return 1;
    }

    int64_t total_bytes = torrentfs_size(tfs);
    printf("Fichier: %.1f Mo, %d peers au depart\n\n",
           total_bytes / (1024.0 * 1024.0), torrentfs_peer_count(tfs));

    // Diagnostic: if a 3rd arg "tail" is passed, pin the playhead ~8 MB before
    // the end of the stream (where an MP4 moov atom usually lives) to test
    // whether the swarm can actually serve those tail pieces.
    if (argc >= 4 && strcmp(argv[3], "tail") == 0) {
        int64_t off = total_bytes - 8 * 1024 * 1024;
        if (off < 0) off = 0;
        torrentfs_set_playhead(tfs, off);
        printf(">>> playhead force pres de la fin (offset %.1f Mo)\n\n",
               off / (1024.0 * 1024.0));
    }

    int stream_sim = (argc >= 4 && strcmp(argv[3], "stream") == 0);
    int64_t plen = torrentfs_piece_len(tfs);

    int64_t prev_done = 0;
    for (int sec = 0; sec < max_secs && !g_stop; sec++) {
        sleep(1);

        int64_t done, total, ph;
        torrentfs_stats(tfs, &done, &total, &ph);

        // Simulate a player: pin the playhead at the lowest not-yet-done piece
        // (where a sequential player would block), so the sliding read-ahead
        // window and the stall-rescue path get exercised without mpv.
        if (stream_sim) {
            int64_t lowest = 0;
            while (lowest < total && torrentfs_piece_done(tfs, lowest)) lowest++;
            torrentfs_set_playhead(tfs, lowest * plen);
        }
        // Count how many of the last 4 pieces (the moov/tail region) are done.
        int tail_done = 0;
        for (int64_t k = total - 4; k < total; k++)
            if (torrentfs_piece_done(tfs, k)) tail_done++;
        int c[10];
        torrentfs_debug_counts(tfs, c);
        char last[128];
        torrentfs_last_err(tfs, last, sizeof(last));

        int64_t bytes_done = total > 0 ? (int64_t)((double)done / total * total_bytes) : 0;
        double per_piece = total > 0 ? (double)total_bytes / total : 0;
        double kbps = (done - prev_done) * per_piece / 1024.0;
        prev_done = done;

        printf("[%3ds] %lld/%lld morceaux  %.1f Mo  ph=%lld tail=%d/4  %d peers | "
               "conn %d/%d IN %d unchoke %d piece %d fail %d/%d | "
               "INT %d REQ %d up %d | %s\n",
               sec + 1, (long long)done, (long long)total,
               torrentfs_bytes_recv(tfs) / (1024.0 * 1024.0),
               (long long)ph, tail_done,
               torrentfs_peer_count(tfs),
               c[0], c[1], torrentfs_incoming_count(tfs),
               c[2], c[4], c[5], c[6], c[8], c[9], c[7], last);
        (void)kbps;
        fflush(stdout);
    }

    printf("\nArret. Fermeture...\n");
    torrentfs_cancel(tfs);
    torrentfs_close(tfs);
    curl_global_cleanup();
    return 0;
}
