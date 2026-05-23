#pragma once

#include <cstdint>

class PowerLog {
 public:
  enum class Mode : uint8_t { Active, SoftSleep };

  struct Stats {
    uint32_t uptimeSeconds = 0;
    uint32_t activeSeconds = 0;
    uint32_t softSleepSeconds = 0;
    uint32_t wifiSeconds = 0;
    uint32_t chargerSeconds = 0;
    uint32_t syncSeconds = 0;
    uint32_t sampleCount = 0;
    uint32_t recentSampleCount = 0;
    uint16_t batteryPercent = 0;
    uint16_t voltageMv = 0;
    int16_t currentMa = 0;
    int32_t powerMw = 0;
    bool hasVoltage = false;
    bool hasCurrent = false;
  };

  void begin();
  void loop();
  void setMode(Mode mode);
  void setSyncActive(bool active);
  void event(const char* name);
  Stats getStats() const;

  static constexpr const char* LOG_FILE = "/.crosspoint/power-log.csv";

 private:
  Mode mode_ = Mode::Active;
  bool syncActive_ = false;
  bool initialized_ = false;
  uint32_t lastTickMs_ = 0;
  uint32_t lastSampleMs_ = 0;
  mutable uint32_t lastRecentScanMs_ = 0;
  mutable uint32_t cachedRecentSampleCount_ = 0;
  Stats stats_;
  uint64_t activeMs_ = 0;
  uint64_t softSleepMs_ = 0;
  uint64_t wifiMs_ = 0;
  uint64_t chargerMs_ = 0;
  uint64_t syncMs_ = 0;

  void account(uint32_t now);
  void appendSample(const char* eventName = nullptr);
  void ensureHeader();
};

extern PowerLog POWER_LOG;
