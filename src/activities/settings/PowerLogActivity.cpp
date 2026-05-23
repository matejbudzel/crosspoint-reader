#include "PowerLogActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <cstdio>
#include <string>

#include "CrossPointSettings.h"
#include "PowerLog.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int ITEM_ENABLE = 0;
constexpr int ITEM_OPEN_LOG = 1;
constexpr int ITEM_UPTIME = 2;
constexpr int ITEM_OFF = 3;
constexpr int ITEM_ACTIVE = 4;
constexpr int ITEM_SOFT_SLEEP = 5;
constexpr int ITEM_WIFI = 6;
constexpr int ITEM_CHARGER = 7;
constexpr int ITEM_SYNC = 8;
constexpr int ITEM_BATTERY = 9;
constexpr int ITEM_VOLTAGE = 10;
constexpr int ITEM_CURRENT = 11;
constexpr int ITEM_POWER = 12;
constexpr int ITEM_RECENT = 13;
constexpr int ITEM_COUNT = 14;
constexpr const char* LOG_VIEW_FILE = "/.crosspoint/power-log.txt";

std::string formatDuration(uint32_t seconds) {
  const uint32_t days = seconds / 86400;
  seconds %= 86400;
  const uint32_t hours = seconds / 3600;
  seconds %= 3600;
  const uint32_t minutes = seconds / 60;
  char buf[32];
  if (days > 0) {
    snprintf(buf, sizeof(buf), "%lud %luh", static_cast<unsigned long>(days), static_cast<unsigned long>(hours));
  } else if (hours > 0) {
    snprintf(buf, sizeof(buf), "%luh %lum", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, sizeof(buf), "%lum", static_cast<unsigned long>(minutes));
  }
  return buf;
}
}  // namespace

void PowerLogActivity::onEnter() {
  Activity::onEnter();
  selectedIndex_ = 0;
  requestUpdate();
}

void PowerLogActivity::toggleEnabled() {
  SETTINGS.powerLogEnabled = !SETTINGS.powerLogEnabled;
  SETTINGS.saveToFile();
  POWER_LOG.event(SETTINGS.powerLogEnabled ? "enabled" : "disabled");
  requestUpdate();
}

void PowerLogActivity::openLog() {
  if (!Storage.exists(PowerLog::LOG_FILE)) {
    requestUpdate();
    return;
  }

  const String content = Storage.readFile(PowerLog::LOG_FILE);
  if (Storage.writeFile(LOG_VIEW_FILE, content)) {
    onSelectBook(LOG_VIEW_FILE);
  }
}

void PowerLogActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.popActivity();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (selectedIndex_ == ITEM_ENABLE) {
      toggleEnabled();
    } else if (selectedIndex_ == ITEM_OPEN_LOG) {
      openLog();
    }
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, ITEM_COUNT);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, ITEM_COUNT);
    requestUpdate();
  });
}

std::string PowerLogActivity::statTitle(int index) const {
  switch (index) {
    case ITEM_ENABLE:
      return "Power log";
    case ITEM_OPEN_LOG:
      return "Open raw log";
    case ITEM_UPTIME:
      return "Time on";
    case ITEM_OFF:
      return "Time off";
    case ITEM_ACTIVE:
      return "Active";
    case ITEM_SOFT_SLEEP:
      return "Soft sleep";
    case ITEM_WIFI:
      return "WiFi on";
    case ITEM_CHARGER:
      return "On charger";
    case ITEM_SYNC:
      return "Auto-sync";
    case ITEM_BATTERY:
      return "Battery";
    case ITEM_VOLTAGE:
      return "Voltage";
    case ITEM_CURRENT:
      return "Current";
    case ITEM_POWER:
      return "Power";
    case ITEM_RECENT:
      return "Recent samples";
    default:
      return "";
  }
}

std::string PowerLogActivity::statValue(int index) const {
  const PowerLog::Stats stats = POWER_LOG.getStats();
  char buf[40];
  switch (index) {
    case ITEM_ENABLE:
      return SETTINGS.powerLogEnabled ? tr(STR_STATE_ON) : tr(STR_STATE_OFF);
    case ITEM_OPEN_LOG:
      return Storage.exists(PowerLog::LOG_FILE) ? "CSV" : "No log";
    case ITEM_UPTIME:
      return formatDuration(stats.uptimeSeconds);
    case ITEM_OFF:
      return "n/a";
    case ITEM_ACTIVE:
      return formatDuration(stats.activeSeconds);
    case ITEM_SOFT_SLEEP:
      return formatDuration(stats.softSleepSeconds);
    case ITEM_WIFI:
      return formatDuration(stats.wifiSeconds);
    case ITEM_CHARGER:
      return formatDuration(stats.chargerSeconds);
    case ITEM_SYNC:
      return formatDuration(stats.syncSeconds);
    case ITEM_BATTERY:
      snprintf(buf, sizeof(buf), "%u%%", stats.batteryPercent);
      return buf;
    case ITEM_VOLTAGE:
      if (!stats.hasVoltage) return "n/a";
      snprintf(buf, sizeof(buf), "%u mV", stats.voltageMv);
      return buf;
    case ITEM_CURRENT:
      if (!stats.hasCurrent) return "n/a";
      snprintf(buf, sizeof(buf), "%d mA", stats.currentMa);
      return buf;
    case ITEM_POWER:
      if (!stats.hasCurrent || !stats.hasVoltage) return "n/a";
      snprintf(buf, sizeof(buf), "%ld mW", static_cast<long>(stats.powerMw));
      return buf;
    case ITEM_RECENT:
      snprintf(buf, sizeof(buf), "%lu / 7d", static_cast<unsigned long>(stats.recentSampleCount));
      return buf;
    default:
      return "";
  }
}

void PowerLogActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_POWER_LOG),
                 SETTINGS.powerLogEnabled ? "Current boot + recent SD samples" : tr(STR_STATE_OFF));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, ITEM_COUNT, selectedIndex_,
      [this](int index) { return statTitle(index); }, nullptr, nullptr, [this](int index) { return statValue(index); },
      true);

  const char* confirmLabel = selectedIndex_ == ITEM_ENABLE ? tr(STR_TOGGLE)
                                                           : selectedIndex_ == ITEM_OPEN_LOG ? tr(STR_OPEN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
