// The approved-devices file holds bearer credentials: it must be owner-only.
#include <sys/stat.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "host/device_approval_manager.h"
#include "test_framework.h"

using namespace dualdeck::host;

// mkdtemp creates the directory atomically with mode 0700, so no other
// user can pre-create or write into it. It is made under the working
// directory (the build tree) rather than a shared, world-writable /tmp.
static std::filesystem::path makeTestDir(const char* prefix) {
    std::string tmpl = (std::filesystem::current_path() / (std::string(prefix) + "XXXXXX")).string();
    char* made = ::mkdtemp(tmpl.data());
    return made ? std::filesystem::path(made) : std::filesystem::path();
}

MDR_TEST(approved_devices_file_is_owner_only) {
    auto dir = makeTestDir("dualdeck-approval-test-");
    MDR_CHECK(!dir.empty());
    auto path = (dir / "approved_devices.txt").string();

    {
        DeviceApprovalManager manager(path, std::chrono::seconds(60));
        manager.check("0123456789abcdef0123456789abcdef", "client", "127.0.0.1");
        MDR_CHECK(manager.approve("01234567"));
    }

    struct stat st{};
    MDR_CHECK_EQ(::stat(path.c_str(), &st), 0);
    MDR_CHECK_EQ(st.st_mode & 0777, 0600u);

    DeviceApprovalManager reloaded(path, std::chrono::seconds(60));
    MDR_CHECK(reloaded.isApproved("0123456789abcdef0123456789abcdef"));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MDR_TEST(existing_loose_approved_devices_file_is_tightened_on_load) {
    auto dir = makeTestDir("dualdeck-approval-loose-");
    MDR_CHECK(!dir.empty());
    auto path = (dir / "approved_devices.txt").string();
    {
        std::ofstream out(path);
        out << "0123456789abcdef0123456789abcdef\n";
    }
    ::chmod(path.c_str(), 0640);

    DeviceApprovalManager manager(path, std::chrono::seconds(60));
    MDR_CHECK(manager.isApproved("0123456789abcdef0123456789abcdef"));

    struct stat st{};
    MDR_CHECK_EQ(::stat(path.c_str(), &st), 0);
    MDR_CHECK_EQ(st.st_mode & 0777, 0600u);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

MDR_TEST(approval_handles_are_unique_stable_and_do_not_disclose_credentials) {
    DeviceApprovalManager manager("");
    const std::string first = "aaaaaaaa11111111";
    const std::string second = "aaaaaaaa22222222";
    manager.check(first, "first", "x");
    auto requests = manager.pendingRequests();
    const auto firstHandle = DeviceApprovalManager::displayId(requests, first);
    MDR_CHECK(firstHandle.starts_with("request-"));
    MDR_CHECK(firstHandle.find(first.substr(0, 8)) == std::string::npos);
    manager.check(second, "second", "x");
    manager.check(first, "first", "x");
    requests = manager.pendingRequests();
    const auto secondHandle = DeviceApprovalManager::displayId(requests, second);
    MDR_CHECK_EQ(DeviceApprovalManager::displayId(requests, first), firstHandle);
    MDR_CHECK(firstHandle != secondHandle);
    MDR_CHECK(manager.approve(firstHandle, DeviceApprovalManager::Lookup::RequestHandle));
    MDR_CHECK(manager.isApproved(first));
    MDR_CHECK(!manager.isApproved(second));
    MDR_CHECK(manager.deny(secondHandle, DeviceApprovalManager::Lookup::RequestHandle));
    MDR_CHECK(manager.pendingRequests().empty());
    manager.check(second, "second", "x");
    MDR_CHECK(!manager.approve(secondHandle, DeviceApprovalManager::Lookup::RequestHandle)); // stale handles must not target a new request
}

MDR_TEST(exact_credential_approval_does_not_resolve_another_requests_handle) {
    DeviceApprovalManager manager("");
    manager.check("first-device", "first", "x");
    manager.check("request-1", "second", "x");
    MDR_CHECK(manager.approve("request-1"));
    MDR_CHECK(manager.isApproved("request-1"));
    MDR_CHECK(!manager.isApproved("first-device"));
}

MDR_TEST(exact_credential_denial_does_not_resolve_another_requests_handle) {
    DeviceApprovalManager manager("");
    manager.check("first-device", "first", "x");
    manager.check("request-1", "second", "x");
    MDR_CHECK(manager.deny("request-1"));
    const auto requests = manager.pendingRequests();
    MDR_CHECK_EQ(requests.size(), 1u);
    MDR_CHECK_EQ(requests.front().deviceId, "first-device");
}

MDR_TEST(handle_lookup_does_not_fall_back_to_credentials) {
    DeviceApprovalManager manager("");
    manager.check("first-device", "first", "x");
    manager.check("request-1", "second", "x");
    MDR_CHECK(manager.approve("request-1", DeviceApprovalManager::Lookup::RequestHandle));
    MDR_CHECK(manager.isApproved("first-device"));
    MDR_CHECK(!manager.isApproved("request-1"));
    MDR_CHECK(!manager.deny("request-1", DeviceApprovalManager::Lookup::RequestHandle));
    MDR_CHECK(!manager.approve("request-", DeviceApprovalManager::Lookup::RequestHandle));
    MDR_CHECK(manager.deny("request-2", DeviceApprovalManager::Lookup::RequestHandle));
    MDR_CHECK(manager.pendingRequests().empty());
}
