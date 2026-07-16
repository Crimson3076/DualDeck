#include "host/pairing_manager.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace melonds_remote::host {

namespace {

std::mt19937 makeSeededRng() {
    std::random_device rd;
    std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    return std::mt19937(seed);
}

} // namespace

PairingManager::PairingManager(std::string stateFilePath, std::chrono::seconds codeTtl)
    : stateFilePath_(std::move(stateFilePath)), codeTtl_(codeTtl), rng_(makeSeededRng()) {
    std::lock_guard<std::mutex> lock(mutex_);
    loadTokensLocked();
}

void PairingManager::loadTokensLocked() {
    if (stateFilePath_.empty()) return;

    std::ifstream in(stateFilePath_);
    if (!in.is_open()) return; // fine -- no prior pairings, or first run

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            knownTokens_.insert(line);
        }
    }
}

void PairingManager::persistTokenLocked(const std::string& token) {
    if (stateFilePath_.empty()) return;

    std::error_code ec;
    std::filesystem::path path(stateFilePath_);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    // Append-only: every previously-paired device keeps working even if
    // the process restarts mid-write of a later line.
    std::ofstream out(stateFilePath_, std::ios::app);
    if (out.is_open()) {
        out << token << '\n';
    }
}

std::string PairingManager::generateCode() {
    std::uniform_int_distribution<int> dist(0, 999999);
    int value = dist(rng_);
    char buf[7];
    std::snprintf(buf, sizeof(buf), "%06d", value);
    return std::string(buf);
}

std::string PairingManager::generateToken() {
    // 128 bits of randomness as 32 hex characters -- opaque, not
    // meant to be typed by a human (unlike the 6-digit code).
    std::uniform_int_distribution<int> nibble(0, 15);
    static const char kHexDigits[] = "0123456789abcdef";
    std::string token(32, '0');
    for (char& c : token) {
        c = kHexDigits[nibble(rng_)];
    }
    return token;
}

void PairingManager::setCodeLocked(std::optional<std::string> code) {
    currentCode_ = std::move(code);
    if (currentCode_) {
        codeExpiry_ = std::chrono::steady_clock::now() + codeTtl_;
    }
    if (onCodeChanged_) {
        onCodeChanged_(currentCode_);
    }
}

void PairingManager::expireIfNeededLocked() {
    if (currentCode_ && std::chrono::steady_clock::now() >= codeExpiry_) {
        setCodeLocked(std::nullopt);
    }
}

PairingManager::CheckResult PairingManager::check(const std::string& presented) {
    std::lock_guard<std::mutex> lock(mutex_);
    expireIfNeededLocked();

    if (!presented.empty() && knownTokens_.count(presented) > 0) {
        return CheckResult::ValidExistingToken;
    }

    if (currentCode_ && !presented.empty() && presented == *currentCode_) {
        std::string newToken = generateToken();
        knownTokens_.insert(newToken);
        persistTokenLocked(newToken);
        lastIssuedToken_ = newToken;
        setCodeLocked(std::nullopt); // single-use: consumed
        return CheckResult::ValidCode;
    }

    if (currentCode_) {
        return CheckResult::WrongCode;
    }

    setCodeLocked(generateCode());
    return CheckResult::NeedsCode;
}

void PairingManager::ensureCodeActive() {
    std::lock_guard<std::mutex> lock(mutex_);
    expireIfNeededLocked();
    if (!currentCode_) {
        setCodeLocked(generateCode());
    }
}

std::string PairingManager::lastIssuedToken() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastIssuedToken_;
}

std::optional<std::string> PairingManager::currentCode() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentCode_;
}

void PairingManager::setOnCodeChanged(std::function<void(std::optional<std::string>)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    onCodeChanged_ = std::move(callback);
}

} // namespace melonds_remote::host
