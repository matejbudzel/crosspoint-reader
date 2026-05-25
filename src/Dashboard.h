#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GfxRenderer;

class Dashboard {
 public:
  void begin(GfxRenderer& renderer);
  void end();
  void loop(GfxRenderer& renderer);
  void next(GfxRenderer& renderer, const char* reason = "dashboard_image_next");
  void previous(GfxRenderer& renderer);
  bool renderCurrent(GfxRenderer& renderer, const char* eventName = "dashboard_image_render");
  bool renderNextForSleep(GfxRenderer& renderer);
  uint32_t millisUntilNextChange() const;
  bool hasItems();
  size_t itemCount();

 private:
  std::vector<std::string> files_;
  std::string directory_;
  std::string currentFilename_;
  size_t currentIndex_ = 0;
  uint32_t lastChangeMs_ = 0;
  bool active_ = false;
  bool hasRendered_ = false;

  bool scanFiles();
  void drawOverlay(GfxRenderer& renderer) const;
  void drawDot(GfxRenderer& renderer, int centerX, int centerY, bool selected) const;
};

extern Dashboard DASHBOARD;
