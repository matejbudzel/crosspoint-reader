#include "AutoSyncScheduler.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalStorage.h>

#include <set>
#include <vector>

#include "CrossPointSettings.h"
#include "PowerLog.h"
#include "network/CrossPointNetworkManager.h"
#include "network/HttpDownloader.h"

AutoSyncScheduler AUTO_SYNC_SCHEDULER;

namespace {
constexpr const char* JOBS_FILE = "/.crosspoint/auto-sync.json";
constexpr const char* JOBS_FILE_RELATIVE = ".crosspoint/auto-sync.json";
constexpr const char* LOG_FILE = "/.crosspoint/auto-sync.log";
constexpr const char* MANIFEST_DOWNLOAD_TMP = "/.crosspoint/auto-sync.next.json";
constexpr uint32_t CHECK_INTERVAL_MS = 30UL * 1000;

struct Job {
  std::string name;
  std::string url;
  std::string path;
  uint32_t intervalMinutes = 0;
};

bool isHttpUrl(const std::string& url) { return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0; }

std::string normalizeJobPath(const std::string& path) { return path == JOBS_FILE_RELATIVE ? JOBS_FILE : path; }

bool isManifestPath(const std::string& path) { return path == JOBS_FILE; }

void appendLog(const std::string& line) {
  Storage.mkdir("/.crosspoint");
  FsFile file = Storage.open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    return;
  }
  file.print("[");
  file.print(millis());
  file.print("] ");
  file.println(line.c_str());
  file.close();
}

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

bool validateJob(const Job& job, std::string& error) {
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

bool parseJobs(std::vector<Job>& jobs, std::string& error) {
  jobs.clear();
  const String json = Storage.readFile(JOBS_FILE);
  if (json.isEmpty()) {
    error = "Missing auto-sync.json";
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, json.c_str());
  if (err) {
    error = std::string("JSON error: ") + err.c_str();
    return false;
  }

  if ((doc["version"] | 0) != 1) {
    error = "Unsupported job file version";
    return false;
  }

  JsonArray arr = doc["jobs"].as<JsonArray>();
  if (arr.isNull()) {
    error = "Missing jobs array";
    return false;
  }

  std::set<std::string> seenPaths;
  for (JsonObject item : arr) {
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

    std::string jobError;
    if (!validateJob(job, jobError)) {
      appendLog("Scheduled sync skipped " + job.path + ": " + jobError);
      continue;
    }
    jobs.push_back(job);
  }

  return true;
}

bool validateManifestFile(const std::string& path, std::string& error) {
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
  if ((doc["version"] | 0) != 1 || doc["jobs"].as<JsonArray>().isNull()) {
    error = "Manifest shape";
    return false;
  }

  std::set<std::string> seenPaths;
  for (JsonObject item : doc["jobs"].as<JsonArray>()) {
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

std::string displayName(const Job& job) {
  if (!job.name.empty()) return job.name;
  const size_t slash = job.path.rfind('/');
  if (slash != std::string::npos && slash + 1 < job.path.size()) return job.path.substr(slash + 1);
  return job.path.empty() ? job.url : job.path;
}

bool fetchJob(const Job& job) {
  std::string error;
  if (!validateJob(job, error)) {
    appendLog("Scheduled sync skipped " + job.path + ": " + error);
    return false;
  }
  if (!ensureParentDirectory(job.path)) {
    appendLog("Scheduled sync directory failed for " + job.path);
    return false;
  }

  const bool updatesManifest = isManifestPath(job.path);
  const std::string tempPath = updatesManifest ? MANIFEST_DOWNLOAD_TMP : job.path + ".part";
  Storage.remove(tempPath.c_str());

  const auto result = HttpDownloader::downloadToFile(job.url, tempPath, nullptr, nullptr);
  if (result != HttpDownloader::OK) {
    Storage.remove(tempPath.c_str());
    appendLog("Scheduled sync download failed for " + job.url);
    return false;
  }

  if (updatesManifest && !validateManifestFile(tempPath, error)) {
    Storage.remove(tempPath.c_str());
    appendLog("Scheduled manifest rejected for " + job.url + ": " + error);
    return false;
  }

  if (Storage.exists(job.path.c_str())) {
    Storage.remove(job.path.c_str());
  }
  if (!Storage.rename(tempPath.c_str(), job.path.c_str())) {
    Storage.remove(tempPath.c_str());
    appendLog("Scheduled sync replace failed for " + job.path);
    return false;
  }

  appendLog("Scheduled fetched " + displayName(job) + ": " + job.url + " -> " + job.path);
  return true;
}
}  // namespace

void AutoSyncScheduler::loop(bool softSleepActive) {
  if (!softSleepActive || !SETTINGS.autoSyncScheduled || !SETTINGS.softSleepEnabled) {
    return;
  }

  const uint32_t now = millis();
  if (lastCheckMs_ != 0 && now - lastCheckMs_ < CHECK_INTERVAL_MS) {
    return;
  }
  lastCheckMs_ = now;
  runDueJobs();
}

void AutoSyncScheduler::runDueJobs() {
  std::vector<Job> jobs;
  std::string error;
  if (!parseJobs(jobs, error)) {
    appendLog("Scheduled sync unavailable: " + error);
    return;
  }

  std::vector<Job> due;
  const uint32_t now = millis();
  for (const Job& job : jobs) {
    if (job.intervalMinutes == 0) {
      continue;
    }
    const uint32_t intervalMs = job.intervalMinutes * 60UL * 1000;
    auto it = lastRunMs_.find(job.path);
    if (it == lastRunMs_.end() || now - it->second >= intervalMs) {
      due.push_back(job);
    }
  }

  if (due.empty()) {
    return;
  }

  NetworkSession session = NETWORK_MANAGER.claim("AutoSyncScheduled", NetworkClaimMode::Background);
  if (!session.isActive()) {
    return;
  }

  const NetworkConnectResult connectResult = session.connectKnownNetwork();
  if (connectResult != NetworkConnectResult::Connected) {
    appendLog("Scheduled sync WiFi failed");
    session.disconnect();
    return;
  }

  POWER_LOG.setSyncActive(true);
  for (const Job& job : due) {
    if (fetchJob(job)) {
      lastRunMs_[job.path] = millis();
    }
  }
  POWER_LOG.setSyncActive(false);

  session.disconnect();
}
