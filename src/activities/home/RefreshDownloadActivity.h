#pragma once

#include <string>

#include "activities/Activity.h"

class RefreshDownloadActivity final : public Activity {
 public:
  explicit RefreshDownloadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RefreshDownload", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::DOWNLOADING || state == State::DONE || state == State::ERROR; }
  bool skipLoopDelay() override { return true; }

 private:
  enum class State { WIFI_SELECTION, DOWNLOADING, DONE, ERROR };

  State state = State::WIFI_SELECTION;
  size_t downloaded = 0;
  size_t total = 0;
  bool cancelRequested = false;
  std::string errorMessage;

  void onWifiSelectionComplete(bool success);
  bool prepareDestination(const std::string& path);
};
