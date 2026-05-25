#include "DashboardActivity.h"

#include <I18n.h>

#include "Dashboard.h"
#include "components/UITheme.h"
#include "fontIds.h"

void DashboardActivity::onEnter() {
  Activity::onEnter();
  if (DASHBOARD.hasItems()) {
    DASHBOARD.begin(renderer);
  } else {
    requestUpdate();
  }
}

void DashboardActivity::onExit() {
  DASHBOARD.end();
  Activity::onExit();
}

void DashboardActivity::loop() {
  buttonNavigator.onNext([this] { DASHBOARD.next(renderer); });
  buttonNavigator.onPrevious([this] { DASHBOARD.previous(renderer); });

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::DASHBOARD);
  }
}

void DashboardActivity::render(RenderLock&&) {
  if (DASHBOARD.hasItems()) {
    return;
  }

  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, UITheme::getInstance().getMetrics().topPadding, pageWidth,
                                UITheme::getInstance().getMetrics().headerHeight},
                 "Dashboard");
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, "No dashboard images", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Put .bmp files into /.dashboard on the SD card.");
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 24, "Fallback folders: /.sleep, /sleep.");
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
