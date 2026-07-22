// Covers the one-time melonDS-Remote -> DualDeck config-dir migration
// added to each defaultXPath() function (device_identity.cpp,
// discovery_store.cpp, client_settings.cpp, wizard_state.cpp): an old
// file under ~/.config/melonds-remote-client/ should be copied forward,
// byte-for-byte, to the new ~/.config/dualdeck-client/ location the
// first time each function is called, without touching the old file.
#include "client_settings.h"
#include "device_identity.h"
#include "discovery_store.h"
#include "test_framework.h"
#include "wizard_state.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace melonds_remote::client;

namespace {

// Sets $HOME to a fresh temp directory for the duration of one test,
// restoring the previous value (or unsetting it) afterward -- these
// defaultXPath() functions read $HOME directly via std::getenv, so
// this is the only way to exercise them in isolation.
class ScopedFakeHome {
public:
    explicit ScopedFakeHome(const std::string& name) {
        auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("dualdeck-migration-test-" + name + "-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);

        if (const char* existing = std::getenv("HOME")) {
            hadPrevious_ = true;
            previous_ = existing;
        }
        setenv("HOME", path_.string().c_str(), 1);
    }

    ~ScopedFakeHome() {
        if (hadPrevious_) {
            setenv("HOME", previous_.c_str(), 1);
        } else {
            unsetenv("HOME");
        }
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedFakeHome(const ScopedFakeHome&) = delete;
    ScopedFakeHome& operator=(const ScopedFakeHome&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
    bool hadPrevious_ = false;
    std::string previous_;
};

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::trunc);
    out << content;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return content;
}

} // namespace

MDR_TEST(device_identity_migrates_forward_from_legacy_path) {
    ScopedFakeHome home("device-identity");
    auto legacyPath = home.path() / ".config/melonds-remote-client/device_id.txt";
    writeFile(legacyPath, "abc123deadbeef\n");

    std::string newPath = defaultDeviceIdentityStorePath();
    MDR_CHECK(newPath == (home.path() / ".config/dualdeck-client/device_id.txt").string());
    MDR_CHECK(std::filesystem::exists(newPath));
    MDR_CHECK(readFile(newPath) == "abc123deadbeef\n");
    // The old file must survive untouched -- it's the fallback if
    // something goes wrong, and this project never deletes identity
    // files out from under a user.
    MDR_CHECK(std::filesystem::exists(legacyPath));
    MDR_CHECK(readFile(legacyPath) == "abc123deadbeef\n");
}

MDR_TEST(device_identity_no_migration_when_new_path_already_exists) {
    ScopedFakeHome home("device-identity-no-clobber");
    auto legacyPath = home.path() / ".config/melonds-remote-client/device_id.txt";
    auto newPath = home.path() / ".config/dualdeck-client/device_id.txt";
    writeFile(legacyPath, "old-identity\n");
    writeFile(newPath, "already-migrated-identity\n");

    std::string resolved = defaultDeviceIdentityStorePath();
    MDR_CHECK(resolved == newPath.string());
    // Must not overwrite an identity that's already there -- migrating
    // "forward" a second time would silently discard whatever identity
    // this install has already been using since the first migration.
    MDR_CHECK(readFile(newPath) == "already-migrated-identity\n");
}

MDR_TEST(device_identity_no_legacy_file_no_migration) {
    ScopedFakeHome home("device-identity-fresh");
    std::string newPath = defaultDeviceIdentityStorePath();
    MDR_CHECK(newPath == (home.path() / ".config/dualdeck-client/device_id.txt").string());
    MDR_CHECK(!std::filesystem::exists(newPath));
}

MDR_TEST(last_host_migrates_forward_from_legacy_path) {
    ScopedFakeHome home("last-host");
    auto legacyPath = home.path() / ".config/melonds-remote-client/last_host.txt";
    writeFile(legacyPath, "192.168.1.50\n");

    std::string newPath = defaultLastHostStorePath();
    MDR_CHECK(std::filesystem::exists(newPath));
    MDR_CHECK(readFile(newPath) == "192.168.1.50\n");
    MDR_CHECK(readFile(legacyPath) == "192.168.1.50\n");
}

MDR_TEST(client_settings_migrates_forward_from_legacy_path) {
    ScopedFakeHome home("client-settings");
    auto legacyPath = home.path() / ".config/melonds-remote-client/settings.conf";
    writeFile(legacyPath, "auto_update_on_launch=0\nmic_muted=1\n");

    std::string newPath = defaultClientSettingsPath();
    MDR_CHECK(std::filesystem::exists(newPath));
    ClientSettings settings = loadClientSettings(newPath);
    MDR_CHECK(!settings.autoUpdateOnLaunch);
    MDR_CHECK(settings.micMuted);
    MDR_CHECK(readFile(legacyPath) == "auto_update_on_launch=0\nmic_muted=1\n");
}

MDR_TEST(wizard_state_migrates_forward_from_legacy_path) {
    ScopedFakeHome home("wizard-state");
    auto legacyPath = home.path() / ".config/melonds-remote-client/setup_complete.txt";
    writeFile(legacyPath, "done\n");

    std::string newPath = defaultWizardStatePath();
    MDR_CHECK(isSetupComplete(newPath));
    MDR_CHECK(std::filesystem::exists(legacyPath));
}
