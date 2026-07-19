#pragma once

#include <borealis.hpp>

// The options screen, opened with X from the browser. Every change is written
// to config.json on the spot -- there is no "save" button to forget.
class SettingsActivity : public brls::Activity
{
  public:
    brls::View* createContentView() override;
};
