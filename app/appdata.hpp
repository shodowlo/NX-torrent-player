#pragma once

// All files the app writes live under one folder on the SD card instead of the
// root. The directory is created at startup by ensureAppDataDir() in main.cpp.
#define APPDATA_DIR      "sdmc:/switch/NX-torrent-player"
#define APPDATA_LOG      APPDATA_DIR "/nx-torrent-player.log"
#define APPDATA_CACHE    APPDATA_DIR "/cache.bin"
// Folder the user drops .torrent files into (scanned for the main menu list).
#define APPDATA_TORRENTS APPDATA_DIR "/torrents"
// Cached Stremio artwork, one .jpg per library item. Posters never change, so
// entries are kept forever and a hit never touches the network.
#define APPDATA_POSTERS  APPDATA_DIR "/posters"
