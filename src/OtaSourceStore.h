#pragma once

#include <string>
#include <vector>

struct OtaSource {
  std::string name;
  std::string url;
};

class OtaSourceStore;
namespace JsonSettingsIO {
bool saveOtaSources(const OtaSourceStore& store, const char* path);
bool loadOtaSources(OtaSourceStore& store, const char* json);
}  // namespace JsonSettingsIO

class OtaSourceStore {
 private:
  static OtaSourceStore instance;
  std::vector<OtaSource> sources;

  static constexpr size_t MAX_SOURCES = 8;

  OtaSourceStore() = default;

  friend bool JsonSettingsIO::saveOtaSources(const OtaSourceStore&, const char*);
  friend bool JsonSettingsIO::loadOtaSources(OtaSourceStore&, const char*);

 public:
  OtaSourceStore(const OtaSourceStore&) = delete;
  OtaSourceStore& operator=(const OtaSourceStore&) = delete;

  static OtaSourceStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  bool addSource(const OtaSource& source);
  bool updateSource(size_t index, const OtaSource& source);
  bool removeSource(size_t index);

  const std::vector<OtaSource>& getSources() const { return sources; }
  const OtaSource* getSource(size_t index) const;
  size_t getCount() const { return sources.size(); }
};

#define OTA_SOURCE_STORE OtaSourceStore::getInstance()
