#include "OpdsSyncSelectionStore.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>

#include <algorithm>

OpdsSyncSelectionStore OpdsSyncSelectionStore::instance;

namespace {
constexpr char OPDS_SYNC_SELECTION_FILE_JSON[] = "/.crosspoint/opds_sync.json";
}

bool OpdsSyncSelectionStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  return JsonSettingsIO::saveOpdsSyncSelections(*this, OPDS_SYNC_SELECTION_FILE_JSON);
}

bool OpdsSyncSelectionStore::loadFromFile() {
  selections.clear();
  if (!Storage.exists(OPDS_SYNC_SELECTION_FILE_JSON)) {
    return false;
  }

  String json = Storage.readFile(OPDS_SYNC_SELECTION_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }

  return JsonSettingsIO::loadOpdsSyncSelections(*this, json.c_str());
}

bool OpdsSyncSelectionStore::isSelected(const std::string& serverKey, const std::string& href) const {
  return findSelection(serverKey, href) != nullptr;
}

const OpdsSyncSelection* OpdsSyncSelectionStore::findSelection(const std::string& serverKey,
                                                               const std::string& href) const {
  auto it = std::find_if(selections.begin(), selections.end(), [&](const OpdsSyncSelection& selection) {
    return selection.serverKey == serverKey && selection.href == href;
  });
  return it == selections.end() ? nullptr : &*it;
}

bool OpdsSyncSelectionStore::toggleSelection(const OpdsSyncSelection& selection) {
  auto it = std::find_if(selections.begin(), selections.end(), [&](const OpdsSyncSelection& existing) {
    return existing.serverKey == selection.serverKey && existing.href == selection.href;
  });

  if (it != selections.end()) {
    selections.erase(it);
  } else {
    selections.push_back(selection);
  }
  return saveToFile();
}

bool OpdsSyncSelectionStore::markSynced(const std::string& serverKey, const std::string& href,
                                        const std::string& updated, const size_t size, const bool hasSize,
                                        const std::string& targetPath) {
  auto it = std::find_if(selections.begin(), selections.end(), [&](const OpdsSyncSelection& existing) {
    return existing.serverKey == serverKey && existing.href == href;
  });

  if (it == selections.end()) {
    return false;
  }

  it->syncedUpdated = updated;
  it->syncedSize = size;
  it->hasSyncedSize = hasSize;
  it->syncedTargetPath = targetPath;
  return saveToFile();
}

std::vector<OpdsSyncSelection> OpdsSyncSelectionStore::getSelectionsForServer(const std::string& serverKey) const {
  std::vector<OpdsSyncSelection> result;
  for (const auto& selection : selections) {
    if (selection.serverKey == serverKey) {
      result.push_back(selection);
    }
  }
  return result;
}
