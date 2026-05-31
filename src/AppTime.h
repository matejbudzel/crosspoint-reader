#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

class AppTime {
 public:
  enum class SyncResult {
    Success,
    NoWifi,
    Failed,
  };

  static AppTime& getInstance();

  bool isKnown() const;
  uint64_t now() const;
  SyncResult syncFromNetwork();
  bool formatTime(char* buffer, size_t bufferSize, uint8_t utcOffsetQuarterHoursBiased = 48,
                  bool use12Hour = false) const;
  bool formatDateAndWeekday(char* buffer, size_t bufferSize, uint8_t utcOffsetQuarterHoursBiased = 48) const;
  std::string statusText(uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

 private:
  AppTime() = default;

  bool known_ = false;
  uint64_t baseEpoch_ = 0;
  unsigned long baseMillis_ = 0;
};

#define APP_TIME AppTime::getInstance()
