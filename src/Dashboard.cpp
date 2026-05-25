#include "Dashboard.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>

#include <algorithm>
#include <cmath>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "PowerLog.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"

Dashboard DASHBOARD;

namespace {
constexpr const char* DASHBOARD_DIR = "/.dashboard";
constexpr const char* PRIMARY_SLEEP_DIR = "/.sleep";
constexpr const char* FALLBACK_SLEEP_DIR = "/sleep";
constexpr int OVERLAY_HEIGHT = 28;
constexpr int OVERLAY_MARGIN = 8;
constexpr int OVERLAY_PADDING = 8;
constexpr int DOT_RADIUS = 2;
constexpr int DOT_SPACING = 8;

bool openDirectory(const char* path, FsFile& dir) {
  dir = Storage.open(path);
  return dir && dir.isDirectory();
}
}  // namespace

void Dashboard::begin(GfxRenderer& renderer) {
  active_ = true;
  lastChangeMs_ = millis();

  if (!scanFiles()) {
    return;
  }

  renderCurrent(renderer, "dashboard_start");
}

void Dashboard::end() { active_ = false; }

void Dashboard::loop(GfxRenderer& renderer) {
  if (!active_ || files_.empty()) {
    return;
  }

  if (millisUntilNextChange() == 0) {
    next(renderer, "dashboard_image_auto");
  }
}

uint32_t Dashboard::millisUntilNextChange() const {
  const uint32_t intervalMs = static_cast<uint32_t>(std::max<uint8_t>(SETTINGS.softSleepSlideshowIntervalSeconds, 15)) *
                              1000UL;
  if (!active_ || files_.empty()) {
    return intervalMs;
  }
  const uint32_t elapsed = millis() - lastChangeMs_;
  return elapsed >= intervalMs ? 0 : intervalMs - elapsed;
}

void Dashboard::next(GfxRenderer& renderer, const char* reason) {
  if (!active_) {
    begin(renderer);
    return;
  }
  if (!scanFiles()) {
    return;
  }

  currentIndex_ = (currentIndex_ + 1) % files_.size();
  renderCurrent(renderer, reason ? reason : "dashboard_image_next");
}

void Dashboard::previous(GfxRenderer& renderer) {
  if (!active_) {
    begin(renderer);
    return;
  }
  if (!scanFiles()) {
    return;
  }

  currentIndex_ = currentIndex_ == 0 ? files_.size() - 1 : currentIndex_ - 1;
  renderCurrent(renderer, "dashboard_image_prev");
}

bool Dashboard::renderNextForSleep(GfxRenderer& renderer) {
  active_ = true;
  if (!scanFiles()) {
    return false;
  }
  if (hasRendered_) {
    currentIndex_ = (currentIndex_ + 1) % files_.size();
  }
  return renderCurrent(renderer, "dashboard_sleep");
}

bool Dashboard::hasItems() { return scanFiles(); }

size_t Dashboard::itemCount() {
  scanFiles();
  return files_.size();
}

bool Dashboard::scanFiles() {
  const std::string previousFile =
      !currentFilename_.empty()
          ? currentFilename_
          : (currentIndex_ < files_.size() ? files_[currentIndex_] : APP_STATE.dashboardLastImage);
  files_.clear();
  directory_.clear();

  FsFile dir;
  if (openDirectory(DASHBOARD_DIR, dir)) {
    directory_ = DASHBOARD_DIR;
  } else if (openDirectory(PRIMARY_SLEEP_DIR, dir)) {
    directory_ = PRIMARY_SLEEP_DIR;
  } else if (openDirectory(FALLBACK_SLEEP_DIR, dir)) {
    directory_ = FALLBACK_SLEEP_DIR;
  } else {
    return false;
  }

  char name[500];
  for (auto dirFile = dir.openNextFile(); dirFile; dirFile = dir.openNextFile()) {
    if (dirFile.isDirectory()) {
      dirFile.close();
      continue;
    }
    dirFile.getName(name, sizeof(name));
    std::string filename(name);
    if (filename.empty() || filename[0] == '.' || !FsHelpers::hasBmpExtension(filename)) {
      dirFile.close();
      continue;
    }
    Bitmap bitmap(dirFile);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      files_.push_back(filename);
    }
    dirFile.close();
  }
  dir.close();

  std::sort(files_.begin(), files_.end());
  if (files_.empty()) {
    currentFilename_.clear();
    currentIndex_ = 0;
    return false;
  }

  const auto it = std::find(files_.begin(), files_.end(), previousFile);
  if (it != files_.end()) {
    currentIndex_ = static_cast<size_t>(std::distance(files_.begin(), it));
  } else if (currentIndex_ >= files_.size()) {
    currentIndex_ = 0;
  }
  currentFilename_ = files_[currentIndex_];
  return true;
}

bool Dashboard::renderCurrent(GfxRenderer& renderer, const char* eventName) {
  if (!scanFiles()) {
    return false;
  }

  FsFile file;
  for (size_t attempt = 0; attempt < files_.size(); ++attempt) {
    const std::string path = directory_ + "/" + files_[currentIndex_];
    if (Storage.openFileForRead("DASH", path, file)) {
      Bitmap bitmap(file, true);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        int x, y;
        const int pageWidth = renderer.getScreenWidth();
        const int pageHeight = renderer.getScreenHeight();
        float cropX = 0, cropY = 0;

        if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
          float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
          if (ratio > screenRatio) {
            if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
              cropX = 1.0f - (screenRatio / ratio);
              ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
            }
            x = 0;
            y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
          } else {
            if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
              cropY = 1.0f - (ratio / screenRatio);
              ratio = static_cast<float>(bitmap.getWidth()) /
                      ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
            }
            x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
            y = 0;
          }
        } else {
          x = (pageWidth - bitmap.getWidth()) / 2;
          y = (pageHeight - bitmap.getHeight()) / 2;
        }

        {
          RenderLock lock;
          renderer.clearScreen();
          renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
          if (SETTINGS.sleepScreenCoverFilter ==
              CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
            renderer.invertScreen();
          }
          drawOverlay(renderer);
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }

        file.close();
        currentFilename_ = files_[currentIndex_];
        if (APP_STATE.dashboardLastImage != currentFilename_) {
          APP_STATE.dashboardLastImage = currentFilename_;
          APP_STATE.saveToFile();
        }
        lastChangeMs_ = millis();
        hasRendered_ = true;
        POWER_LOG.event(eventName);
        return true;
      }
      file.close();
    }
    currentIndex_ = (currentIndex_ + 1) % files_.size();
  }
  return false;
}

void Dashboard::drawOverlay(GfxRenderer& renderer) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int visibleDots = static_cast<int>(std::min<size_t>(files_.size(), 24));
  const int dotsWidth = visibleDots > 0 ? (visibleDots - 1) * DOT_SPACING + DOT_RADIUS * 2 + OVERLAY_PADDING : 0;
  const int batteryAreaWidth = metrics.batteryWidth + 8 + 28;
  const int overlayWidth = std::max(78, OVERLAY_PADDING + dotsWidth + batteryAreaWidth);
  const int x = renderer.getScreenWidth() - overlayWidth - OVERLAY_MARGIN;
  const int y = OVERLAY_MARGIN;

  renderer.fillRect(x, y, overlayWidth, OVERLAY_HEIGHT, false);
  renderer.drawRect(x, y, overlayWidth, OVERLAY_HEIGHT, true);

  if (visibleDots > 0) {
    const int dotsY = y + OVERLAY_HEIGHT / 2 + 2;
    int dotsX = x + OVERLAY_PADDING + DOT_RADIUS;
    for (int i = 0; i < visibleDots; ++i) {
      drawDot(renderer, dotsX + i * DOT_SPACING, dotsY, static_cast<size_t>(i) == currentIndex_);
    }
  }

  GUI.drawBatteryRight(renderer,
                       Rect{x + overlayWidth - metrics.batteryWidth - 8, y + 2, metrics.batteryWidth,
                            metrics.batteryHeight},
                       true);
}

void Dashboard::drawDot(GfxRenderer& renderer, int centerX, int centerY, bool selected) const {
  for (int dy = -DOT_RADIUS; dy <= DOT_RADIUS; ++dy) {
    for (int dx = -DOT_RADIUS; dx <= DOT_RADIUS; ++dx) {
      if (dx * dx + dy * dy <= DOT_RADIUS * DOT_RADIUS) {
        if (selected || dx * dx + dy * dy >= (DOT_RADIUS - 1) * (DOT_RADIUS - 1)) {
          renderer.drawPixel(centerX + dx, centerY + dy, true);
        }
      }
    }
  }
}
