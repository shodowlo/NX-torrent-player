#!/bin/sh
# Builds the PC engine test. Run inside a Debian/Ubuntu container with
# build-essential libmbedtls-dev libcurl4-openssl-dev zlib1g-dev installed.
set -e
cd "$(dirname "$0")/.."

gcc -O2 -g -Wall \
    -Ipctest/compat -Iengine \
    engine/bencode.c \
    engine/torrent.c \
    engine/udp_tracker.c \
    engine/peer.c \
    engine/torrentfs.c \
    engine/magnet.c \
    engine/dht.c \
    engine/dhtclient.c \
    pctest/utpstub.c \
    pctest/main.c \
    -o pctest/enginetest \
    -lpthread -lmbedcrypto -lmbedtls -lmbedx509 -lcurl -lz

echo "OK -> pctest/enginetest"
