#include "OtaSourceSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int BASE_ITEMS = 2;
}

int OtaSourceSettingsActivity::getMenuItemCount() const { return isNewSource ? BASE_ITEMS : BASE_ITEMS + 1; }

void OtaSourceSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  isNewSource = (sourceIndex < 0);
  showSaveError = false;

  if (!isNewSource) {
    const auto* source = OTA_SOURCE_STORE.getSource(static_cast<size_t>(sourceIndex));
    if (source) {
      editSource = *source;
    } else {
      isNewSource = true;
      sourceIndex = -1;
    }
  }

  requestUpdate();
}

void OtaSourceSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int menuItems = getMenuItemCount();
  buttonNavigator.onNext([this, menuItems] {
    selectedIndex = (selectedIndex + 1) % menuItems;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this, menuItems] {
    selectedIndex = (selectedIndex + menuItems - 1) % menuItems;
    requestUpdate();
  });
}

bool OtaSourceSettingsActivity::saveSource() {
  bool success = false;

  if (isNewSource) {
    success = OTA_SOURCE_STORE.addSource(editSource);
    if (success) {
      isNewSource = false;
      sourceIndex = static_cast<int>(OTA_SOURCE_STORE.getCount()) - 1;
    } else {
      LOG_ERR("OTA", "Failed to add OTA source");
    }
  } else {
    success = OTA_SOURCE_STORE.updateSource(static_cast<size_t>(sourceIndex), editSource);
    if (!success) {
      LOG_ERR("OTA", "Failed to update OTA source at index %d", sourceIndex);
    }
  }

  showSaveError = !success;
  if (showSaveError) {
    requestUpdate();
  }
  return success;
}

void OtaSourceSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editSource.name = kb.text;
        saveSource();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_OTA_SOURCE_NAME),
                                                                   editSource.name, 63, InputType::Text),
                           handler);
  } else if (selectedIndex == 1) {
    const std::string prefillUrl = editSource.url.empty() ? "http://" : editSource.url;
    auto handler = [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        const auto& kb = std::get<KeyboardResult>(result.data);
        editSource.url = (kb.text == "http://" || kb.text == "https://") ? "" : kb.text;
        saveSource();
        requestUpdate();
      }
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_FIRMWARE_URL),
                                                                   prefillUrl, 255, InputType::Url),
                           handler);
  } else if (selectedIndex == 2 && !isNewSource) {
    if (!OTA_SOURCE_STORE.removeSource(static_cast<size_t>(sourceIndex))) {
      LOG_ERR("OTA", "Failed to remove OTA source at index %d", sourceIndex);
      showSaveError = true;
      requestUpdate();
      return;
    }
    finish();
  }
}

void OtaSourceSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const char* header = isNewSource ? tr(STR_ADD_OTA_SOURCE) : tr(STR_OTA_SOURCE);
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, header);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int menuItems = getMenuItemCount();
  const StrId fieldNames[] = {StrId::STR_OTA_SOURCE_NAME, StrId::STR_FIRMWARE_URL};

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, menuItems, static_cast<int>(selectedIndex),
      [this, &fieldNames](int index) {
        if (index < BASE_ITEMS) {
          return std::string(I18N.get(fieldNames[index]));
        }
        return std::string(tr(STR_DELETE_OTA_SOURCE));
      },
      nullptr, nullptr,
      [this](int index) {
        if (index == 0) {
          return editSource.name.empty() ? std::string(tr(STR_NOT_SET)) : editSource.name;
        } else if (index == 1) {
          return editSource.url.empty() ? std::string(tr(STR_NOT_SET)) : editSource.url;
        }
        return std::string("");
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showSaveError) {
    GUI.drawPopup(renderer, tr(STR_ERROR_GENERAL_FAILURE));
  }

  renderer.displayBuffer();
}
