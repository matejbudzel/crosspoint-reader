#pragma once

#include <string>

#include "activities/Activity.h"

class TimeSyncActivity final : public Activity {
 public:
  explicit TimeSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TimeSync", renderer, mappedInput) {}

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
    Error,
  };

  State state_ = State::Connecting;
  bool started_ = false;
  unsigned long successAt_ = 0;
  std::string message_;
  std::string detail_;

  void runSync();
};
