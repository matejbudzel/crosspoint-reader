#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "activities/sync/AutoSyncActivity.h"
#include "network/CrossPointNetworkManager.h"

class TimeSyncActivity final : public Activity {
 public:
  explicit TimeSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool bootMode = false)
      : Activity("TimeSync", renderer, mappedInput), bootMode_(bootMode) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return true; }
  bool preventAutoSleep() override { return state_ == State::Connecting || state_ == State::Syncing; }

 private:
  enum class State {
    Connecting,
    Syncing,
    Success,
    StalePrompt,
    Fetching,
    Done,
    Error,
  };

  State state_ = State::Connecting;
  bool bootMode_ = false;
  bool started_ = false;
  unsigned long successAt_ = 0;
  std::string message_;
  std::string detail_;
  std::string dateDetail_;
  std::vector<AutoSyncActivity::StaleJobInfo> staleJobs_;
  NetworkSession session_;
  size_t currentJob_ = 0;
  size_t totalJobs_ = 0;
  size_t downloadProgress_ = 0;
  size_t downloadTotal_ = 0;

  void runSync();
  void loadBootStaleJobs();
  void fetchBootStaleJobs();
  void closeNetwork();
};
