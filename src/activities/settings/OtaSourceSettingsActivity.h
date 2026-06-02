#pragma once

#include "OtaSourceStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class OtaSourceSettingsActivity final : public Activity {
 public:
  explicit OtaSourceSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int sourceIndex = -1)
      : Activity("OtaSourceSettings", renderer, mappedInput), sourceIndex(sourceIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;
  int sourceIndex;
  OtaSource editSource;
  bool isNewSource = false;
  bool showSaveError = false;

  int getMenuItemCount() const;
  void handleSelection();
  bool saveSource();
};
