#include "RefreshDownloadActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>

#include <cstdarg>
#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "FsHelpers.h"
#include "util/BookCacheUtils.h"

namespace {
constexpr const char* HTTP_DEBUG_LOG_PATH = "/.crosspoint/http-debug.log";

void refreshDebugLog(const char* fmt, ...) {
  Storage.mkdir("/.crosspoint");
  HalFile file = Storage.open(HTTP_DEBUG_LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    return;
  }

  char line[240];
  va_list args;
  va_start(args, fmt);
  const int written = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (written > 0) {
    file.write(reinterpret_cast<const uint8_t*>(line),
               static_cast<size_t>(std::min(written, static_cast<int>(sizeof(line) - 1))));
  }
  file.close();
}
}  // namespace

void RefreshDownloadActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void RefreshDownloadActivity::onExit() {
  Activity::onExit();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
  }
}

bool RefreshDownloadActivity::prepareDestination(const std::string& path) {
  if (path.empty() || path[0] != '/') {
    return false;
  }

  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return true;
  }

  return Storage.mkdir(path.substr(0, lastSlash).c_str(), true);
}

void RefreshDownloadActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  const std::string url = SETTINGS.refreshDownloadUrl;
  const std::string path = SETTINGS.refreshDownloadPath;
  refreshDebugLog("\n[%lu] REFRESH start url=%s dest=%s\n", millis(), url.c_str(), path.c_str());
  if (url.empty() || path.empty() || !prepareDestination(path)) {
    refreshDebugLog("[%lu] REFRESH result=ERROR reason=missing_or_bad_destination\n", millis());
    RenderLock lock(*this);
    state = State::ERROR;
    errorMessage = tr(STR_NOT_SET);
    return;
  }

  {
    RenderLock lock(*this);
    state = State::DOWNLOADING;
    downloaded = 0;
    total = 0;
    cancelRequested = false;
  }
  requestUpdateAndWait();

  const auto result = HttpDownloader::downloadToFile(
      url, path,
      [this](size_t bytesDownloaded, size_t bytesTotal) {
        downloaded = bytesDownloaded;
        total = bytesTotal;
        mappedInput.update();
        if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
            mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          cancelRequested = true;
        }
        requestUpdate(true);
      },
      &cancelRequested);
  refreshDebugLog("[%lu] REFRESH download_done result=%d cancel=%s downloaded=%zu total=%zu\n", millis(),
                  static_cast<int>(result), cancelRequested ? "yes" : "no", downloaded, total);

  if (result == HttpDownloader::OK && !cancelRequested && downloaded > 0) {
    clearBookCache(path);
    RecentBook book = RECENT_BOOKS.getDataFromBook(path);
    if (FsHelpers::hasXtcExtension(path)) {
      book.coverBmpPath.clear();
    }
    if (book.title.empty()) {
      const size_t lastSlash = path.find_last_of('/');
      book.title = lastSlash == std::string::npos ? path : path.substr(lastSlash + 1);
    }
    RECENT_BOOKS.addBook(path, book.title, book.author, book.coverBmpPath);
  }

  bool shouldFinish = false;
  {
    RenderLock lock(*this);
    if (result == HttpDownloader::OK && !cancelRequested && downloaded > 0) {
      state = State::DONE;
      refreshDebugLog("[%lu] REFRESH result=OK downloaded=%zu\n", millis(), downloaded);
    } else if (cancelRequested) {
      shouldFinish = true;
      refreshDebugLog("[%lu] REFRESH result=ABORTED downloaded=%zu\n", millis(), downloaded);
    } else {
      state = State::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      refreshDebugLog("[%lu] REFRESH result=ERROR reason=fetch_failed downloaded=%zu\n", millis(), downloaded);
    }
  }
  if (shouldFinish) {
    finish();
  }
}

void RefreshDownloadActivity::loop() {
  if (state == State::DONE || state == State::ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (state == State::DOWNLOADING && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    cancelRequested = true;
  }
}

void RefreshDownloadActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_REFRESH_DOWNLOAD));

  const int height = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - height) / 2;

  if (state == State::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_DOWNLOADING));
    if (total > 0) {
      const int percent = static_cast<int>((downloaded * 100) / total);
      GUI.drawProgressBar(
          renderer,
          Rect{metrics.contentSidePadding, top + height + metrics.verticalSpacing,
               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
          percent, 100);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == State::DONE) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_DONE), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, errorMessage.empty() ? tr(STR_DOWNLOAD_FAILED) : errorMessage.c_str(),
                              true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
