#include "AutoSyncActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "AppTime.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/CrossPointNetworkManager.h"

namespace {
constexpr const char* JOBS_FILE = "/.crosspoint/auto-sync.json";
constexpr const char* JOBS_FILE_RELATIVE = ".crosspoint/auto-sync.json";
constexpr const char* LOG_FILE = "/.crosspoint/auto-sync.log";
constexpr const char* LOG_VIEW_FILE = "/.crosspoint/auto-sync-log.txt";
constexpr const char* METADATA_FILE = "/.crosspoint/sync-metadata.json";
constexpr const char* MANIFEST_DOWNLOAD_TMP = "/.crosspoint/auto-sync.next.json";
constexpr const char* LOG_TAG = "SYNC";
constexpr int ACTION_FETCH_STALE = 0;
constexpr int ACTION_FETCH_ALL = 1;
constexpr int ACTION_RELOAD = 2;
constexpr int ACTION_OPEN_LOG = 3;
constexpr int ACTION_COUNT = 4;

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
  HalFile existing = Storage.open(parent.c_str());
  if (existing) {
    const bool isDirectory = existing.isDirectory();
    existing.close();
    return isDirectory;
  }
  return Storage.mkdir(parent.c_str());
}

bool filesEqual(const std::string& leftPath, const std::string& rightPath) {
  HalFile left = Storage.open(leftPath.c_str());
  HalFile right = Storage.open(rightPath.c_str());
  if (!left || !right) {
    if (left) left.close();
    if (right) right.close();
    return false;
  }

  if (left.fileSize64() != right.fileSize64()) {
    left.close();
    right.close();
    return false;
  }

  uint8_t leftBuffer[512];
  uint8_t rightBuffer[512];
  while (left.available() > 0 || right.available() > 0) {
    const int leftRead = left.read(leftBuffer, sizeof(leftBuffer));
    const int rightRead = right.read(rightBuffer, sizeof(rightBuffer));
    if (leftRead != rightRead || leftRead < 0) {
      left.close();
      right.close();
      return false;
    }
    if (leftRead == 0) {
      break;
    }
    if (memcmp(leftBuffer, rightBuffer, static_cast<size_t>(leftRead)) != 0) {
      left.close();
      right.close();
      return false;
    }
  }

  left.close();
  right.close();
  return true;
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

std::string formatInterval(uint32_t totalMinutes) {
  if (totalMinutes == 0) {
    return "0min";
  }

  const uint32_t days = totalMinutes / (24 * 60);
  totalMinutes %= 24 * 60;
  const uint32_t hours = totalMinutes / 60;
  const uint32_t minutes = totalMinutes % 60;

  std::string result;
  if (days > 0) {
    result += std::to_string(days) + "d";
  }
  if (hours > 0) {
    if (!result.empty()) result += ":";
    result += std::to_string(hours) + "h";
  }
  if (minutes > 0) {
    if (!result.empty()) result += ":";
    result += std::to_string(minutes) + "min";
  }
  return result;
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

  loadMetadata();
  refreshStaleStatus();
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

void AutoSyncActivity::loadMetadata() {
  const String json = Storage.readFile(METADATA_FILE);
  if (json.isEmpty()) {
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) {
    return;
  }

  JsonObject files = doc["files"].as<JsonObject>();
  if (files.isNull()) {
    return;
  }

  for (Job& job : jobs_) {
    JsonVariant entry = files[job.path];
    if (entry.is<JsonObject>()) {
      job.lastFetched = entry["lastFetched"] | 0;
      job.lastChanged = entry["lastChanged"] | 0;
    } else {
      job.lastFetched = entry | 0;
      job.lastChanged = job.lastFetched;
    }
  }
}

void AutoSyncActivity::saveMetadata() const {
  Storage.mkdir("/.crosspoint");
  JsonDocument doc;
  doc["version"] = 1;
  JsonObject files = doc["files"].to<JsonObject>();
  for (const Job& job : jobs_) {
    if (job.lastFetched > 0) {
      files[job.path]["lastFetched"] = job.lastFetched;
      if (job.lastChanged > 0) {
        files[job.path]["lastChanged"] = job.lastChanged;
      }
    }
  }

  std::string json;
  serializeJsonPretty(doc, json);
  Storage.writeFile(METADATA_FILE, String(json.c_str()));
}

bool AutoSyncActivity::isJobStale(const Job& job) const {
  if (!APP_TIME.isKnown() || job.intervalMinutes == 0) {
    return false;
  }
  if (job.lastFetched == 0) {
    return true;
  }
  return job.lastFetched + static_cast<uint64_t>(job.intervalMinutes) * 60 < APP_TIME.now();
}

void AutoSyncActivity::refreshStaleStatus() {
  for (Job& job : jobs_) {
    job.stale = isJobStale(job);
    if (job.stale && (job.status == "Not fetched" || job.status == "OK")) {
      job.status = "Stale";
    }
  }
}

bool AutoSyncActivity::validateJob(const Job& job, std::string& error) {
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

bool AutoSyncActivity::validateManifestFile(const std::string& path, std::string& error) {
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
  HalFile file = Storage.open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND);
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

  NetworkSession session = NETWORK_MANAGER.claim("Sync", NetworkClaimMode::Foreground);
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
  saveMetadata();
  refreshStaleStatus();
  session.disconnect();
  state_ = State::Ready;
  requestUpdate();
}

void AutoSyncActivity::fetchStale() {
  if (!APP_TIME.isKnown()) {
    message_ = "Time not set";
    requestUpdate();
    return;
  }

  refreshStaleStatus();
  size_t staleCount = 0;
  for (const Job& job : jobs_) {
    if (job.stale) {
      staleCount++;
    }
  }
  if (staleCount == 0) {
    message_ = "No stale items";
    requestUpdate();
    return;
  }

  NetworkSession session = NETWORK_MANAGER.claim("Sync", NetworkClaimMode::Foreground);
  if (!session.isActive()) {
    message_ = "Network busy";
    appendLog("Network busy for fetch stale");
    requestUpdate();
    return;
  }

  state_ = State::Fetching;
  currentJob_ = 0;
  totalJobs_ = staleCount;
  if (!connectForFetch(session)) {
    session.disconnect();
    state_ = State::Ready;
    requestUpdate();
    return;
  }

  size_t fetched = 0;
  size_t failed = 0;
  for (size_t i = 0; i < jobs_.size(); ++i) {
    if (!jobs_[i].stale) {
      continue;
    }
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
  saveMetadata();
  refreshStaleStatus();
  message_ = summary;
  state_ = State::Ready;
  requestUpdate();
}

void AutoSyncActivity::fetchAll() {
  if (jobs_.empty()) {
    message_ = "No jobs in file";
    requestUpdate();
    return;
  }

  NetworkSession session = NETWORK_MANAGER.claim("Sync", NetworkClaimMode::Foreground);
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
  saveMetadata();
  refreshStaleStatus();
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

  const bool hadExistingFile = Storage.exists(job.path.c_str());
  const bool changed = !hadExistingFile || !filesEqual(job.path, tempPath);
  const uint64_t fetchedAt = APP_TIME.isKnown() ? APP_TIME.now() : 0;

  if (!changed) {
    Storage.remove(tempPath.c_str());
    job.status = "Unchanged";
    if (fetchedAt > 0) {
      job.lastFetched = fetchedAt;
      job.stale = false;
    }
    message_ = "Unchanged " + jobDisplayName(job);
    downloadProgress_ = downloadTotal_;
    appendLog("Unchanged " + jobDisplayName(job) + ": " + job.url + " -> " + job.path);
    requestUpdate();
    return true;
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

  job.status = hadExistingFile ? "Changed" : "OK";
  if (fetchedAt > 0) {
    job.lastFetched = fetchedAt;
    job.lastChanged = fetchedAt;
    job.stale = false;
  }
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
    } else if (selectedIndex_ == ACTION_FETCH_STALE) {
      fetchStale();
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
  if (index == ACTION_FETCH_STALE) {
    return "Fetch stale";
  }
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
  if (index == ACTION_FETCH_STALE) {
    return APP_TIME.isKnown() ? "Run jobs past their refresh interval" : "Time not set";
  }
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
  return formatInterval(job.intervalMinutes) + " | " + job.path;
}

std::string AutoSyncActivity::menuValue(int index) const {
  if (index == ACTION_FETCH_STALE) {
    if (!APP_TIME.isKnown()) {
      return "";
    }
    size_t count = 0;
    for (const Job& job : jobs_) {
      if (job.stale) {
        count++;
      }
    }
    return std::to_string(count);
  }
  const int jobIndex = index - ACTION_COUNT;
  if (jobIndex < 0 || jobIndex >= static_cast<int>(jobs_.size())) {
    return "";
  }
  return jobs_[jobIndex].status;
}

bool AutoSyncActivity::hasStaleJobs() {
  return !staleJobs().empty();
}

std::vector<AutoSyncActivity::StaleJobInfo> AutoSyncActivity::staleJobs() {
  std::vector<StaleJobInfo> result;
  if (!APP_TIME.isKnown()) {
    return result;
  }

  const String json = Storage.readFile(JOBS_FILE);
  if (json.isEmpty()) {
    return result;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) {
    return result;
  }

  JsonDocument metadataDoc;
  std::map<std::string, uint64_t> fetchedByPath;
  const String metadataJson = Storage.readFile(METADATA_FILE);
  if (!metadataJson.isEmpty() && !deserializeJson(metadataDoc, metadataJson.c_str())) {
    JsonObject files = metadataDoc["files"].as<JsonObject>();
    for (JsonPair item : files) {
      JsonVariant entry = item.value();
      fetchedByPath[item.key().c_str()] = entry.is<JsonObject>() ? (entry["lastFetched"] | 0) : (entry | 0);
    }
  }

  JsonArray jobs = doc["jobs"].as<JsonArray>();
  if (jobs.isNull()) {
    return result;
  }

  const uint64_t now = APP_TIME.now();
  for (JsonObject item : jobs) {
    const uint32_t intervalMinutes = item["intervalMinutes"] | 0;
    if (intervalMinutes == 0) {
      continue;
    }
    const std::string path = normalizeJobPath(item["path"] | "");
    const uint64_t lastFetched = fetchedByPath[path];
    if (lastFetched == 0 || lastFetched + static_cast<uint64_t>(intervalMinutes) * 60 < now) {
      const std::string name = item["name"] | "";
      result.push_back({name.empty() ? path : name, path});
    }
  }

  return result;
}

AutoSyncActivity::FetchSummary AutoSyncActivity::fetchStaleWithSession(NetworkSession& session,
                                                                       const ProgressCallback& progress) {
  FetchSummary summary;
  if (!APP_TIME.isKnown()) {
    summary.message = "Time not set";
    return summary;
  }
  if (!session.isActive()) {
    summary.message = "Network busy";
    return summary;
  }

  const String json = Storage.readFile(JOBS_FILE);
  if (json.isEmpty()) {
    summary.message = std::string("Missing ") + JOBS_FILE;
    return summary;
  }

  JsonDocument doc;
  if (deserializeJson(doc, json.c_str())) {
    summary.message = "JSON error";
    return summary;
  }

  JsonArray items = doc["jobs"].as<JsonArray>();
  if (items.isNull()) {
    summary.message = "Missing jobs array";
    return summary;
  }

  JsonDocument metadataDoc;
  std::map<std::string, uint64_t> fetchedByPath;
  std::map<std::string, uint64_t> changedByPath;
  const String metadataJson = Storage.readFile(METADATA_FILE);
  if (!metadataJson.isEmpty() && !deserializeJson(metadataDoc, metadataJson.c_str())) {
    JsonObject files = metadataDoc["files"].as<JsonObject>();
    for (JsonPair item : files) {
      JsonVariant entry = item.value();
      fetchedByPath[item.key().c_str()] = entry.is<JsonObject>() ? (entry["lastFetched"] | 0) : (entry | 0);
      changedByPath[item.key().c_str()] =
          entry.is<JsonObject>() ? (entry["lastChanged"] | 0) : fetchedByPath[item.key().c_str()];
    }
  }

  std::vector<Job> jobs;
  jobs.reserve(items.size());
  std::set<std::string> seenPaths;
  for (JsonObject item : items) {
    Job job;
    job.name = item["name"] | "";
    job.url = item["url"] | "";
    job.path = normalizeJobPath(item["path"] | "");
    job.intervalMinutes = item["intervalMinutes"] | 0;
    job.lastFetched = fetchedByPath[job.path];
    job.lastChanged = changedByPath[job.path];

    if (!job.path.empty() && seenPaths.find(job.path) != seenPaths.end()) {
      continue;
    }
    seenPaths.insert(job.path);

    std::string error;
    if (!validateJob(job, error)) {
      continue;
    }

    if (job.intervalMinutes == 0) {
      continue;
    }
    const uint64_t now = APP_TIME.now();
    if (job.lastFetched == 0 || job.lastFetched + static_cast<uint64_t>(job.intervalMinutes) * 60 < now) {
      jobs.push_back(std::move(job));
    }
  }

  if (jobs.empty()) {
    summary.message = "No stale items";
    return summary;
  }

  for (size_t i = 0; i < jobs.size(); ++i) {
    Job& job = jobs[i];
    const std::string displayName = !job.name.empty() ? job.name : job.path;
    if (progress) progress("Downloading " + displayName, i + 1, jobs.size(), 0, 0);

    if (!ensureParentDirectory(job.path)) {
      summary.failed++;
      continue;
    }

    const bool updatesManifest = isManifestPath(job.path);
    const std::string tempPath = updatesManifest ? MANIFEST_DOWNLOAD_TMP : job.path + ".part";
    Storage.remove(tempPath.c_str());

    const auto downloadResult = HttpDownloader::downloadToFile(
        job.url, tempPath,
        [&progress, &displayName, i, &jobs](size_t downloaded, size_t total) {
          if (progress) progress("Downloading " + displayName, i + 1, jobs.size(), downloaded, total);
        },
        nullptr);

    if (downloadResult != HttpDownloader::OK) {
      Storage.remove(tempPath.c_str());
      summary.failed++;
      continue;
    }

    if (updatesManifest) {
      std::string error;
      if (!validateManifestFile(tempPath, error)) {
        Storage.remove(tempPath.c_str());
        summary.failed++;
        continue;
      }
    }

    const bool hadExistingFile = Storage.exists(job.path.c_str());
    const bool changed = !hadExistingFile || !filesEqual(job.path, tempPath);
    const uint64_t fetchedAt = APP_TIME.now();

    if (changed) {
      if (Storage.exists(job.path.c_str())) {
        Storage.remove(job.path.c_str());
      }
      if (!Storage.rename(tempPath.c_str(), job.path.c_str())) {
        Storage.remove(tempPath.c_str());
        summary.failed++;
        continue;
      }
      job.lastChanged = fetchedAt;
    } else {
      Storage.remove(tempPath.c_str());
    }

    job.lastFetched = fetchedAt;
    summary.fetched++;
  }

  Storage.mkdir("/.crosspoint");
  JsonDocument out;
  out["version"] = 1;
  JsonObject files = out["files"].to<JsonObject>();
  for (const auto& item : fetchedByPath) {
    if (item.second > 0) {
      files[item.first]["lastFetched"] = item.second;
      const uint64_t lastChanged = changedByPath[item.first];
      if (lastChanged > 0) {
        files[item.first]["lastChanged"] = lastChanged;
      }
    }
  }
  for (const Job& job : jobs) {
    if (job.lastFetched > 0) {
      files[job.path]["lastFetched"] = job.lastFetched;
      if (job.lastChanged > 0) {
        files[job.path]["lastChanged"] = job.lastChanged;
      }
    }
  }
  std::string metadata;
  serializeJsonPretty(out, metadata);
  Storage.writeFile(METADATA_FILE, String(metadata.c_str()));

  char buffer[48];
  snprintf(buffer, sizeof(buffer), "Done: %lu OK, %lu failed", static_cast<unsigned long>(summary.fetched),
           static_cast<unsigned long>(summary.failed));
  summary.message = buffer;
  return summary;
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

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Sync",
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

  const char* confirmLabel = selectedIndex_ == ACTION_RELOAD       ? "Reload"
                             : selectedIndex_ == ACTION_OPEN_LOG   ? "Open"
                                                                    : "Fetch";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
