#include "AutoSyncActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <set>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/CrossPointNetworkManager.h"

namespace {
constexpr const char* JOBS_FILE = "/.crosspoint/auto-sync.json";
constexpr const char* JOBS_FILE_RELATIVE = ".crosspoint/auto-sync.json";
constexpr const char* LOG_FILE = "/.crosspoint/auto-sync.log";
constexpr const char* LOG_VIEW_FILE = "/.crosspoint/auto-sync-log.txt";
constexpr const char* MANIFEST_DOWNLOAD_TMP = "/.crosspoint/auto-sync.next.json";
constexpr const char* LOG_TAG = "SYNC";
constexpr int ACTION_FETCH_ALL = 0;
constexpr int ACTION_RELOAD = 1;
constexpr int ACTION_OPEN_LOG = 2;
constexpr int ACTION_COUNT = 3;

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0; }

std::string normalizeJobPath(const std::string& path) {
  if (path == JOBS_FILE_RELATIVE) {
    return JOBS_FILE;
  }
  return path;
}

bool isManifestPath(const std::string& path) { return path == JOBS_FILE; }

bool ensureParentDirectory(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) {
    return true;
  }
  const std::string parent = path.substr(0, slash);
  FsFile existing = Storage.open(parent.c_str());
  if (existing) {
    const bool isDirectory = existing.isDirectory();
    existing.close();
    return isDirectory;
  }
  return Storage.mkdir(parent.c_str());
}

std::string downloadErrorText(HttpDownloader::DownloadError error) {
  switch (error) {
    case HttpDownloader::OK:
      return "OK";
    case HttpDownloader::HTTP_ERROR:
      return "HTTP error";
    case HttpDownloader::FILE_ERROR:
      return "File error";
    case HttpDownloader::ABORTED:
      return "Aborted";
  }
  return "Unknown error";
}
}  // namespace

void AutoSyncActivity::onEnter() {
  Activity::onEnter();
  loadJobs();
  requestUpdate();
}

void AutoSyncActivity::loadJobs() {
  state_ = State::Loading;
  jobs_.clear();
  selectedIndex_ = 0;
  message_.clear();

  const String json = Storage.readFile(JOBS_FILE);
  if (json.isEmpty()) {
    state_ = State::Error;
    message_ = std::string("Missing ") + JOBS_FILE;
    return;
  }

  if (!parseJobsFile(json.c_str())) {
    state_ = State::Error;
    return;
  }

  state_ = State::Ready;
  message_ = jobs_.empty() ? "No jobs in file" : "Ready";
}

bool AutoSyncActivity::parseJobsFile(const char* json) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json);
  if (err) {
    message_ = std::string("JSON error: ") + err.c_str();
    return false;
  }

  const int version = doc["version"] | 0;
  if (version != 1) {
    message_ = "Unsupported job file version";
    return false;
  }

  JsonArray jobs = doc["jobs"].as<JsonArray>();
  if (jobs.isNull()) {
    message_ = "Missing jobs array";
    return false;
  }

  std::set<std::string> seenPaths;
  jobs_.reserve(jobs.size());
  for (JsonObject item : jobs) {
    Job job;
    job.name = item["name"] | "";
    job.url = item["url"] | "";
    job.path = normalizeJobPath(item["path"] | "");
    job.intervalMinutes = item["intervalMinutes"] | 0;
    job.status = "Not fetched";

    if (!job.path.empty()) {
      if (seenPaths.find(job.path) != seenPaths.end()) {
        message_ = "Duplicate target path: " + job.path;
        jobs_.clear();
        return false;
      }
      seenPaths.insert(job.path);
    }

    std::string error;
    if (!validateJob(job, error)) {
      job.status = error;
    }
    jobs_.push_back(job);
  }

  return true;
}

bool AutoSyncActivity::validateJob(const Job& job, std::string& error) const {
  if (!isHttpUrl(job.url)) {
    error = "Invalid URL";
    return false;
  }
  if (job.path.empty() || job.path[0] != '/') {
    error = "Invalid path";
    return false;
  }
  if (job.path.find("..") != std::string::npos) {
    error = "Path contains ..";
    return false;
  }
  if (job.path.rfind("/.crosspoint/auto-sync", 0) == 0 && !isManifestPath(job.path)) {
    error = "Reserved path";
    return false;
  }
  return true;
}

bool AutoSyncActivity::validateManifestFile(const std::string& path, std::string& error) const {
  const String json = Storage.readFile(path.c_str());
  if (json.isEmpty()) {
    error = "Manifest is empty";
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json.c_str());
  if (err) {
    error = std::string("Manifest JSON: ") + err.c_str();
    return false;
  }

  const int version = doc["version"] | 0;
  if (version != 1) {
    error = "Manifest version";
    return false;
  }

  JsonArray jobs = doc["jobs"].as<JsonArray>();
  if (jobs.isNull()) {
    error = "Manifest jobs";
    return false;
  }

  std::set<std::string> seenPaths;
  for (JsonObject item : jobs) {
    Job job;
    job.name = item["name"] | "";
    job.url = item["url"] | "";
    job.path = normalizeJobPath(item["path"] | "");
    job.intervalMinutes = item["intervalMinutes"] | 0;

    if (!job.path.empty()) {
      if (seenPaths.find(job.path) != seenPaths.end()) {
        error = "Duplicate target path: " + job.path;
        return false;
      }
      seenPaths.insert(job.path);
    }

    if (!validateJob(job, error)) {
      return false;
    }
  }

  return true;
}

void AutoSyncActivity::appendLog(const std::string& line) {
  Storage.mkdir("/.crosspoint");
  FsFile file = Storage.open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR(LOG_TAG, "Failed to append log");
    return;
  }
  file.print("[");
  file.print(millis());
  file.print("] ");
  file.println(line.c_str());
  file.close();
}

void AutoSyncActivity::fetchSelected() {
  const int jobIndex = selectedIndex_ - ACTION_COUNT;
  if (jobIndex < 0 || jobIndex >= static_cast<int>(jobs_.size())) {
    return;
  }

  NetworkSession session = NETWORK_MANAGER.claim("AutoSync", NetworkClaimMode::Foreground);
  if (!session.isActive()) {
    jobs_[jobIndex].status = "Network busy";
    message_ = "Network busy";
    appendLog("Network busy for " + jobs_[jobIndex].url);
    requestUpdate();
    return;
  }

  state_ = State::Fetching;
  currentJob_ = 1;
  totalJobs_ = 1;
  if (!connectForFetch(session)) {
    session.disconnect();
    state_ = State::Ready;
    requestUpdate();
    return;
  }

  fetchJob(static_cast<size_t>(jobIndex), session);
  session.disconnect();
  state_ = State::Ready;
  requestUpdate();
}

void AutoSyncActivity::fetchAll() {
  if (jobs_.empty()) {
    message_ = "No jobs in file";
    requestUpdate();
    return;
  }

  NetworkSession session = NETWORK_MANAGER.claim("AutoSync", NetworkClaimMode::Foreground);
  if (!session.isActive()) {
    message_ = "Network busy";
    appendLog("Network busy for fetch all");
    requestUpdate();
    return;
  }

  state_ = State::Fetching;
  currentJob_ = 0;
  totalJobs_ = jobs_.size();
  if (!connectForFetch(session)) {
    session.disconnect();
    state_ = State::Ready;
    requestUpdate();
    return;
  }

  size_t fetched = 0;
  size_t failed = 0;
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (fetchJob(i, session)) {
      fetched++;
    } else {
      failed++;
    }
  }
  char summary[48];
  snprintf(summary, sizeof(summary), "Done: %lu OK, %lu failed", static_cast<unsigned long>(fetched),
           static_cast<unsigned long>(failed));
  session.disconnect();
  message_ = summary;
  state_ = State::Ready;
  requestUpdate();
}

void AutoSyncActivity::openLog() {
  if (!Storage.exists(LOG_FILE)) {
    message_ = "No log file";
    requestUpdate();
    return;
  }

  const String content = Storage.readFile(LOG_FILE);
  if (!Storage.writeFile(LOG_VIEW_FILE, content)) {
    message_ = "Log open failed";
    requestUpdate();
    return;
  }

  onSelectBook(LOG_VIEW_FILE);
}

bool AutoSyncActivity::connectForFetch(NetworkSession& session) {
  connectedSsid_.clear();
  downloadProgress_ = 0;
  downloadTotal_ = 0;
  message_ = "Connecting WiFi";
  requestUpdateAndWait();

  const NetworkConnectResult connectResult = session.connectKnownNetwork();
  if (connectResult != NetworkConnectResult::Connected) {
    message_ = connectResult == NetworkConnectResult::NoCredentials ? "No WiFi credentials" : "WiFi failed";
    appendLog("Connection failed: " + message_);
    return false;
  }

  const std::string ssid = NETWORK_MANAGER.connectedSsid();
  connectedSsid_ = ssid.empty() ? "" : "WiFi: " + ssid;
  message_ = connectedSsid_.empty() ? "WiFi connected" : connectedSsid_;
  requestUpdateAndWait();
  return true;
}

bool AutoSyncActivity::fetchJob(size_t index, NetworkSession& session) {
  if (index >= jobs_.size()) {
    return false;
  }
  if (!session.isActive()) {
    message_ = "Network busy";
    requestUpdate();
    return false;
  }

  Job& job = jobs_[index];
  std::string error;
  if (!validateJob(job, error)) {
    job.status = error;
    message_ = error;
    appendLog("Skipped " + job.path + ": " + error);
    requestUpdate();
    return false;
  }

  state_ = State::Fetching;
  currentJob_ = index + 1;
  totalJobs_ = jobs_.size();

  if (!ensureParentDirectory(job.path)) {
    job.status = "Directory error";
    message_ = job.status;
    appendLog("Failed to create directory for " + job.path);
    requestUpdate();
    return false;
  }

  const bool updatesManifest = isManifestPath(job.path);
  const std::string tempPath = updatesManifest ? MANIFEST_DOWNLOAD_TMP : job.path + ".part";
  Storage.remove(tempPath.c_str());

  job.status = "Downloading";
  message_ = "Downloading " + jobDisplayName(job);
  downloadProgress_ = 0;
  downloadTotal_ = 0;
  requestUpdateAndWait();

  const auto result = HttpDownloader::downloadToFile(
      job.url, tempPath,
      [this](size_t downloaded, size_t total) {
        downloadProgress_ = downloaded;
        downloadTotal_ = total;
        requestUpdate(true);
      },
      nullptr);
  if (result != HttpDownloader::OK) {
    Storage.remove(tempPath.c_str());
    job.status = downloadErrorText(result);
    message_ = job.status;
    appendLog("Download failed for " + job.url + " -> " + job.path + ": " + job.status);
    requestUpdate();
    return false;
  }

  if (updatesManifest) {
    if (!validateManifestFile(tempPath, error)) {
      Storage.remove(tempPath.c_str());
      job.status = error;
      message_ = error;
      appendLog("Manifest update rejected for " + job.url + ": " + error);
      requestUpdate();
      return false;
    }
  }

  if (Storage.exists(job.path.c_str())) {
    Storage.remove(job.path.c_str());
  }
  if (!Storage.rename(tempPath.c_str(), job.path.c_str())) {
    Storage.remove(tempPath.c_str());
    job.status = "Replace failed";
    message_ = job.status;
    appendLog("Replace failed for " + job.path);
    requestUpdate();
    return false;
  }

  job.status = "OK";
  message_ = "Fetched " + jobDisplayName(job);
  downloadProgress_ = downloadTotal_;
  appendLog("Fetched " + jobDisplayName(job) + ": " + job.url + " -> " + job.path);
  requestUpdate();
  return true;
}

void AutoSyncActivity::loop() {
  const int itemCount = ACTION_COUNT + static_cast<int>(jobs_.size());

  if (state_ == State::Fetching) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == ACTION_FETCH_ALL) {
      fetchAll();
    } else if (selectedIndex_ == ACTION_RELOAD) {
      loadJobs();
      requestUpdate();
    } else if (selectedIndex_ == ACTION_OPEN_LOG) {
      openLog();
    } else {
      fetchSelected();
    }
    return;
  }

  buttonNavigator.onNext([this, itemCount] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, itemCount);
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, itemCount] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, itemCount);
    requestUpdate();
  });
}

std::string AutoSyncActivity::menuTitle(int index) const {
  if (index == ACTION_FETCH_ALL) {
    return "Fetch all";
  }
  if (index == ACTION_RELOAD) {
    return "Reload jobs";
  }
  if (index == ACTION_OPEN_LOG) {
    return "Open log";
  }
  const int jobIndex = index - ACTION_COUNT;
  if (jobIndex < 0 || jobIndex >= static_cast<int>(jobs_.size())) {
    return "";
  }
  return jobDisplayName(jobs_[jobIndex]);
}

std::string AutoSyncActivity::menuSubtitle(int index) const {
  if (index == ACTION_FETCH_ALL) {
    return "Run every job from auto-sync.json";
  }
  if (index == ACTION_RELOAD) {
    return JOBS_FILE;
  }
  if (index == ACTION_OPEN_LOG) {
    return LOG_FILE;
  }
  const int jobIndex = index - ACTION_COUNT;
  if (jobIndex < 0 || jobIndex >= static_cast<int>(jobs_.size())) {
    return "";
  }
  const Job& job = jobs_[jobIndex];
  char interval[24];
  snprintf(interval, sizeof(interval), "%lum | ", static_cast<unsigned long>(job.intervalMinutes));
  return std::string(interval) + job.path;
}

std::string AutoSyncActivity::menuValue(int index) const {
  const int jobIndex = index - ACTION_COUNT;
  if (jobIndex < 0 || jobIndex >= static_cast<int>(jobs_.size())) {
    return "";
  }
  return jobs_[jobIndex].status;
}

std::string AutoSyncActivity::jobDisplayName(const Job& job) const {
  if (!job.name.empty()) {
    return job.name;
  }
  if (!job.path.empty()) {
    const size_t slash = job.path.rfind('/');
    if (slash != std::string::npos && slash + 1 < job.path.size()) {
      return job.path.substr(slash + 1);
    }
    return job.path;
  }
  return job.url;
}

void AutoSyncActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int itemCount = ACTION_COUNT + static_cast<int>(jobs_.size());

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Auto Sync",
                 message_.empty() ? nullptr : message_.c_str());

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  if (state_ == State::Loading) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, tr(STR_LOADING));
  } else if (state_ == State::Fetching) {
    char progress[64];
    if (currentJob_ == 0) {
      snprintf(progress, sizeof(progress), "%lu job%s", static_cast<unsigned long>(totalJobs_),
               totalJobs_ == 1 ? "" : "s");
    } else {
      snprintf(progress, sizeof(progress), "Job %lu/%lu", static_cast<unsigned long>(currentJob_),
               static_cast<unsigned long>(totalJobs_));
    }
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int centerY = pageHeight / 2 - lineHeight * 2;
    renderer.drawCenteredText(UI_12_FONT_ID, centerY, message_.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight + metrics.verticalSpacing / 2, progress);
    if (!connectedSsid_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, centerY + lineHeight * 2 + metrics.verticalSpacing,
                                connectedSsid_.c_str());
    }
    if (downloadTotal_ > 0) {
      const int barY = centerY + lineHeight * 3 + metrics.verticalSpacing * 2;
      GUI.drawProgressBar(
          renderer,
          Rect{metrics.contentSidePadding, barY, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
          downloadProgress_, downloadTotal_);
    }
  } else if (jobs_.empty() && state_ == State::Error) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 12, "No jobs loaded", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 12, message_.c_str());
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex_,
        [this](int index) { return menuTitle(index); }, [this](int index) { return menuSubtitle(index); }, nullptr,
        [this](int index) { return menuValue(index); }, false);
  }

  const char* confirmLabel = selectedIndex_ == ACTION_RELOAD ? "Reload" : selectedIndex_ == ACTION_OPEN_LOG ? "Open"
                                                                                                            : "Fetch";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
