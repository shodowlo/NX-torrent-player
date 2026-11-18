#pragma once

#include <borealis.hpp>

// The options screen, opened from the header's gear. Every change is written to
// config.json on the spot -- there is no "save" button to forget.
class SettingsActivity : public brls::Activity
{
  public:
    brls::View* createContentView() override;
};

// The account screen, opened from the header's profile button: who is signed in
// to Stremio, and the way out. Its own screen rather than a section of Options,
// which is about the app rather than about the account.
class AccountActivity : public brls::Activity
{
  public:
    brls::View* createContentView() override;
};

// Resizes borealis' logical space to the UI size configured for the mode the
// console is currently in (docked or handheld), and relayouts. Call it once the
// window exists, on every window-size change, and after the setting is edited.
void applyUiScale();

// Whether the console is docked, from the applet's operation mode. Not from the
// window width: on Switch 2 the app always renders at 1080p (see the LOCAL PATCH
// in borealis' glfw_video.cpp), handheld included.
bool isDocked();

// Run after applyUiScale has actually changed the logical space -- for whatever
// has to be recomputed from it and cannot hear about it otherwise: the size
// change comes from setWindowSize, which does NOT fire the window-size-changed
// event (that event is what calls us, not the other way round).
void setUiScaleHook(std::function<void()> fn);
