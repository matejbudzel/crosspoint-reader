#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class PowerLogActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex_ = 0;

  void toggleEnabled();
  void openLog();
  std::string statTitle(int index) const;
  std::string statValue(int index) const;

 public:
  explicit PowerLogActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("PowerLog", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
