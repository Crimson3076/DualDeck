// The approved-devices file holds bearer credentials: it must be owner-only.
#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "host/device_approval_manager.h"
#include "test_framework.h"

using namespace dualdeck::host;

MDR_TEST(approved_devices_file_is_owner_only) {
    auto dir = std::filesystem::temp_directory_path() /
               ("dualdeck-approval-test-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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
