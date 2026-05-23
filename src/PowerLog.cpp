#include "PowerLog.h"

#include <Arduino.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <WiFi.h>

#include <cstdlib>
#include <cstring>

#include "CrossPointSettings.h"

PowerLog POWER_LOG;

namespace {
constexpr uint32_t SAMPLE_INTERVAL_MS = 60UL * 1000;
constexpr uint32_t RECENT_SCAN_INTERVAL_MS = 5UL * 60 * 1000;
constexpr uint32_t WEEK_MS = 7UL * 24 * 60 * 60 * 1000;

const char* modeName(PowerLog::Mode mode) { return mode == PowerLog::Mode::SoftSleep ? "soft_sleep" : "active"; }

bool wifiActive() { return WiFi.getMode() != WIFI_MODE_NULL; }

uint32_t parseLeadingMillis(const char* line) {
  char* end = nullptr;
  const unsigned long value = strtoul(line, &end, 10);
  if (end == line || *end != ',') {
    return UINT32_MAX;
  }
  return static_cast<uint32_t>(value);
}
}  // namespace

void PowerLog::begin() {
  initialized_ = true;
  lastTickMs_ = millis();
  lastSampleMs_ = 0;
  if (SETTINGS.powerLogEnabled) {
    ensureHeader();
    appendSample("boot");
  }
}

void PowerLog::loop() {
  if (!initialized_) {
    begin();
  }

  const uint32_t now = millis();
  account(now);

  if (!SETTINGS.powerLogEnabled) {
    return;
  }

  if (lastSampleMs_ == 0 || now - lastSampleMs_ >= SAMPLE_INTERVAL_MS) {
    appendSample();
  }
}

void PowerLog::setMode(Mode mode) {
  const uint32_t now = millis();
  account(now);
  if (mode_ == mode) {
    return;
  }
  mode_ = mode;
  event(mode == Mode::SoftSleep ? "soft_sleep_enter" : "soft_sleep_exit");
}

void PowerLog::setSyncActive(bool active) {
  const uint32_t now = millis();
  account(now);
  if (syncActive_ == active) {
    return;
  }
  syncActive_ = active;
  event(active ? "sync_start" : "sync_end");
}

void PowerLog::event(const char* name) {
  if (!SETTINGS.powerLogEnabled) {
    return;
  }
  appendSample(name);
}

PowerLog::Stats PowerLog::getStats() const {
  Stats result = stats_;
  result.uptimeSeconds = millis() / 1000;
  result.activeSeconds = activeMs_ / 1000;
  result.softSleepSeconds = softSleepMs_ / 1000;
  result.wifiSeconds = wifiMs_ / 1000;
  result.chargerSeconds = chargerMs_ / 1000;
  result.syncSeconds = syncMs_ / 1000;

  const auto telemetry = powerManager.readBatteryTelemetry();
  result.batteryPercent = telemetry.percent;
  result.voltageMv = telemetry.voltageMv;
  result.currentMa = telemetry.currentMa;
  result.hasVoltage = telemetry.hasVoltage;
  result.hasCurrent = telemetry.hasCurrent;
  if (telemetry.hasVoltage && telemetry.hasCurrent) {
    result.powerMw = static_cast<int32_t>(telemetry.voltageMv) * static_cast<int32_t>(telemetry.currentMa) / 1000;
  }

  const uint32_t now = millis();
  if (SETTINGS.powerLogEnabled && (lastRecentScanMs_ == 0 || now - lastRecentScanMs_ >= RECENT_SCAN_INTERVAL_MS)) {
    cachedRecentSampleCount_ = 0;
    FsFile file;
    if (Storage.openFileForRead("PLOG", LOG_FILE, file)) {
      char line[180];
      size_t pos = 0;
      while (file.available()) {
        const int ch = file.read();
        if (ch < 0) {
          break;
        }
        if (ch == '\n' || pos >= sizeof(line) - 1) {
          line[pos] = '\0';
          const uint32_t sampleMs = parseLeadingMillis(line);
          if (sampleMs != UINT32_MAX && sampleMs <= now && now - sampleMs <= WEEK_MS) {
            cachedRecentSampleCount_++;
          }
          pos = 0;
        } else if (ch != '\r') {
          line[pos++] = static_cast<char>(ch);
        }
      }
      if (pos > 0) {
        line[pos] = '\0';
        const uint32_t sampleMs = parseLeadingMillis(line);
        if (sampleMs != UINT32_MAX && sampleMs <= now && now - sampleMs <= WEEK_MS) {
          cachedRecentSampleCount_++;
        }
      }
      file.close();
    }
    lastRecentScanMs_ = now;
  }
  result.recentSampleCount = cachedRecentSampleCount_;
  return result;
}

void PowerLog::account(uint32_t now) {
  if (!initialized_) {
    lastTickMs_ = now;
    initialized_ = true;
    return;
  }

  const uint32_t deltaMs = now - lastTickMs_;
  lastTickMs_ = now;
  if (mode_ == Mode::SoftSleep) {
    softSleepMs_ += deltaMs;
  } else {
    activeMs_ += deltaMs;
  }
  if (wifiActive()) {
    wifiMs_ += deltaMs;
  }
  if (gpio.isUsbConnected()) {
    chargerMs_ += deltaMs;
  }
  if (syncActive_) {
    syncMs_ += deltaMs;
  }
}

void PowerLog::appendSample(const char* eventName) {
  ensureHeader();

  const auto telemetry = powerManager.readBatteryTelemetry();
  const bool charging = gpio.isUsbConnected();
  const bool wifi = wifiActive();
  int32_t powerMw = 0;
  if (telemetry.hasVoltage && telemetry.hasCurrent) {
    powerMw = static_cast<int32_t>(telemetry.voltageMv) * static_cast<int32_t>(telemetry.currentMa) / 1000;
  }

  FsFile file = Storage.open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    return;
  }
  file.print(millis());
  file.print(",");
  file.print(modeName(mode_));
  file.print(",");
  file.print(wifi ? 1 : 0);
  file.print(",");
  file.print(charging ? 1 : 0);
  file.print(",");
  file.print(syncActive_ ? 1 : 0);
  file.print(",");
  file.print(getCpuFrequencyMhz());
  file.print(",");
  file.print(telemetry.percent);
  file.print(",");
  if (telemetry.hasVoltage) file.print(telemetry.voltageMv);
  file.print(",");
  if (telemetry.hasCurrent) file.print(telemetry.currentMa);
  file.print(",");
  if (telemetry.hasVoltage && telemetry.hasCurrent) file.print(powerMw);
  file.print(",");
  if (eventName) file.print(eventName);
  file.println();
  file.close();

  lastSampleMs_ = millis();
  stats_.sampleCount++;
}

void PowerLog::ensureHeader() {
  Storage.mkdir("/.crosspoint");
  if (Storage.exists(LOG_FILE)) {
    return;
  }

  FsFile file = Storage.open(LOG_FILE, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    return;
  }
  file.println("ms,mode,wifi,charger,sync,cpu_mhz,battery_pct,voltage_mv,current_ma,power_mw,event");
  file.close();
}
