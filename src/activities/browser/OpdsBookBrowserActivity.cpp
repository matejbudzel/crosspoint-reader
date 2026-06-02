#include "OpdsBookBrowserActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <OpdsStream.h>
#include <Arduino.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdarg>
#include <cstring>
#include <sstream>
#include <vector>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/BookCacheUtils.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr const char* OPDS_SYNC_TMP_DIR = "/.crosspoint/tmp/opds-sync";
constexpr const char* OPDS_SYNC_LOG_PATH = "/.crosspoint/opds-sync.log";
constexpr unsigned long DOWNLOAD_PROGRESS_RENDER_INTERVAL_MS = 500;
constexpr size_t DOWNLOAD_PROGRESS_RENDER_BYTES = 16 * 1024;

const char* downloadErrorName(const HttpDownloader::DownloadError result) {
  switch (result) {
    case HttpDownloader::OK:
      return "OK";
    case HttpDownloader::HTTP_ERROR:
      return "HTTP_ERROR";
    case HttpDownloader::FILE_ERROR:
      return "FILE_ERROR";
    case HttpDownloader::ABORTED:
      return "ABORTED";
  }
  return "UNKNOWN";
}

std::string normalizeDownloadRoot(const std::string& root) {
  std::vector<std::string> components;
  std::stringstream stream(root);
  std::string component;

  while (std::getline(stream, component, '/')) {
    if (component.empty() || component == "." || component == "..") {
      continue;
    }
    std::string clean = StringUtils::sanitizeFilename(component);
    if (!clean.empty()) {
      components.push_back(clean);
    }
  }

  if (components.empty()) return "/";

  std::string normalized;
  for (const auto& part : components) {
    normalized += "/";
    normalized += part;
  }
  return normalized;
}

std::string joinPath(const std::string& dir, const std::string& filename) {
  if (dir.empty() || dir == "/") return "/" + filename;
  return dir + "/" + filename;
}

std::string fnv1aHex(const std::string& value) {
  uint32_t hash = 2166136261u;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 16777619u;
  }

  char buf[9];
  snprintf(buf, sizeof(buf), "%08lx", static_cast<unsigned long>(hash));
  return buf;
}

std::string buildServerKey(const OpdsServer& server) {
  const std::string stable = server.url.empty() ? server.name : server.url;
  return fnv1aHex(stable);
}

std::string buildDownloadPath(const OpdsServer& server, const OpdsEntry& book) {
  const std::string root = normalizeDownloadRoot(server.downloadRoot);
  std::string title = StringUtils::sanitizeFilename(book.title);
  if (title.empty()) {
    title = "untitled";
  }

  if (server.saveLayout == OpdsSaveLayout::ByAuthor) {
    std::string author = book.author.empty() ? "unknown" : StringUtils::sanitizeFilename(book.author);
    if (author.empty()) {
      author = "unknown";
    }
    const std::string dir = joinPath(root, author);
    if (!Storage.exists(dir.c_str())) {
      Storage.mkdir(dir.c_str());
    }
    return joinPath(dir, title + ".epub");
  }

  if (!Storage.exists(root.c_str())) {
    Storage.mkdir(root.c_str());
  }

  const std::string basename =
      book.author.empty() ? title : StringUtils::sanitizeFilename(book.author + " - " + book.title);
  return joinPath(root, basename + ".epub");
}

bool filesEqual(const std::string& leftPath, const std::string& rightPath) {
  HalFile left;
  HalFile right;
  if (!Storage.openFileForRead("OPDS", leftPath, left) || !Storage.openFileForRead("OPDS", rightPath, right)) {
    return false;
  }
  if (left.fileSize64() != right.fileSize64()) {
    return false;
  }

  constexpr size_t CHUNK_SIZE = 512;
  uint8_t leftBuf[CHUNK_SIZE];
  uint8_t rightBuf[CHUNK_SIZE];

  while (left.available() > 0) {
    const int leftRead = left.read(leftBuf, sizeof(leftBuf));
    const int rightRead = right.read(rightBuf, sizeof(rightBuf));
    if (leftRead != rightRead || leftRead < 0 || rightRead < 0) {
      return false;
    }
    if (memcmp(leftBuf, rightBuf, static_cast<size_t>(leftRead)) != 0) {
      return false;
    }
  }
  return true;
}
}

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  serverKey = buildServerKey(server);
  OPDS_SYNC_SELECTION_STORE.loadFromFile();
  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  searchTemplate = "";
  currentPath = "";
  selectorIndex = 0;
  consumeConfirm = false;
  consumeBack = false;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);
  syncCurrent = syncTotal = 0;
  syncPreflightKnownBytes = 0;
  syncPreflightUnknownCount = 0;
  syncPreflightProbeFailures = 0;
  blockingSyncInProgress = false;
  cancelDownload = false;
  requestUpdate();

  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();
  closeSyncLog();
  entries.clear();
  navigationHistory.clear();

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }

  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (consumeBack && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    consumeBack = false;
    return;
  }

  if (state == BrowserState::ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        state = BrowserState::LOADING;
        statusMessage = tr(STR_LOADING);
        requestUpdateAndWait();
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
               mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    }
    return;
  }

  if (state == BrowserState::SYNC_PREFLIGHT) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      syncSelectedBooks();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      logSync("preflight cancelled before download");
      closeSyncLog();
      state = BrowserState::BROWSING;
      requestUpdate();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    if (blockingSyncInProgress) {
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.isPressed(MappedInputManager::Button::Back)) {
      cancelDownload = true;
    }
    return;
  }

  if (state == BrowserState::BROWSING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (totalRowCount() > 0) {
        if (isSyncRow(selectorIndex)) {
          prepareSyncPreflight();
        } else if (isSelectForSyncRow(selectorIndex)) {
          selectorIndex = 0;
          state = BrowserState::SELECTING_SYNC;
          requestUpdate();
        } else if (const auto* entry = entryForRow(selectorIndex)) {
          entry->type == OpdsEntryType::BOOK ? downloadBook(*entry) : navigateToEntry(*entry);
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (!searchTemplate.empty() && selectorIndex == actionRowCount()) launchSearch();
    }

    const size_t rows = totalRowCount();
    if (rows > 0) {
      buttonNavigator.onNextRelease([this] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, totalRowCount());
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, totalRowCount());
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, totalRowCount(), PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, totalRowCount(), PAGE_ITEMS);
        requestUpdate();
      });
    }
  } else if (state == BrowserState::SELECTING_SYNC) {
    const auto bookRows = bookEntryIndices();
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!bookRows.empty()) {
        toggleSyncSelection(entries[bookRows[selectorIndex]]);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      selectorIndex = 0;
      state = BrowserState::BROWSING;
      requestUpdate();
    }

    if (!bookRows.empty()) {
      buttonNavigator.onNextRelease([this, count = bookRows.size()] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, count);
        requestUpdate();
      });
      buttonNavigator.onPreviousRelease([this, count = bookRows.size()] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, count);
        requestUpdate();
      });
      buttonNavigator.onNextContinuous([this, count = bookRows.size()] {
        selectorIndex = ButtonNavigator::nextPageIndex(selectorIndex, count, PAGE_ITEMS);
        requestUpdate();
      });
      buttonNavigator.onPreviousContinuous([this, count = bookRows.size()] {
        selectorIndex = ButtonNavigator::previousPageIndex(selectorIndex, count, PAGE_ITEMS);
        requestUpdate();
      });
    }
  }
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Show server name in header if available, otherwise generic title
  const char* headerTitle = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, headerTitle, true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::SYNC_PREFLIGHT) {
    renderer.drawCenteredText(UI_10_FONT_ID, 70, "Confirm OPDS sync", true, EpdFontFamily::BOLD);

    char countLine[48];
    snprintf(countLine, sizeof(countLine), "Files: %zu", syncTotal);
    renderer.drawCenteredText(UI_10_FONT_ID, 105, countLine);

    std::string sizeLine = "Size: " + formatBytes(syncPreflightKnownBytes);
    if (syncPreflightUnknownCount > 0) {
      char suffix[40];
      snprintf(suffix, sizeof(suffix), " + %zu unknown", syncPreflightUnknownCount);
      sizeLine += suffix;
    }
    renderer.drawCenteredText(UI_10_FONT_ID, 135, sizeLine.c_str());

    if (syncPreflightProbeFailures > 0) {
      char unknownLine[48];
      snprintf(unknownLine, sizeof(unknownLine), "Size probe failed: %zu", syncPreflightProbeFailures);
      renderer.drawCenteredText(UI_10_FONT_ID, 165, unknownLine);
    }

    const std::string signal = "WiFi: " + wifiSignalIndicator();
    renderer.drawCenteredText(UI_10_FONT_ID, 195, signal.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, 230, "Download will block UI");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    if (blockingSyncInProgress) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const std::string signal = "WiFi: " + wifiSignalIndicator();
      auto signalLine = renderer.truncatedText(SMALL_FONT_ID, signal.c_str(), pageWidth - metrics.contentSidePadding * 2);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, 38, signalLine.c_str());
    }

    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, tr(STR_DOWNLOADING));
    auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), pageWidth - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, title.c_str());
    if (syncTotal > 0) {
      char syncStatus[32];
      snprintf(syncStatus, sizeof(syncStatus), "%zu/%zu", syncCurrent, syncTotal);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 18, syncStatus);
    }
    if (downloadTotal > 0) {
      const int barY = syncTotal > 0 ? pageHeight / 2 + 44 : pageHeight / 2 + 20;
      GUI.drawProgressBar(renderer, Rect{50, barY, pageWidth - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    if (!blockingSyncInProgress) {
      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::SELECTING_SYNC) {
    const auto bookRows = bookEntryIndices();
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_TOGGLE), "", tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    if (bookRows.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
    } else {
      const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
      renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

      for (size_t i = pageStartIndex; i < bookRows.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS);
           i++) {
        const auto& entry = entries[bookRows[i]];
        std::string displayText =
            OPDS_SYNC_SELECTION_STORE.isSelected(serverKey, downloadUrlForBook(entry)) ? "[x] " : "[ ] ";
        displayText += entry.title;
        if (!entry.author.empty()) displayText += " - " + entry.author;
        auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
        renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                          i != static_cast<size_t>(selectorIndex));
      }
    }
    renderer.displayBuffer();
    return;
  }

  const OpdsEntry* selectedEntry = entryForRow(selectorIndex);
  const char* confirmLabel = tr(STR_OPEN);
  if (isSyncRow(selectorIndex)) {
    confirmLabel = tr(STR_SYNC_SELECTED);
  } else if (isSelectForSyncRow(selectorIndex)) {
    confirmLabel = tr(STR_SELECT);
  } else if (selectedEntry && selectedEntry->type == OpdsEntryType::BOOK) {
    confirmLabel = tr(STR_DOWNLOAD);
  }
  const char* searchLabel = (!searchTemplate.empty() && selectorIndex == actionRowCount()) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  const size_t rowCount = totalRowCount();
  if (rowCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_ENTRIES));
  } else {
    const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
    renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

    for (size_t i = pageStartIndex; i < rowCount && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
      std::string displayText;
      if (isSyncRow(i)) {
        displayText = "> ";
        displayText += tr(STR_SYNC_SELECTED);
      } else if (isSelectForSyncRow(i)) {
        displayText = "> ";
        displayText += tr(STR_SELECT_FOR_SYNC);
      } else if (const auto* entry = entryForRow(i)) {
        displayText = (entry->type == OpdsEntryType::NAVIGATION) ? "> " + entry->title : entry->title;
        if (entry->type == OpdsEntryType::BOOK && !entry->author.empty()) displayText += " - " + entry->author;
      }
      auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), pageWidth - 40);
      renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(),
                        i != static_cast<size_t>(selectorIndex));
    }
  }
  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  std::string url = (path.find("http") == 0) ? path : UrlUtils::buildUrl(server.url, path);
  LOG_DBG("OPDS", "Fetching: %s", url.c_str());
  OpdsParser parser;
  {
    OpdsParserStream stream{parser};
    if (!HttpDownloader::fetchUrl(url, stream, server.username, server.password)) {
      state = BrowserState::ERROR;
      errorMessage = tr(STR_FETCH_FEED_FAILED);
      requestUpdate();
      return;
    }
  }

  if (!parser) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  searchTemplate = parser.getSearchTemplate();
  const auto& nextUrl = parser.getNextPageUrl();
  const auto& prevUrl = parser.getPrevPageUrl();
  entries = std::move(parser).getEntries();

  if (!prevUrl.empty()) {
    entries.insert(entries.begin(), OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_PREV_PAGE), "", prevUrl, ""});
  }
  if (!nextUrl.empty()) {
    entries.push_back(OpdsEntry{OpdsEntryType::NAVIGATION, tr(STR_NEXT_PAGE), "", nextUrl, ""});
  }

  selectorIndex = 0;
  state = entries.empty() ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entries.empty()) errorMessage = tr(STR_NO_ENTRIES);
  requestUpdate();
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  navigationHistory.push_back(currentPath);
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, entry.href);

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  entries.clear();
  selectorIndex = 0;
  requestUpdateAndWait();
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    onGoHome();
  } else {
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    entries.clear();
    selectorIndex = 0;
    requestUpdateAndWait();
    fetchFeed(currentPath);
  }
}

size_t OpdsBookBrowserActivity::syncOverlapCount() const {
  size_t count = 0;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK && OPDS_SYNC_SELECTION_STORE.isSelected(serverKey, downloadUrlForBook(entry))) {
      count++;
    }
  }
  return count;
}

bool OpdsBookBrowserActivity::hasBookEntries() const {
  return std::any_of(entries.begin(), entries.end(),
                     [](const OpdsEntry& entry) { return entry.type == OpdsEntryType::BOOK; });
}

size_t OpdsBookBrowserActivity::actionRowCount() const {
  size_t count = 0;
  if (syncOverlapCount() > 0) {
    count++;
  }
  if (hasBookEntries()) {
    count++;
  }
  return count;
}

bool OpdsBookBrowserActivity::isSyncRow(const size_t row) const { return syncOverlapCount() > 0 && row == 0; }

bool OpdsBookBrowserActivity::isSelectForSyncRow(const size_t row) const {
  const size_t syncRows = syncOverlapCount() > 0 ? 1 : 0;
  return hasBookEntries() && row == syncRows;
}

size_t OpdsBookBrowserActivity::totalRowCount() const { return actionRowCount() + entries.size(); }

const OpdsEntry* OpdsBookBrowserActivity::entryForRow(const size_t row) const {
  const size_t actions = actionRowCount();
  if (row < actions) {
    return nullptr;
  }
  const size_t entryIndex = row - actions;
  return entryIndex < entries.size() ? &entries[entryIndex] : nullptr;
}

std::vector<size_t> OpdsBookBrowserActivity::bookEntryIndices() const {
  std::vector<size_t> result;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].type == OpdsEntryType::BOOK) {
      result.push_back(i);
    }
  }
  return result;
}

void OpdsBookBrowserActivity::toggleSyncSelection(const OpdsEntry& book) {
  OPDS_SYNC_SELECTION_STORE.toggleSelection(
      OpdsSyncSelection{serverKey, downloadUrlForBook(book), book.title, book.author});
  requestUpdate();
}

void OpdsBookBrowserActivity::resetDownloadProgressRender() {
  lastProgressRenderBytes = 0;
  lastProgressRenderMs = millis();
}

void OpdsBookBrowserActivity::startSyncLog(const size_t selectedCount) {
  closeSyncLog();
  syncCancelLogged = false;
  Storage.mkdir("/.crosspoint");
  syncLogOpen = Storage.openFileForWrite("OPS", OPDS_SYNC_LOG_PATH, syncLogFile);
  if (!syncLogOpen) {
    LOG_ERR("OPDS", "Failed to open OPDS sync log");
    return;
  }
  logSync("sync start server='%s' url='%s' path='%s' selected=%zu", server.name.c_str(), server.url.c_str(),
          currentPath.c_str(), selectedCount);
}

void OpdsBookBrowserActivity::closeSyncLog() {
  if (syncLogOpen) {
    syncLogFile.flush();
    syncLogFile.close();
  }
  syncLogOpen = false;
}

void OpdsBookBrowserActivity::logSync(const char* fmt, ...) {
  if (!syncLogOpen) return;

  char message[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  char line[300];
  const int len = snprintf(line, sizeof(line), "[%lu] %s\n", millis(), message);
  if (len <= 0) return;
  const size_t writeLen = std::min(static_cast<size_t>(len), sizeof(line) - 1);
  syncLogFile.write(line, writeLen);
  syncLogFile.flush();
}

void OpdsBookBrowserActivity::updateDownloadProgress(const size_t downloaded, const size_t total) {
  downloadProgress = downloaded;
  downloadTotal = total;

  if (!blockingSyncInProgress) {
    mappedInput.update();
    if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelDownload = true;
      if (syncLogOpen && !syncCancelLogged) {
        logSync("cancel requested by Back button at downloaded=%zu total=%zu", downloaded, total);
        syncCancelLogged = true;
      }
    }
  }

  const unsigned long now = millis();
  const bool enoughTime = now - lastProgressRenderMs >= DOWNLOAD_PROGRESS_RENDER_INTERVAL_MS;
  const bool enoughBytes = downloaded >= lastProgressRenderBytes + DOWNLOAD_PROGRESS_RENDER_BYTES;
  const bool complete = total > 0 && downloaded >= total;
  if (enoughTime || enoughBytes || complete) {
    lastProgressRenderMs = now;
    lastProgressRenderBytes = downloaded;
    requestUpdate(true);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  syncCurrent = syncTotal = 0;
  cancelDownload = false;
  resetDownloadProgressRender();
  requestUpdate(true);

  std::string filename = buildDownloadPath(server, book);
  if (downloadBookToPath(book, filename)) {
    markBookSynced(book, filename);
    state = BrowserState::BROWSING;
  } else if (cancelDownload) {
    state = BrowserState::BROWSING;
    consumeBack = true;
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
  }
  cancelDownload = false;
  requestUpdate();
}

bool OpdsBookBrowserActivity::downloadBookToPath(const OpdsEntry& book, const std::string& destPath) {
  const std::string downloadUrl = downloadUrlForBook(book);
  LOG_DBG("OPDS", "Downloading: %s -> %s", downloadUrl.c_str(), destPath.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, destPath,
      [this](const size_t downloaded, const size_t total) { updateDownloadProgress(downloaded, total); },
      &cancelDownload, server.username, server.password);

  if (result == HttpDownloader::OK) {
    clearBookCache(destPath);
    return true;
  }
  return false;
}

bool OpdsBookBrowserActivity::isAlreadySynced(const OpdsEntry& book, const std::string& targetPath) const {
  if (book.updated.empty() || !Storage.exists(targetPath.c_str())) {
    return false;
  }

  const std::string downloadUrl = downloadUrlForBook(book);
  const auto* selection = OPDS_SYNC_SELECTION_STORE.findSelection(serverKey, downloadUrl);
  if (!selection || selection->syncedTargetPath != targetPath || selection->syncedUpdated != book.updated) {
    return false;
  }

  return !book.hasAcquisitionSize || (selection->hasSyncedSize && selection->syncedSize == book.acquisitionSize);
}

void OpdsBookBrowserActivity::markBookSynced(const OpdsEntry& book, const std::string& targetPath) {
  if (book.updated.empty()) {
    return;
  }
  OPDS_SYNC_SELECTION_STORE.markSynced(serverKey, downloadUrlForBook(book), book.updated, book.acquisitionSize,
                                       book.hasAcquisitionSize, targetPath);
}

std::string OpdsBookBrowserActivity::downloadUrlForBook(const OpdsEntry& book) const {
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  return UrlUtils::buildUrl(feedUrl, book.href);
}

std::string OpdsBookBrowserActivity::wifiSignalIndicator() const {
  if (WiFi.status() != WL_CONNECTED) {
    return "[....] no link";
  }

  const int rssi = WiFi.RSSI();
  int dots = 0;
  if (rssi >= -60) {
    dots = 4;
  } else if (rssi >= -67) {
    dots = 3;
  } else if (rssi >= -75) {
    dots = 2;
  } else if (rssi >= -82) {
    dots = 1;
  }

  std::string indicator = "[";
  for (int i = 0; i < 4; ++i) {
    indicator += i < dots ? '*' : '.';
  }
  indicator += "]";
  if (dots >= 4) return indicator + " strong";
  if (dots == 3) return indicator + " good";
  if (dots == 2) return indicator + " weak";
  if (dots == 1) return indicator + " poor";
  return indicator + " bad";
}

std::string OpdsBookBrowserActivity::formatBytes(const size_t bytes) const {
  char buf[32];
  if (bytes >= 1024 * 1024) {
    const size_t mb10 = bytes * 10 / (1024 * 1024);
    snprintf(buf, sizeof(buf), "%zu.%zu MB", mb10 / 10, mb10 % 10);
  } else if (bytes >= 1024) {
    snprintf(buf, sizeof(buf), "%zu KB", (bytes + 1023) / 1024);
  } else {
    snprintf(buf, sizeof(buf), "%zu B", bytes);
  }
  return buf;
}

void OpdsBookBrowserActivity::prepareSyncPreflight() {
  std::vector<const OpdsEntry*> selectedBooks;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK && OPDS_SYNC_SELECTION_STORE.isSelected(serverKey, downloadUrlForBook(entry))) {
      selectedBooks.push_back(&entry);
    }
  }
  if (selectedBooks.empty()) {
    return;
  }

  syncTotal = selectedBooks.size();
  syncCurrent = 0;
  syncPreflightKnownBytes = 0;
  syncPreflightUnknownCount = 0;
  syncPreflightProbeFailures = 0;

  startSyncLog(selectedBooks.size());
  logSync("preflight start signal='%s'", wifiSignalIndicator().c_str());

  state = BrowserState::LOADING;
  statusMessage = "Checking download sizes";
  requestUpdateAndWait();

  for (const auto* book : selectedBooks) {
    const std::string url = downloadUrlForBook(*book);
    const std::string targetPath = buildDownloadPath(server, *book);
    if (isAlreadySynced(*book, targetPath)) {
      logSync("preflight title='%s' skipped cached updated='%s' size=%zu", book->title.c_str(), book->updated.c_str(),
              book->acquisitionSize);
      continue;
    }
    if (book->hasAcquisitionSize) {
      syncPreflightKnownBytes += book->acquisitionSize;
      logSync("preflight title='%s' opdsSize=%zu", book->title.c_str(), book->acquisitionSize);
      continue;
    }
    const auto probe = HttpDownloader::probeContentLength(url, server.username, server.password);
    if (probe.result == HttpDownloader::OK && probe.hasContentLength) {
      syncPreflightKnownBytes += probe.contentLength;
      logSync("preflight title='%s' size=%zu", book->title.c_str(), probe.contentLength);
    } else {
      syncPreflightUnknownCount++;
      if (probe.result != HttpDownloader::OK) {
        syncPreflightProbeFailures++;
      }
      logSync("preflight title='%s' size=unknown result=%s status=%d", book->title.c_str(),
              downloadErrorName(probe.result), probe.httpStatus);
    }
  }

  logSync("preflight summary files=%zu knownBytes=%zu unknown=%zu failures=%zu signal='%s'", syncTotal,
          syncPreflightKnownBytes, syncPreflightUnknownCount, syncPreflightProbeFailures, wifiSignalIndicator().c_str());
  state = BrowserState::SYNC_PREFLIGHT;
  requestUpdate();
}

void OpdsBookBrowserActivity::syncSelectedBooks() {
  std::vector<const OpdsEntry*> selectedBooks;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK && OPDS_SYNC_SELECTION_STORE.isSelected(serverKey, downloadUrlForBook(entry))) {
      selectedBooks.push_back(&entry);
    }
  }
  if (selectedBooks.empty()) {
    return;
  }

  Storage.mkdir("/.crosspoint");
  if (!syncLogOpen) {
    startSyncLog(selectedBooks.size());
  }
  logSync("download confirmed signal='%s'", wifiSignalIndicator().c_str());
  logSync("prepare temp root '%s'", OPDS_SYNC_TMP_DIR);
  Storage.mkdir("/.crosspoint/tmp");
  if (Storage.exists(OPDS_SYNC_TMP_DIR)) {
    logSync("remove existing temp dir '%s'", OPDS_SYNC_TMP_DIR);
    Storage.removeDir(OPDS_SYNC_TMP_DIR);
  }
  const bool tmpReady = Storage.mkdir(OPDS_SYNC_TMP_DIR);
  logSync("create temp dir result=%s", tmpReady ? "ok" : "failed");

  state = BrowserState::DOWNLOADING;
  syncCurrent = 0;
  syncTotal = selectedBooks.size();
  blockingSyncInProgress = true;
  cancelDownload = false;
  bool success = tmpReady;

  for (const auto* book : selectedBooks) {
    if (!success) break;
    syncCurrent++;
    statusMessage = book->title;
    downloadProgress = downloadTotal = 0;
    resetDownloadProgressRender();
    requestUpdate(true);

    const std::string downloadUrl = downloadUrlForBook(*book);
    const std::string tmpPath = joinPath(OPDS_SYNC_TMP_DIR, fnv1aHex(downloadUrl) + ".epub.tmp");
    const std::string targetPath = buildDownloadPath(server, *book);
    logSync("item %zu/%zu title='%s' target='%s'", syncCurrent, syncTotal, book->title.c_str(), targetPath.c_str());
    if (isAlreadySynced(*book, targetPath)) {
      logSync("skip unchanged title='%s' updated='%s' size=%zu", book->title.c_str(), book->updated.c_str(),
              book->acquisitionSize);
      continue;
    }
    Storage.remove(tmpPath.c_str());

    const auto result = HttpDownloader::downloadToFile(
        downloadUrl, tmpPath,
        [this](const size_t downloaded, const size_t total) { updateDownloadProgress(downloaded, total); },
        &cancelDownload, server.username, server.password);
    logSync("download result=%s progress=%zu total=%zu cancel=%s", downloadErrorName(result), downloadProgress,
            downloadTotal, cancelDownload ? "yes" : "no");
    if (result != HttpDownloader::OK) {
      success = false;
      Storage.remove(tmpPath.c_str());
      logSync("failed url='%s' tmp removed='%s'", downloadUrl.c_str(), tmpPath.c_str());
      break;
    }

    if (Storage.exists(targetPath.c_str()) && filesEqual(tmpPath, targetPath)) {
      Storage.remove(tmpPath.c_str());
      markBookSynced(*book, targetPath);
      continue;
    }

    if (Storage.exists(targetPath.c_str()) && !Storage.remove(targetPath.c_str())) {
      success = false;
      Storage.remove(tmpPath.c_str());
      logSync("failed removing existing target='%s', tmp removed='%s'", targetPath.c_str(), tmpPath.c_str());
      break;
    }
    if (!Storage.rename(tmpPath.c_str(), targetPath.c_str())) {
      success = false;
      Storage.remove(tmpPath.c_str());
      logSync("rename tmp='%s' target='%s' failed, tmp removed", tmpPath.c_str(), targetPath.c_str());
      break;
    }
    clearBookCache(targetPath);
    markBookSynced(*book, targetPath);
  }

  if (Storage.exists(OPDS_SYNC_TMP_DIR)) {
    logSync("cleanup temp dir '%s'", OPDS_SYNC_TMP_DIR);
    Storage.removeDir(OPDS_SYNC_TMP_DIR);
  }

  syncCurrent = syncTotal = 0;
  downloadProgress = downloadTotal = 0;
  blockingSyncInProgress = false;
  if (success) {
    state = BrowserState::BROWSING;
    statusMessage = tr(STR_SYNC_COMPLETE);
    logSync("sync complete");
  } else {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_SYNC_FAILED_MSG);
    logSync("sync failed");
  }
  closeSyncLog();
  cancelDownload = false;
  requestUpdate();
}

void OpdsBookBrowserActivity::launchSearch() {
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  navigationHistory.push_back(currentPath);  // <-- add this
  currentPath = url;                         // <-- add this

  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdate(true);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate();
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    state = BrowserState::LOADING;
    statusMessage = tr(STR_LOADING);
    requestUpdate(true);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
