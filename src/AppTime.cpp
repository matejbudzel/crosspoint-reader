#include "AppTime.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <time.h>

#include <cstdio>
#include <cstdlib>

namespace {
constexpr const char* LOG_TAG = "TIME";
constexpr const char* BRATISLAVA_TZ = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr time_t MIN_VALID_EPOCH = 1609459200;  // 2021-01-01T00:00:00Z
constexpr int MAX_ATTEMPTS = 50;
constexpr int ATTEMPT_DELAY_MS = 100;
}  // namespace

AppTime& AppTime::getInstance() {
  static AppTime instance;
  return instance;
}

bool AppTime::isKnown() const { return known_; }

uint64_t AppTime::now() const {
  if (!known_) {
    return 0;
  }
  return baseEpoch_ + (millis() - baseMillis_) / 1000;
}

AppTime::SyncResult AppTime::syncFromNetwork() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF(LOG_TAG, "WiFi not connected, cannot sync time");
    return SyncResult::NoWifi;
  }

  LOG_INF(LOG_TAG, "Starting NTP sync");
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTzTime(BRATISLAVA_TZ, "pool.ntp.org", "time.nist.gov");

  for (int i = 0; i < MAX_ATTEMPTS; ++i) {
    const time_t current = time(nullptr);
    if (current >= MIN_VALID_EPOCH) {
      baseEpoch_ = static_cast<uint64_t>(current);
      baseMillis_ = millis();
      known_ = true;
      LOG_INF(LOG_TAG, "Time synced: %llu", static_cast<unsigned long long>(baseEpoch_));
      return SyncResult::Success;
    }
    delay(ATTEMPT_DELAY_MS);
  }

  LOG_ERR(LOG_TAG, "NTP sync timed out");
  return SyncResult::Failed;
}

bool AppTime::formatTime(char* buffer, size_t bufferSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  (void)utcOffsetQuarterHoursBiased;
  if (!known_ || buffer == nullptr || bufferSize < (use12Hour ? 9u : 6u)) {
    return false;
  }

  setenv("TZ", BRATISLAVA_TZ, 1);
  tzset();

  const time_t localTime = static_cast<time_t>(now());
  struct tm timeinfo;
  localtime_r(&localTime, &timeinfo);

  if (use12Hour) {
    const bool pm = timeinfo.tm_hour >= 12;
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) {
      hour12 = 12;
    }
    snprintf(buffer, bufferSize, "%d:%02d %s", hour12, timeinfo.tm_min, pm ? "PM" : "AM");
  } else {
    snprintf(buffer, bufferSize, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  }
  return true;
}

bool AppTime::formatDateAndWeekday(char* buffer, size_t bufferSize, uint8_t utcOffsetQuarterHoursBiased) const {
  (void)utcOffsetQuarterHoursBiased;
  if (!known_ || buffer == nullptr || bufferSize < 16u) {
    return false;
  }

  static constexpr const char* WEEKDAYS[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                             "Thursday", "Friday", "Saturday"};

  setenv("TZ", BRATISLAVA_TZ, 1);
  tzset();

  const time_t localTime = static_cast<time_t>(now());
  struct tm timeinfo;
  localtime_r(&localTime, &timeinfo);
  const int weekday = (timeinfo.tm_wday >= 0 && timeinfo.tm_wday < 7) ? timeinfo.tm_wday : 0;
  snprintf(buffer, bufferSize, "%s, %04d-%02d-%02d", WEEKDAYS[weekday], timeinfo.tm_year + 1900,
           timeinfo.tm_mon + 1, timeinfo.tm_mday);
  return true;
}

std::string AppTime::statusText(uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  char buffer[16];
  if (!formatTime(buffer, sizeof(buffer), utcOffsetQuarterHoursBiased, use12Hour)) {
    return "Not set";
  }
  return buffer;
}
