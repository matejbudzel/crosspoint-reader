#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen before esp_http_client (which includes lwip). Pin this
// order; clang-format would otherwise sort the local header last and break the
// build.
#include "HttpDownloader.h"
#include "FirmwareFlasher.h"
#include <HalStorage.h>
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <string>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/crosspoint-reader/crosspoint-reader/releases/latest";
constexpr char OTA_TMP_DIR[] = "/.crosspoint/tmp/ota";
constexpr char OTA_TMP_BIN[] = "/.crosspoint/tmp/ota/firmware.bin";

esp_err_t http_client_set_header_cb(esp_http_client_handle_t http_client) {
  return esp_http_client_set_header(http_client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
}

struct DirectFlashProgress {
  OtaUpdater* updater;
  OtaUpdater::ProgressCallback callback;
  void* callbackCtx;
};
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);
  directUrlMode = false;

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  int currentMajor, currentMinor, currentPatch;
  int latestMajor, latestMinor, latestPatch;

  const auto currentVersion = CROSSPOINT_VERSION;

  // semantic version check (only match on 3 segments)
  sscanf(latestVersion.c_str(), "%d.%d.%d", &latestMajor, &latestMinor, &latestPatch);
  sscanf(currentVersion, "%d.%d.%d", &currentMajor, &currentMinor, &currentPatch);

  /*
   * Compare major versions.
   * If they differ, return true if latest major version greater than current major version
   * otherwise return false.
   */
  if (latestMajor != currentMajor) return latestMajor > currentMajor;

  /*
   * Compare minor versions.
   * If they differ, return true if latest minor version greater than current minor version
   * otherwise return false.
   */
  if (latestMinor != currentMinor) return latestMinor > currentMinor;

  /*
   * Check patch versions.
   */
  if (latestPatch != currentPatch) return latestPatch > currentPatch;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(currentVersion, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

void OtaUpdater::useDirectUrl(const std::string& url, const std::string& name) {
  directUrlMode = true;
  updateAvailable = true;
  otaUrl = url;
  latestVersion = name.empty() ? url : name;
  otaSize = 0;
  processedSize = 0;
  totalSize = 0;
  phase = Phase::Idle;
}

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!directUrlMode && !isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  if (directUrlMode) {
    Storage.mkdir("/.crosspoint");
    Storage.mkdir("/.crosspoint/tmp");
    if (Storage.exists(OTA_TMP_DIR)) {
      Storage.removeDir(OTA_TMP_DIR);
    }
    Storage.mkdir(OTA_TMP_DIR);

    processedSize = 0;
    totalSize = 0;
    phase = Phase::Downloading;
    esp_wifi_set_ps(WIFI_PS_NONE);
    const auto downloadResult = HttpDownloader::downloadToFile(
        otaUrl, OTA_TMP_BIN,
        [this, onProgress, ctx](const size_t downloaded, const size_t total) {
          processedSize = downloaded;
          totalSize = total;
          if (onProgress) {
            onProgress(ctx);
          }
        },
        nullptr);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    if (downloadResult != HttpDownloader::OK) {
      Storage.remove(OTA_TMP_BIN);
      Storage.removeDir(OTA_TMP_DIR);
      return HTTP_ERROR;
    }

    const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
    if (!dest) {
      Storage.remove(OTA_TMP_BIN);
      Storage.removeDir(OTA_TMP_DIR);
      return INTERNAL_UPDATE_ERROR;
    }

    phase = Phase::Validating;
    if (onProgress) {
      onProgress(ctx);
    }
    const auto validationResult = firmware_flash::validateImageFile(OTA_TMP_BIN, dest->size);
    if (validationResult != firmware_flash::Result::OK) {
      LOG_ERR("OTA", "staged firmware validation failed: %s", firmware_flash::resultName(validationResult));
      Storage.remove(OTA_TMP_BIN);
      Storage.removeDir(OTA_TMP_DIR);
      return validationResult == firmware_flash::Result::OOM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
    }

    processedSize = 0;
    {
      HalFile staged;
      if (Storage.openFileForRead("OTA", OTA_TMP_BIN, staged) && staged) {
        totalSize = staged.fileSize();
        staged.close();
      }
    }
    phase = Phase::Updating;
    if (onProgress) {
      onProgress(ctx);
    }
    DirectFlashProgress progress{this, onProgress, ctx};
    const auto flashResult = firmware_flash::flashFromSdPath(
        OTA_TMP_BIN,
        [](const size_t written, const size_t total, void* cbCtx) {
          auto* progress = static_cast<DirectFlashProgress*>(cbCtx);
          progress->updater->processedSize = written;
          progress->updater->totalSize = total;
          if (progress->callback) {
            progress->callback(progress->callbackCtx);
          }
        },
        &progress, true);
    Storage.remove(OTA_TMP_BIN);
    Storage.removeDir(OTA_TMP_DIR);

    if (flashResult != firmware_flash::Result::OK) {
      LOG_ERR("OTA", "staged firmware flash failed: %s", firmware_flash::resultName(flashResult));
      return flashResult == firmware_flash::Result::OOM ? OOM_ERROR : INTERNAL_UPDATE_ERROR;
    }

    LOG_INF("OTA", "Direct URL staged update completed");
    return OK;
  }

  esp_https_ota_handle_t ota_handle = NULL;
  esp_err_t esp_err;

  esp_http_client_config_t client_config = {
      .url = otaUrl.c_str(),
      .timeout_ms = 15000,
      // 4096 holds the github->CDN redirect headers (the 512 default truncates
      // them); TX only carries our GET. Both are contiguous blocks contending
      // with the TLS handshake on a tight internal arena, so keep them minimal.
      .buffer_size = 4096,
      .buffer_size_tx = 1024,
      .skip_cert_common_name_check = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .keep_alive_enable = true,
  };

  esp_https_ota_config_t ota_config = {
      .http_config = &client_config,
      .http_client_init_cb = http_client_set_header_cb,
  };

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);
  phase = Phase::Updating;

  esp_err = esp_https_ota_begin(&ota_config, &ota_handle);
  if (esp_err != ESP_OK) {
    LOG_DBG("OTA", "HTTP OTA Begin Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  int lastReportedPct = -1;
  do {
    esp_err = esp_https_ota_perform(ota_handle);
    processedSize = esp_https_ota_get_image_len_read(ota_handle);
    // Fire the callback only on whole-percent change. Without this it fired
    // every ~100ms perform iteration, waking the render task whose framebuffer
    // work contends with TLS on the same internal arena. E-ink can't repaint
    // faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    } else if (onProgress) {
      onProgress(ctx);
    }
    delay(100);  // TODO: should we replace this with something better?
  } while (esp_err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_perform Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return HTTP_ERROR;
  }

  if (!esp_https_ota_is_complete_data_received(ota_handle)) {
    LOG_ERR("OTA", "esp_https_ota_is_complete_data_received Failed: %s", esp_err_to_name(esp_err));
    esp_https_ota_finish(ota_handle);
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_https_ota_finish(ota_handle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_https_ota_finish Failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}
