#pragma once

#include <cstdint>
#include <map>
#include <string>

class AutoSyncScheduler {
 public:
  void loop(bool softSleepActive);

 private:
  std::map<std::string, uint32_t> lastRunMs_;
  uint32_t lastCheckMs_ = 0;

  void runDueJobs();
};

extern AutoSyncScheduler AUTO_SYNC_SCHEDULER;
