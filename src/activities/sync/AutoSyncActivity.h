#pragma once

#include <string>
#include <vector>
#include <functional>

#include "activities/Activity.h"
#include "network/HttpDownloader.h"
#include "network/CrossPointNetworkManager.h"
#include "util/ButtonNavigator.h"

class AutoSyncActivity final : public Activity {
  enum class State {
    Loading,
    Ready,
    Fetching,
    Error,
  };

  struct Job {
    std::string name;
    std::string url;
    std::string path;
    uint32_t intervalMinutes = 0;
    uint64_t lastFetched = 0;
    uint64_t lastChanged = 0;
    bool stale = false;
    std::string status;
  };

  ButtonNavigator buttonNavigator;
  State state_ = State::Loading;
  std::vector<Job> jobs_;
  int selectedIndex_ = 0;
  std::string message_;
  std::string connectedSsid_;
  size_t currentJob_ = 0;
  size_t totalJobs_ = 0;
  size_t downloadProgress_ = 0;
  size_t downloadTotal_ = 0;

  void loadJobs();
  bool parseJobsFile(const char* json);
  void loadMetadata();
  void saveMetadata() const;
  void refreshStaleStatus();
  bool isJobStale(const Job& job) const;
  void fetchSelected();
  void fetchStale();
  void fetchAll();
  void openLog();
  bool fetchJob(size_t index, NetworkSession& session);
  bool connectForFetch(NetworkSession& session);
  static bool validateManifestFile(const std::string& path, std::string& error);
  static bool validateJob(const Job& job, std::string& error);
  void appendLog(const std::string& line);
  std::string jobDisplayName(const Job& job) const;
  std::string menuTitle(int index) const;
  std::string menuSubtitle(int index) const;
  std::string menuValue(int index) const;

 public:
  struct StaleJobInfo {
    std::string name;
    std::string path;
  };

  struct FetchSummary {
    size_t fetched = 0;
    size_t failed = 0;
    std::string message;
  };

  using ProgressCallback =
      std::function<void(const std::string& message, size_t currentJob, size_t totalJobs, size_t downloaded,
                         size_t total)>;

  explicit AutoSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Sync", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ == State::Fetching; }

  static bool hasStaleJobs();
  static std::vector<StaleJobInfo> staleJobs();
  static FetchSummary fetchStaleWithSession(NetworkSession& session, const ProgressCallback& progress = nullptr);
};
