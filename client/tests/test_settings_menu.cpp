#include "settings_menu.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <string>

using namespace dualdeck::client;

namespace {

// Under ctest's working directory (the build tree), not a shared
// temp directory.
std::filesystem::path temporarySettingsPath(const std::string& name) {
    auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::current_path() /
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
    MDR_CHECK(full.size() == 11);
    MDR_CHECK(full[7] == "RUN SETUP WIZARD");
    MDR_CHECK(full[8] == "MICROPHONE: SYSTEM DEFAULT");
    MDR_CHECK(full.back() == "BACK");

    const auto minimal = withoutWizard.items(false);
    MDR_CHECK(minimal.size() == 8);
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

MDR_TEST(settings_menu_video_codec_cycles_through_auto) {
    auto path = temporarySettingsPath("codec");
    ClientSettings settings;
    SettingsMenu menu(settings, path.string(), true);
    MDR_CHECK(menu.items(false)[5] == "VIDEO CODEC: AUTO");

    for (int i = 0; i < 5; ++i) menu.handle(MenuAction::Down, false, nullptr);
    menu.handle(MenuAction::Right, false, nullptr);
    MDR_CHECK(menu.items(false)[5] == "VIDEO CODEC: JPEG");
    MDR_CHECK(!loadClientSettings(path.string()).videoCodecAuto);
    menu.handle(MenuAction::Right, false, nullptr);
    MDR_CHECK(menu.items(false)[5] == "VIDEO CODEC: H264");
    menu.handle(MenuAction::Right, false, nullptr);
    MDR_CHECK(menu.items(false)[5] == "VIDEO CODEC: PYROWAVE");
    menu.handle(MenuAction::Right, false, nullptr);
    MDR_CHECK(menu.items(false)[5] == "VIDEO CODEC: AUTO");
    MDR_CHECK(!settings.videoCodecH264Experimental && !settings.videoCodecPyroWaveExperimental);
    // Left from AUTO wraps to PYROWAVE.
    menu.handle(MenuAction::Left, false, nullptr);
    MDR_CHECK(settings.videoCodecPyroWaveExperimental && !settings.videoCodecAuto);
    MDR_CHECK(menu.takeReconnectRequest());
    removeSettingsDir(path);
}

MDR_TEST(settings_menu_stream_fps_cycles_and_asks_for_reconnect) {
    auto path = temporarySettingsPath("stream-fps");
    ClientSettings settings;
    SettingsMenu menu(settings, path.string(), true);
    MDR_CHECK(menu.items(false)[2] == "STREAM FPS: DEFAULT");

    for (int i = 0; i < 2; ++i) menu.handle(MenuAction::Down, false, nullptr);
    menu.handle(MenuAction::Right, false, nullptr);
    MDR_CHECK(menu.items(false)[2] == "STREAM FPS: 30");
    MDR_CHECK(loadClientSettings(path.string()).streamFps == 30);
    MDR_CHECK(menu.takeReconnectRequest());
    // Left from 30 back to DEFAULT, then wraps to 120.
    menu.handle(MenuAction::Left, false, nullptr);
    MDR_CHECK(settings.streamFps == 0);
    menu.handle(MenuAction::Left, false, nullptr);
    MDR_CHECK(settings.streamFps == 120);
    removeSettingsDir(path);
}
