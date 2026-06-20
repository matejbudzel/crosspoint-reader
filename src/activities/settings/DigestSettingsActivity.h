#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DigestSettingsActivity final : public Activity {
 public:
  explicit DigestSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DigestSettings", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;

  void handleSelection();
};
