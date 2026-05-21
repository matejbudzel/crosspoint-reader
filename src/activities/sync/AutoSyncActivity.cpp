#include "AutoSyncActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/CrossPointNetworkManager.h"

namespace {
constexpr const char* JOBS_FILE = "/.crosspoint/auto-sync.json";
constexpr const char* LOG_FILE = "/.crosspoint/auto-sync.log";
constexpr const char* LOG_TAG = "SYNC";
constexpr int ACTION_FETCH_ALL = 0;
constexpr int ACTION_RELOAD = 1;
constexpr int ACTION_COUNT = 2;

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0; }

bool ensureParentDirectory(const std::string& path) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos || slash == 0) {
    return true;
  }
  return Storage.mkdir(path.substr(0, slash).c_str());
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

  jobs_.reserve(jobs.size());
  for (JsonObject item : jobs) {
    Job job;
    job.url = item["url"] | "";
    job.path = item["path"] | "";
    job.intervalMinutes = item["intervalMinutes"] | 0;
    job.status = "Not fetched";

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
  if (job.path.rfind("/.crosspoint/auto-sync", 0) == 0) {
    error = "Reserved path";
    return false;
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
  fetchJob(static_cast<size_t>(jobIndex));
}

void AutoSyncActivity::fetchAll() {
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (!fetchJob(i)) {
      break;
    }
  }
}

bool AutoSyncActivity::fetchJob(size_t index) {
  if (index >= jobs_.size()) {
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

  NetworkSession session = NETWORK_MANAGER.claim("AutoSync", NetworkClaimMode::Foreground);
  if (!session.isActive()) {
    job.status = "Network busy";
    message_ = "Network busy";
    appendLog("Network busy for " + job.url);
    requestUpdate();
    return false;
  }

  state_ = State::Fetching;
  currentJob_ = index + 1;
  totalJobs_ = jobs_.size();
  job.status = "Connecting";
  message_ = "Connecting";
  requestUpdateAndWait();

  const NetworkConnectResult connectResult = session.connectKnownNetwork();
  if (connectResult != NetworkConnectResult::Connected) {
    session.disconnect();
    job.status = connectResult == NetworkConnectResult::NoCredentials ? "No WiFi credentials" : "WiFi failed";
    message_ = job.status;
    appendLog("Connection failed for " + job.url + ": " + job.status);
    state_ = State::Ready;
    requestUpdate();
    return false;
  }

  if (!ensureParentDirectory(job.path)) {
    session.disconnect();
    job.status = "Directory error";
    message_ = job.status;
    appendLog("Failed to create directory for " + job.path);
    state_ = State::Ready;
    requestUpdate();
    return false;
  }

  const std::string tempPath = job.path + ".part";
  Storage.remove(tempPath.c_str());

  job.status = "Downloading";
  message_ = "Downloading";
  requestUpdateAndWait();

  const auto result = HttpDownloader::downloadToFile(job.url, tempPath);
  if (result != HttpDownloader::OK) {
    session.disconnect();
    Storage.remove(tempPath.c_str());
    job.status = downloadErrorText(result);
    message_ = job.status;
    appendLog("Download failed for " + job.url + " -> " + job.path + ": " + job.status);
    state_ = State::Ready;
    requestUpdate();
    return false;
  }

  if (Storage.exists(job.path.c_str())) {
    Storage.remove(job.path.c_str());
  }
  if (!Storage.rename(tempPath.c_str(), job.path.c_str())) {
    session.disconnect();
    Storage.remove(tempPath.c_str());
    job.status = "Replace failed";
    message_ = job.status;
    appendLog("Replace failed for " + job.path);
    state_ = State::Ready;
    requestUpdate();
    return false;
  }

  session.disconnect();
  job.status = "OK";
  message_ = "Fetched";
  appendLog("Fetched " + job.url + " -> " + job.path);
  state_ = State::Ready;
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
  const int jobIndex = index - ACTION_COUNT;
  if (jobIndex < 0 || jobIndex >= static_cast<int>(jobs_.size())) {
    return "";
  }
  return jobs_[jobIndex].path;
}

std::string AutoSyncActivity::menuSubtitle(int index) const {
  if (index == ACTION_FETCH_ALL) {
    return "Run every job from auto-sync.json";
  }
  if (index == ACTION_RELOAD) {
    return JOBS_FILE;
  }
  const int jobIndex = index - ACTION_COUNT;
  if (jobIndex < 0 || jobIndex >= static_cast<int>(jobs_.size())) {
    return "";
  }
  return jobs_[jobIndex].url + " | " + jobs_[jobIndex].status;
}

std::string AutoSyncActivity::menuValue(int index) const {
  const int jobIndex = index - ACTION_COUNT;
  if (jobIndex < 0 || jobIndex >= static_cast<int>(jobs_.size())) {
    return "";
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "%lum", static_cast<unsigned long>(jobs_[jobIndex].intervalMinutes));
  return buf;
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
    snprintf(progress, sizeof(progress), "Job %lu/%lu", static_cast<unsigned long>(currentJob_),
             static_cast<unsigned long>(totalJobs_));
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 12, message_.c_str());
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 12, progress);
  } else if (jobs_.empty() && state_ == State::Error) {
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 12, "No jobs loaded", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 12, message_.c_str());
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex_,
        [this](int index) { return menuTitle(index); }, [this](int index) { return menuSubtitle(index); }, nullptr,
        [this](int index) { return menuValue(index); }, false);
  }

  const char* confirmLabel = selectedIndex_ == ACTION_RELOAD ? "Reload" : "Fetch";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
