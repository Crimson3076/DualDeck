#include "host/device_approval_manager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace melonds_remote::host {

DeviceApprovalManager::DeviceApprovalManager(std::string stateFilePath,
                                             std::chrono::seconds pendingRequestTtl)
    : stateFilePath_(std::move(stateFilePath)), pendingRequestTtl_(pendingRequestTtl) {
    std::lock_guard<std::mutex> lock(mutex_);
    loadApprovedLocked();
}

void DeviceApprovalManager::loadApprovedLocked() {
    if (stateFilePath_.empty()) return;

    std::ifstream in(stateFilePath_);
    if (!in.is_open()) return; // fine -- no prior approvals, or first run

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            approvedDevices_.insert(line);
        }
    }
}

void DeviceApprovalManager::persistApprovedLocked(const std::string& deviceId) {
    if (stateFilePath_.empty()) return;

    std::error_code ec;
    std::filesystem::path path(stateFilePath_);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    // Append-only: every previously-approved device keeps working even if
    // the process restarts mid-write of a later line.
    std::ofstream out(stateFilePath_, std::ios::app);
    if (out.is_open()) {
        out << deviceId << '\n';
    }
}

void DeviceApprovalManager::evictStaleLocked() {
    auto now = std::chrono::steady_clock::now();
    bool changed = false;
    for (auto it = pendingOrder_.begin(); it != pendingOrder_.end();) {
        auto entryIt = pending_.find(*it);
        if (entryIt == pending_.end() || now - entryIt->second.lastSeen > pendingRequestTtl_) {
            pending_.erase(*it);
            it = pendingOrder_.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed) {
        notifyChangedLocked();
    }
}

void DeviceApprovalManager::notifyChangedLocked() {
    if (!onPendingRequestsChanged_) return;

    std::vector<PendingRequest> requests;
    requests.reserve(pendingOrder_.size());
    for (const auto& id : pendingOrder_) {
        const auto& entry = pending_.at(id);
        requests.push_back(PendingRequest{id, entry.clientName, entry.address});
    }
    onPendingRequestsChanged_(std::move(requests));
}

std::string DeviceApprovalManager::resolvePendingLocked(const std::string& idOrPrefix) const {
    if (idOrPrefix.empty()) return {};
    if (pending_.count(idOrPrefix) > 0) return idOrPrefix;

    std::string match;
    for (const auto& id : pendingOrder_) {
        if (id.compare(0, idOrPrefix.size(), idOrPrefix) == 0) {
            if (!match.empty()) return {}; // ambiguous: more than one match
            match = id;
        }
    }
    return match;
}

DeviceApprovalManager::CheckResult DeviceApprovalManager::check(const std::string& deviceId,
                                                                 const std::string& clientName,
                                                                 const std::string& address) {
    std::lock_guard<std::mutex> lock(mutex_);
    evictStaleLocked();

    if (!deviceId.empty() && approvedDevices_.count(deviceId) > 0) {
        return CheckResult::Approved;
    }

    if (!deviceId.empty()) {
        bool isNew = pending_.count(deviceId) == 0;
        pending_[deviceId] = PendingEntry{clientName, address, std::chrono::steady_clock::now()};
        if (isNew) {
            pendingOrder_.push_back(deviceId);
            notifyChangedLocked();
        }
    }

    return CheckResult::Pending;
}

bool DeviceApprovalManager::approve(const std::string& deviceIdOrPrefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string deviceId = resolvePendingLocked(deviceIdOrPrefix);
    if (deviceId.empty()) return false;

    approvedDevices_.insert(deviceId);
    persistApprovedLocked(deviceId);
    pending_.erase(deviceId);
    pendingOrder_.erase(std::remove(pendingOrder_.begin(), pendingOrder_.end(), deviceId),
                         pendingOrder_.end());
    notifyChangedLocked();
    return true;
}

bool DeviceApprovalManager::deny(const std::string& deviceIdOrPrefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string deviceId = resolvePendingLocked(deviceIdOrPrefix);
    if (deviceId.empty()) return false;

    pending_.erase(deviceId);
    pendingOrder_.erase(std::remove(pendingOrder_.begin(), pendingOrder_.end(), deviceId),
                         pendingOrder_.end());
    notifyChangedLocked();
    return true;
}

std::vector<DeviceApprovalManager::PendingRequest> DeviceApprovalManager::pendingRequests() {
    std::lock_guard<std::mutex> lock(mutex_);
    evictStaleLocked();

    std::vector<PendingRequest> requests;
    requests.reserve(pendingOrder_.size());
    for (const auto& id : pendingOrder_) {
        const auto& entry = pending_.at(id);
        requests.push_back(PendingRequest{id, entry.clientName, entry.address});
    }
    return requests;
}

void DeviceApprovalManager::evictStale() {
    std::lock_guard<std::mutex> lock(mutex_);
    evictStaleLocked();
}

void DeviceApprovalManager::setOnPendingRequestsChanged(
    std::function<void(std::vector<PendingRequest>)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    onPendingRequestsChanged_ = std::move(callback);
}

} // namespace melonds_remote::host
