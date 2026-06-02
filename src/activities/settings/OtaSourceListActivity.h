#pragma once

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class OtaSourceListActivity final : public Activity {
 public:
  explicit OtaSourceListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false)
      : Activity("OtaSourceList", renderer, mappedInput), pickerMode(pickerMode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;
  bool pickerMode = false;

  int getItemCount() const;
  void handleSelection();
};
