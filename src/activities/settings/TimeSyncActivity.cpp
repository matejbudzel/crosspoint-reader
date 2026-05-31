#include "TimeSyncActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "AppTime.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/CrossPointNetworkManager.h"

void TimeSyncActivity::onEnter() {
  Activity::onEnter();
  state_ = State::Connecting;
  started_ = false;
  successAt_ = 0;
  currentJob_ = 0;
  totalJobs_ = 0;
  downloadProgress_ = 0;
  downloadTotal_ = 0;
  message_ = "Connecting WiFi";
  detail_.clear();
  dateDetail_.clear();
  staleJobs_.clear();
  requestUpdate();
}

void TimeSyncActivity::closeNetwork() {
  if (session_.isActive()) {
    session_.disconnect();
    session_.release();
  }
}

void TimeSyncActivity::runSync() {
  session_ = NETWORK_MANAGER.claim("Time", NetworkClaimMode::Foreground);
  if (!session_.isActive()) {
    state_ = State::Error;
    message_ = "Network busy";
    requestUpdate();
    return;
  }

  state_ = State::Connecting;
  message_ = "Connecting WiFi";
  detail_.clear();
  dateDetail_.clear();
  requestUpdateAndWait();

  const NetworkConnectResult connectResult = session_.connectKnownNetwork();
  if (connectResult != NetworkConnectResult::Connected) {
    state_ = State::Error;
    message_ = connectResult == NetworkConnectResult::NoCredentials ? "No WiFi credentials" : "WiFi failed";
    closeNetwork();
    requestUpdate();
    return;
  }

  const std::string ssid = NETWORK_MANAGER.connectedSsid();
  detail_ = ssid.empty() ? "" : "WiFi: " + ssid;
  state_ = State::Syncing;
  message_ = "Fetching time";
  requestUpdateAndWait();

  const AppTime::SyncResult result = APP_TIME.syncFromNetwork();

  if (result != AppTime::SyncResult::Success) {
    state_ = State::Error;
    message_ = result == AppTime::SyncResult::NoWifi ? "WiFi disconnected" : "Time sync failed";
    closeNetwork();
    requestUpdate();
    return;
  }

  char timeBuffer[16];
  if (APP_TIME.formatTime(timeBuffer, sizeof(timeBuffer), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    detail_ = std::string("Current time: ") + timeBuffer;
  } else {
    detail_.clear();
  }
  char dateBuffer[32];
  if (APP_TIME.formatDateAndWeekday(dateBuffer, sizeof(dateBuffer), SETTINGS.clockUtcOffsetQ)) {
    dateDetail_ = dateBuffer;
  } else {
    dateDetail_.clear();
  }
  state_ = State::Success;
  message_ = "Time updated";
  successAt_ = millis();
  if (!bootMode_ || !SETTINGS.checkStaleSyncFilesOnBoot) {
    closeNetwork();
  }
  requestUpdate();
}

void TimeSyncActivity::loadBootStaleJobs() {
  staleJobs_ = AutoSyncActivity::staleJobs();
  if (staleJobs_.empty()) {
    message_ = "No stale sync files";
    detail_.clear();
    dateDetail_.clear();
    closeNetwork();
    state_ = State::Done;
    successAt_ = millis();
    requestUpdate();
    return;
  }

  message_ = "Fetch stale sync files?";
  detail_ = std::to_string(staleJobs_.size()) + " stale";
  state_ = State::StalePrompt;
  requestUpdate();
}

void TimeSyncActivity::fetchBootStaleJobs() {
  state_ = State::Fetching;
  message_ = "Fetching stale files";
  detail_.clear();
  dateDetail_.clear();
  currentJob_ = 0;
  totalJobs_ = staleJobs_.size();
  downloadProgress_ = 0;
  downloadTotal_ = 0;
  requestUpdateAndWait();

  const auto summary = AutoSyncActivity::fetchStaleWithSession(
      session_, [this](const std::string& msg, size_t current, size_t total, size_t downloaded, size_t downloadTotal) {
        message_ = msg;
        currentJob_ = current;
        totalJobs_ = total;
        downloadProgress_ = downloaded;
        downloadTotal_ = downloadTotal;
        requestUpdate(true);
      });

  closeNetwork();
  message_ = summary.message;
  char counts[48];
  snprintf(counts, sizeof(counts), "%lu fetched, %lu failed", static_cast<unsigned long>(summary.fetched),
           static_cast<unsigned long>(summary.failed));
  detail_ = counts;
  state_ = State::Done;
  successAt_ = millis();
  requestUpdate();
}

void TimeSyncActivity::loop() {
  if (!started_) {
    started_ = true;
    requestUpdateAndWait();
    runSync();
    return;
  }

  if (state_ == State::Success && bootMode_ && SETTINGS.checkStaleSyncFilesOnBoot && millis() - successAt_ >= 2000) {
    loadBootStaleJobs();
    return;
  }

  if ((state_ == State::Success || state_ == State::Done) && millis() - successAt_ >= 3000) {
    finish();
    return;
  }

  if (state_ == State::StalePrompt) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      fetchBootStaleJobs();
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      closeNetwork();
      finish();
      return;
    }
  }

  if (state_ == State::Error &&
      (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
       mappedInput.wasPressed(MappedInputManager::Button::Confirm))) {
    finish();
  }
}

void TimeSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const int midY = pageHeight / 2;

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Time");

  if (state_ == State::Error) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, message_.c_str(), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, "Press OK to continue");
  } else if (state_ == State::StalePrompt) {
    renderer.drawCenteredText(UI_12_FONT_ID, metrics.topPadding + metrics.headerHeight + 35, message_.c_str(), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, metrics.topPadding + metrics.headerHeight + 60, detail_.c_str());
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int y = metrics.topPadding + metrics.headerHeight + 95;
    const size_t maxNames = std::min<size_t>(staleJobs_.size(), 8);
    for (size_t i = 0; i < maxNames; ++i) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, staleJobs_[i].name.c_str());
      y += lineHeight + 2;
    }
    if (staleJobs_.size() > maxNames) {
      const std::string more = "... +" + std::to_string(staleJobs_.size() - maxNames) + " more";
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, more.c_str());
    }
  } else if (state_ == State::Fetching) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 35, message_.c_str(), true);
    if (totalJobs_ > 0) {
      char progress[32];
      snprintf(progress, sizeof(progress), "Job %lu/%lu", static_cast<unsigned long>(currentJob_),
               static_cast<unsigned long>(totalJobs_));
      renderer.drawCenteredText(UI_10_FONT_ID, midY - 5, progress);
    }
    if (downloadTotal_ > 0) {
      GUI.drawProgressBar(
          renderer,
          Rect{metrics.contentSidePadding, midY + 25, pageWidth - metrics.contentSidePadding * 2,
               metrics.progressBarHeight},
          downloadProgress_, downloadTotal_);
    }
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, message_.c_str(), state_ != State::Success,
                              state_ == State::Success ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (!detail_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, detail_.c_str());
    }
    if (!dateDetail_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 35, dateDetail_.c_str());
    }
  }

  if (state_ == State::Error || state_ == State::StalePrompt) {
    const auto labels =
        state_ == State::StalePrompt ? mappedInput.mapLabels(tr(STR_BACK), "Fetch", "", "")
                                     : mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
