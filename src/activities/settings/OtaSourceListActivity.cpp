#include "OtaSourceListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "OtaSourceSettingsActivity.h"
#include "OtaSourceStore.h"
#include "OtaUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

int OtaSourceListActivity::getItemCount() const {
  const int sourceCount = static_cast<int>(OTA_SOURCE_STORE.getCount());
  return pickerMode ? sourceCount + 1 : sourceCount + 1;
}

void OtaSourceListActivity::onEnter() {
  Activity::onEnter();
  OTA_SOURCE_STORE.loadFromFile();
  selectedIndex = 0;
  requestUpdate();
}

void OtaSourceListActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int itemCount = getItemCount();
  if (itemCount > 0) {
    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });

    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });
  }
}

void OtaSourceListActivity::handleSelection() {
  const auto sourceCount = static_cast<int>(OTA_SOURCE_STORE.getCount());

  if (pickerMode) {
    if (selectedIndex == 0) {
      startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput), [](const ActivityResult&) {});
      return;
    }

    const auto* source = OTA_SOURCE_STORE.getSource(selectedIndex - 1);
    if (source) {
      startActivityForResult(std::make_unique<OtaUpdateActivity>(renderer, mappedInput, *source),
                             [](const ActivityResult&) {});
    }
    return;
  }

  auto resultHandler = [this](const ActivityResult&) {
    OTA_SOURCE_STORE.loadFromFile();
    selectedIndex = 0;
  };

  if (selectedIndex < sourceCount) {
    startActivityForResult(std::make_unique<OtaSourceSettingsActivity>(renderer, mappedInput, selectedIndex),
                           resultHandler);
  } else {
    startActivityForResult(std::make_unique<OtaSourceSettingsActivity>(renderer, mappedInput, -1), resultHandler);
  }
}

void OtaSourceListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 pickerMode ? tr(STR_OTA_SOURCE) : tr(STR_OTA_SOURCES));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int itemCount = getItemCount();
  const auto& sources = OTA_SOURCE_STORE.getSources();
  const auto sourceCount = static_cast<int>(sources.size());

  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, static_cast<int>(selectedIndex),
      [this, &sources, sourceCount](int index) {
        if (pickerMode) {
          if (index == 0) return std::string(tr(STR_OFFICIAL_RELEASE));
          const auto& source = sources[index - 1];
          return source.name.empty() ? source.url : source.name;
        }

        if (index < sourceCount) {
          const auto& source = sources[index];
          return source.name.empty() ? source.url : source.name;
        }
        return std::string(tr(STR_ADD_OTA_SOURCE));
      },
      [this, &sources, sourceCount](int index) {
        if (pickerMode) {
          if (index == 0) return std::string("");
          return sources[index - 1].url;
        }
        if (index < sourceCount && !sources[index].name.empty()) {
          return sources[index].url;
        }
        return std::string("");
      });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
