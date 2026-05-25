#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class DashboardActivity final : public Activity {
  ButtonNavigator buttonNavigator;

 public:
  explicit DashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Dashboard", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
