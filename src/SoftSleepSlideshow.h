#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

class SoftSleepSlideshow {
 public:
  void begin(GfxRenderer& renderer);
  void end();
  void loop(GfxRenderer& renderer);
  void next(GfxRenderer& renderer, const char* reason = "manual_next");
  void previous(GfxRenderer& renderer);

 private:
  std::vector<std::string> files_;
  std::string directory_;
  size_t currentIndex_ = 0;
  uint32_t lastChangeMs_ = 0;
  bool active_ = false;

  bool scanFiles();
  bool renderCurrent(GfxRenderer& renderer, const char* eventName);
  void drawBatteryOverlay(GfxRenderer& renderer) const;
};

extern SoftSleepSlideshow SOFT_SLEEP_SLIDESHOW;
