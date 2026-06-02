#pragma once
#include <HalStorage.h>
#include <OpdsParser.h>

#include <string>
#include <utility>
#include <vector>

#include "OpdsServerStore.h"
#include "OpdsSyncSelectionStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    SELECTING_SYNC,
    SYNC_PREFLIGHT,
    DOWNLOADING,
    ERROR,
    SEARCH_INPUT
  };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server)
      : Activity("OpdsBookBrowser", renderer, mappedInput), buttonNavigator(), server(std::move(server)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  std::vector<OpdsEntry> entries;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  bool consumeConfirm = false;
  bool consumeBack = false;  // Added missing member
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  size_t syncCurrent = 0;
  size_t syncTotal = 0;
  size_t syncPreflightKnownBytes = 0;
  size_t syncPreflightUnknownCount = 0;
  size_t syncPreflightProbeFailures = 0;
  bool blockingSyncInProgress = false;
  bool cancelDownload = false;
  size_t lastProgressRenderBytes = 0;
  unsigned long lastProgressRenderMs = 0;
  HalFile syncLogFile;
  bool syncLogOpen = false;
  bool syncCancelLogged = false;
  std::string serverKey;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchFeed(const std::string& path);
  void navigateToEntry(const OpdsEntry& entry);
  void navigateBack();
  void downloadBook(const OpdsEntry& book);
  void toggleSyncSelection(const OpdsEntry& book);
  void prepareSyncPreflight();
  void syncSelectedBooks();
  bool downloadBookToPath(const OpdsEntry& book, const std::string& destPath);
  bool isAlreadySynced(const OpdsEntry& book, const std::string& targetPath) const;
  void markBookSynced(const OpdsEntry& book, const std::string& targetPath);
  void resetDownloadProgressRender();
  void updateDownloadProgress(size_t downloaded, size_t total);
  void startSyncLog(size_t selectedCount);
  void closeSyncLog();
  void logSync(const char* fmt, ...);
  std::string downloadUrlForBook(const OpdsEntry& book) const;
  std::string wifiSignalIndicator() const;
  std::string formatBytes(size_t bytes) const;
  size_t actionRowCount() const;
  size_t syncOverlapCount() const;
  bool hasBookEntries() const;
  std::vector<size_t> bookEntryIndices() const;
  const OpdsEntry* entryForRow(size_t row) const;
  bool isSyncRow(size_t row) const;
  bool isSelectForSyncRow(size_t row) const;
  size_t totalRowCount() const;
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override { return true; }
};
