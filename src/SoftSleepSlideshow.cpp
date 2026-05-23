#include "SoftSleepSlideshow.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>

#include <algorithm>
#include <cmath>

#include "CrossPointSettings.h"
#include "PowerLog.h"
#include "activities/RenderLock.h"
#include "components/UITheme.h"

SoftSleepSlideshow SOFT_SLEEP_SLIDESHOW;

namespace {
constexpr const char* PRIMARY_SLEEP_DIR = "/.sleep";
constexpr const char* FALLBACK_SLEEP_DIR = "/sleep";
constexpr int OVERLAY_WIDTH = 78;
constexpr int OVERLAY_HEIGHT = 28;
constexpr int OVERLAY_MARGIN = 8;

bool openDirectory(const char* path, FsFile& dir) {
  dir = Storage.open(path);
  return dir && dir.isDirectory();
}
}  // namespace

void SoftSleepSlideshow::begin(GfxRenderer& renderer) {
  active_ = true;
  currentIndex_ = 0;
  lastChangeMs_ = millis();

  if (!scanFiles()) {
    return;
  }

  renderCurrent(renderer, "soft_sleep_slideshow_start");
}

void SoftSleepSlideshow::end() {
  active_ = false;
  files_.clear();
  directory_.clear();
}

void SoftSleepSlideshow::loop(GfxRenderer& renderer) {
  if (!active_ || files_.empty()) {
    return;
  }

  const uint32_t intervalMs = static_cast<uint32_t>(std::max<uint8_t>(SETTINGS.softSleepSlideshowIntervalSeconds, 15)) *
                              1000UL;
  if (millis() - lastChangeMs_ >= intervalMs) {
    next(renderer, "soft_sleep_image_auto");
  }
}

void SoftSleepSlideshow::next(GfxRenderer& renderer, const char* reason) {
  if (!active_) {
    begin(renderer);
    return;
  }
  if (files_.empty() && !scanFiles()) {
    return;
  }
  if (files_.empty()) {
    return;
  }

  currentIndex_ = (currentIndex_ + 1) % files_.size();
  renderCurrent(renderer, reason ? reason : "soft_sleep_image_next");
}

void SoftSleepSlideshow::previous(GfxRenderer& renderer) {
  if (!active_) {
    begin(renderer);
    return;
  }
  if (files_.empty() && !scanFiles()) {
    return;
  }
  if (files_.empty()) {
    return;
  }

  currentIndex_ = currentIndex_ == 0 ? files_.size() - 1 : currentIndex_ - 1;
  renderCurrent(renderer, "soft_sleep_image_prev");
}

bool SoftSleepSlideshow::scanFiles() {
  files_.clear();
  directory_.clear();

  FsFile dir;
  if (openDirectory(PRIMARY_SLEEP_DIR, dir)) {
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
  if (currentIndex_ >= files_.size()) {
    currentIndex_ = 0;
  }
  return !files_.empty();
}

bool SoftSleepSlideshow::renderCurrent(GfxRenderer& renderer, const char* eventName) {
  if (files_.empty() || directory_.empty()) {
    return false;
  }

  FsFile file;
  for (size_t attempt = 0; attempt < files_.size(); ++attempt) {
    const std::string path = directory_ + "/" + files_[currentIndex_];
    if (Storage.openFileForRead("SSLP", path, file)) {
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
          drawBatteryOverlay(renderer);
          renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        }

        file.close();
        lastChangeMs_ = millis();
        POWER_LOG.event(eventName);
        return true;
      }
      file.close();
    }
    currentIndex_ = (currentIndex_ + 1) % files_.size();
  }
  return false;
}

void SoftSleepSlideshow::drawBatteryOverlay(GfxRenderer& renderer) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int x = renderer.getScreenWidth() - OVERLAY_WIDTH - OVERLAY_MARGIN;
  const int y = OVERLAY_MARGIN;
  renderer.fillRect(x, y, OVERLAY_WIDTH, OVERLAY_HEIGHT, false);
  renderer.drawRect(x, y, OVERLAY_WIDTH, OVERLAY_HEIGHT, true);
  GUI.drawBatteryRight(renderer, Rect{x + OVERLAY_WIDTH - metrics.batteryWidth - 8, y + 2, metrics.batteryWidth,
                                      metrics.batteryHeight},
                       true);
}
