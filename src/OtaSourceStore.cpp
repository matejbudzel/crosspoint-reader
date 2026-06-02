#include "OtaSourceStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <cstddef>

OtaSourceStore OtaSourceStore::instance;

namespace {
constexpr char OTA_SOURCES_FILE_JSON[] = "/.crosspoint/ota_sources.json";
}  // namespace

bool OtaSourceStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveOtaSources(*this, OTA_SOURCES_FILE_JSON);
}

bool OtaSourceStore::loadFromFile() {
  sources.clear();
  if (!Storage.exists(OTA_SOURCES_FILE_JSON)) {
    return false;
  }

  String json = Storage.readFile(OTA_SOURCES_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }

  return JsonSettingsIO::loadOtaSources(*this, json.c_str());
}

bool OtaSourceStore::addSource(const OtaSource& source) {
  if (sources.size() >= MAX_SOURCES) {
    LOG_DBG("OTA", "Cannot add more OTA sources, limit of %zu reached", MAX_SOURCES);
    return false;
  }

  sources.push_back(source);
  return saveToFile();
}

bool OtaSourceStore::updateSource(size_t index, const OtaSource& source) {
  if (index >= sources.size()) {
    return false;
  }

  sources[index] = source;
  return saveToFile();
}

bool OtaSourceStore::removeSource(size_t index) {
  if (index >= sources.size()) {
    return false;
  }

  sources.erase(sources.begin() + static_cast<ptrdiff_t>(index));
  return saveToFile();
}

const OtaSource* OtaSourceStore::getSource(size_t index) const {
  if (index >= sources.size()) {
    return nullptr;
  }
  return &sources[index];
}
