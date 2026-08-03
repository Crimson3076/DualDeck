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

MDR_TEST(client_settings_missing_file_uses_system_default_mic) {
    auto path = temporarySettingsPath("missing-mic");
    ClientSettings settings = loadClientSettings(path.string());
    MDR_CHECK(settings.micDeviceName.empty());
    MDR_CHECK(!settings.micMuted);
}

MDR_TEST(client_settings_round_trip_mic_device_and_mute) {
    auto path = temporarySettingsPath("mic-round-trip");
    ClientSettings settings;
    settings.micDeviceName = "USB Microphone (Blue Yeti)";
    settings.micMuted = true;

    MDR_CHECK(saveClientSettings(path.string(), settings));
    ClientSettings loaded = loadClientSettings(path.string());
    MDR_CHECK(loaded.micDeviceName == "USB Microphone (Blue Yeti)");
    MDR_CHECK(loaded.micMuted);

    settings.micDeviceName = "";
    settings.micMuted = false;
    MDR_CHECK(saveClientSettings(path.string(), settings));
    loaded = loadClientSettings(path.string());
    MDR_CHECK(loaded.micDeviceName.empty());
    MDR_CHECK(!loaded.micMuted);

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

MDR_TEST(client_settings_missing_file_uses_auto_video_quality) {
    auto path = temporarySettingsPath("missing-video-quality");
    ClientSettings settings = loadClientSettings(path.string());
    MDR_CHECK_EQ(settings.videoQuality, 0);
}

MDR_TEST(client_settings_round_trip_video_quality) {
    auto path = temporarySettingsPath("video-quality-round-trip");
    ClientSettings settings;
    settings.videoQuality = 100;

    MDR_CHECK(saveClientSettings(path.string(), settings));
    MDR_CHECK_EQ(loadClientSettings(path.string()).videoQuality, 100);

    settings.videoQuality = 0;
    MDR_CHECK(saveClientSettings(path.string(), settings));
    MDR_CHECK_EQ(loadClientSettings(path.string()).videoQuality, 0);

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

MDR_TEST(client_settings_ignores_out_of_range_video_quality) {
    auto path = temporarySettingsPath("video-quality-out-of-range");
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream out(path);
        out << "video_quality=101\n";
    }

    MDR_CHECK_EQ(loadClientSettings(path.string()).videoQuality, 0);

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}

MDR_TEST(client_settings_missing_file_uses_mirror_host_screen_off_default) {
    auto path = temporarySettingsPath("missing-mirror-host-screen");
    ClientSettings settings = loadClientSettings(path.string());
    MDR_CHECK(!settings.mirrorHostScreen);
}

MDR_TEST(client_settings_round_trip_mirror_host_screen) {
    auto path = temporarySettingsPath("mirror-host-screen-round-trip");
    ClientSettings settings;
    settings.mirrorHostScreen = true;

    MDR_CHECK(saveClientSettings(path.string(), settings));
    MDR_CHECK(loadClientSettings(path.string()).mirrorHostScreen);

    settings.mirrorHostScreen = false;
    MDR_CHECK(saveClientSettings(path.string(), settings));
    MDR_CHECK(!loadClientSettings(path.string()).mirrorHostScreen);

    std::error_code ec;
    std::filesystem::remove_all(path.parent_path(), ec);
}
