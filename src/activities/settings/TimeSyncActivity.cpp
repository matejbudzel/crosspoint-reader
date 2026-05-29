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
  message_ = "Connecting WiFi";
  detail_.clear();
  requestUpdate();
}

void TimeSyncActivity::runSync() {
  NetworkSession session = NETWORK_MANAGER.claim("Time", NetworkClaimMode::Foreground);
  if (!session.isActive()) {
    state_ = State::Error;
    message_ = "Network busy";
    requestUpdate();
    return;
  }

  state_ = State::Connecting;
  message_ = "Connecting WiFi";
  detail_.clear();
  requestUpdateAndWait();

  const NetworkConnectResult connectResult = session.connectKnownNetwork();
  if (connectResult != NetworkConnectResult::Connected) {
    state_ = State::Error;
    message_ = connectResult == NetworkConnectResult::NoCredentials ? "No WiFi credentials" : "WiFi failed";
    session.disconnect();
    requestUpdate();
    return;
  }

  const std::string ssid = NETWORK_MANAGER.connectedSsid();
  detail_ = ssid.empty() ? "" : "WiFi: " + ssid;
  state_ = State::Syncing;
  message_ = "Fetching time";
  requestUpdateAndWait();

  const AppTime::SyncResult result = APP_TIME.syncFromNetwork();
  session.disconnect();

  if (result != AppTime::SyncResult::Success) {
    state_ = State::Error;
    message_ = result == AppTime::SyncResult::NoWifi ? "WiFi disconnected" : "Time sync failed";
    requestUpdate();
    return;
  }

  char timeBuffer[16];
  if (APP_TIME.formatTime(timeBuffer, sizeof(timeBuffer), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    detail_ = std::string("Current time: ") + timeBuffer;
  } else {
    detail_.clear();
  }
  state_ = State::Success;
  message_ = "Time updated";
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

  if (state_ == State::Success && millis() - successAt_ >= 3000) {
    finish();
    return;
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
  } else {
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, message_.c_str(), state_ != State::Success,
                              state_ == State::Success ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (!detail_.empty()) {
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, detail_.c_str());
    }
  }

  if (state_ == State::Error) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
