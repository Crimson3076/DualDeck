#include "settings_menu.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <string>

using namespace dualdeck::client;

namespace {

std::filesystem::path temporarySettingsPath(const std::string& name) {
    auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("dualdeck-settings-menu-test-" + name + "-" + std::to_string(suffix)) / "settings.conf";
}

void removeSettingsDir(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

} // namespace

MDR_TEST(settings_menu_rows_follow_wizard_and_mic_flags) {
    ClientSettings settings;
    SettingsMenu withWizard(settings, "", true);
    SettingsMenu withoutWizard(settings, "", false);

    const auto full = withWizard.items(true);
    MDR_CHECK(full.size() == 10);
    MDR_CHECK(full[6] == "RUN SETUP WIZARD");
    MDR_CHECK(full[7] == "MICROPHONE: SYSTEM DEFAULT");
    MDR_CHECK(full.back() == "BACK");

    const auto minimal = withoutWizard.items(false);
    MDR_CHECK(minimal.size() == 7);
    MDR_CHECK(minimal.back() == "BACK");
}

MDR_TEST(settings_menu_video_quality_saves_and_asks_for_reconnect_once) {
    auto path = temporarySettingsPath("quality");
    ClientSettings settings;
    SettingsMenu menu(settings, path.string(), true);

    MDR_CHECK(menu.handle(MenuAction::Down, false, nullptr) == SettingsMenu::Result::Stay);
    MDR_CHECK(menu.handle(MenuAction::Right, false, nullptr) == SettingsMenu::Result::Stay);
    MDR_CHECK(settings.videoQuality == 40);
    MDR_CHECK(loadClientSettings(path.string()).videoQuality == 40);
    MDR_CHECK(menu.takeReconnectRequest());
    MDR_CHECK(!menu.takeReconnectRequest());

    // Left from LOW goes back to AUTO.
    menu.handle(MenuAction::Left, false, nullptr);
    MDR_CHECK(settings.videoQuality == 0);
    removeSettingsDir(path);
}

MDR_TEST(settings_menu_toggle_does_not_ask_for_reconnect) {
    auto path = temporarySettingsPath("toggle");
    ClientSettings settings;
    SettingsMenu menu(settings, path.string(), true);

    MDR_CHECK(menu.handle(MenuAction::Select, false, nullptr) == SettingsMenu::Result::Stay);
    MDR_CHECK(!settings.autoUpdateOnLaunch);
    MDR_CHECK(!loadClientSettings(path.string()).autoUpdateOnLaunch);
    MDR_CHECK(!menu.takeReconnectRequest());
    removeSettingsDir(path);
}

MDR_TEST(settings_menu_back_wizard_and_wraparound) {
    ClientSettings settings;
    SettingsMenu menu(settings, "", true);

    MDR_CHECK(menu.handle(MenuAction::Back, false, nullptr) == SettingsMenu::Result::Close);

    // Up from the top row wraps to BACK.
    menu.handle(MenuAction::Up, false, nullptr);
    MDR_CHECK(menu.handle(MenuAction::Select, false, nullptr) == SettingsMenu::Result::Close);

    // BACK -> RUN SETUP WIZARD is one row up when the mic rows are hidden.
    menu.handle(MenuAction::Up, false, nullptr);
    MDR_CHECK(menu.handle(MenuAction::Right, false, nullptr) == SettingsMenu::Result::Stay);
    MDR_CHECK(menu.handle(MenuAction::Select, false, nullptr) == SettingsMenu::Result::RunSetupWizard);
}
