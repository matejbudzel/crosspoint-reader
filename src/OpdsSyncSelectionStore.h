#pragma once

#include <string>
#include <vector>

struct OpdsSyncSelection {
  std::string serverKey;
  std::string href;
  std::string title;
  std::string author;
  std::string syncedUpdated;
  std::string syncedTargetPath;
  size_t syncedSize = 0;
  bool hasSyncedSize = false;
};

class OpdsSyncSelectionStore;
namespace JsonSettingsIO {
bool saveOpdsSyncSelections(const OpdsSyncSelectionStore& store, const char* path);
bool loadOpdsSyncSelections(OpdsSyncSelectionStore& store, const char* json);
}  // namespace JsonSettingsIO

class OpdsSyncSelectionStore {
 private:
  static OpdsSyncSelectionStore instance;
  std::vector<OpdsSyncSelection> selections;

  OpdsSyncSelectionStore() = default;

  friend bool JsonSettingsIO::saveOpdsSyncSelections(const OpdsSyncSelectionStore&, const char*);
  friend bool JsonSettingsIO::loadOpdsSyncSelections(OpdsSyncSelectionStore&, const char*);

 public:
  OpdsSyncSelectionStore(const OpdsSyncSelectionStore&) = delete;
  OpdsSyncSelectionStore& operator=(const OpdsSyncSelectionStore&) = delete;

  static OpdsSyncSelectionStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  bool isSelected(const std::string& serverKey, const std::string& href) const;
  const OpdsSyncSelection* findSelection(const std::string& serverKey, const std::string& href) const;
  bool toggleSelection(const OpdsSyncSelection& selection);
  bool markSynced(const std::string& serverKey, const std::string& href, const std::string& updated,
                  size_t size, bool hasSize, const std::string& targetPath);
  std::vector<OpdsSyncSelection> getSelectionsForServer(const std::string& serverKey) const;

  const std::vector<OpdsSyncSelection>& getSelections() const { return selections; }
};

#define OPDS_SYNC_SELECTION_STORE OpdsSyncSelectionStore::getInstance()
