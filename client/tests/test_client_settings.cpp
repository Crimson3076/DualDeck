#include "client_settings.h"
#include "test_framework.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace melonds_remote::client;

namespace {

std::filesystem::path temporarySettingsPath(const std::string& name) {
    auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("melonds-remote-settings-test-" + name + "-" + std::to_string(suffix)) /
           "settings.conf";
}

} // namespace

MDR_TEST(client_settings_missing_file_uses_existing_auto_update_default) {
    auto path = temporarySettingsPath("missing");
    ClientSettings settings = loadClientSettings(path.string());
    MDR_CHECK(settings.autoUpdateOnLaunch);
}

MDR_TEST(client_settings_round_trip_auto_update_toggle) {
    auto path = temporarySettingsPath("round-trip");
    ClientSettings settings;
    settings.autoUpdateOnLaunch = false;

    MDR_CHECK(saveClientSettings(path.string(), settings));
    MDR_CHECK(!loadClientSettings(path.string()).autoUpdateOnLaunch);

    settings.autoUpdateOnLaunch = true;
    MDR_CHECK(saveClientSettings(path.string(), settings));
    MDR_CHECK(loadClientSettings(path.string()).autoUpdateOnLaunch);

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

MDR_TEST(client_settings_ignores_unknown_and_invalid_values) {
    auto path = temporarySettingsPath("invalid");
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path);
        out << "future_setting=1\n";
        out << "auto_update_on_launch=maybe\n";
    }

    MDR_CHECK(loadClientSettings(path.string()).autoUpdateOnLaunch);

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}
