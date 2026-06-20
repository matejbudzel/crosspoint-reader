#include "DigestSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>
#include <memory>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 3;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_REFRESH_DOWNLOAD_URL, StrId::STR_REFRESH_DOWNLOAD_PATH,
                                     StrId::STR_REFRESH_DOWNLOAD_IMAGE_PATH};

struct DigestField {
  char* value;
  size_t maxLen;
  InputType inputType;
};

DigestField fieldForIndex(int index) {
  switch (index) {
    case 0:
      return {SETTINGS.refreshDownloadUrl, sizeof(SETTINGS.refreshDownloadUrl), InputType::Url};
    case 1:
      return {SETTINGS.refreshDownloadPath, sizeof(SETTINGS.refreshDownloadPath), InputType::Text};
    case 2:
      return {SETTINGS.refreshDownloadImagePath, sizeof(SETTINGS.refreshDownloadImagePath), InputType::Text};
    default:
      return {SETTINGS.refreshDownloadUrl, sizeof(SETTINGS.refreshDownloadUrl), InputType::Url};
  }
}
}  // namespace

void DigestSettingsActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  requestUpdate();
}

void DigestSettingsActivity::onExit() { Activity::onExit(); }

void DigestSettingsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void DigestSettingsActivity::handleSelection() {
  const DigestField field = fieldForIndex(selectedIndex);
  const std::string currentValue = field.value;
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, I18N.get(menuNames[selectedIndex]), currentValue,
                                              field.maxLen, field.inputType),
      [this, field](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& kb = std::get<KeyboardResult>(result.data);
          strncpy(field.value, kb.text.c_str(), field.maxLen - 1);
          field.value[field.maxLen - 1] = '\0';
          SETTINGS.saveToFile();
          requestUpdate();
        }
      });
}

void DigestSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DIGEST_SETTINGS));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, MENU_ITEMS, selectedIndex,
      [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr, nullptr,
      [](int index) {
        const DigestField field = fieldForIndex(index);
        return field.value[0] == '\0' ? std::string(tr(STR_NOT_SET)) : std::string(field.value);
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
