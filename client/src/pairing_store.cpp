#include "pairing_store.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace melonds_remote::client {

namespace {

// "host=token" lines; host addresses/tokens are both already restricted
// to simple characters (IPs/hostnames, hex tokens) so no escaping needed.
std::vector<std::pair<std::string, std::string>> loadAllEntries(const std::string& storePath) {
    std::vector<std::pair<std::string, std::string>> entries;
    std::ifstream in(storePath);
    if (!in.is_open()) return entries;

    std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        entries.emplace_back(line.substr(0, eq), line.substr(eq + 1));
    }
    return entries;
}

void writeAllEntries(const std::string& storePath,
                      const std::vector<std::pair<std::string, std::string>>& entries) {
    std::error_code ec;
    std::filesystem::path path(storePath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out(storePath, std::ios::trunc);
    if (!out.is_open()) return;
    for (const auto& [host, token] : entries) {
        out << host << '=' << token << '\n';
    }
}

} // namespace

std::string defaultPairingStorePath() {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::string(home) + "/.config/melonds-remote-client/pairing_tokens.txt";
}

std::optional<std::string> loadPairingToken(const std::string& storePath, const std::string& hostAddress) {
    if (storePath.empty()) return std::nullopt;
    for (const auto& [host, token] : loadAllEntries(storePath)) {
        if (host == hostAddress) return token;
    }
    return std::nullopt;
}

void savePairingToken(const std::string& storePath, const std::string& hostAddress, const std::string& token) {
    if (storePath.empty()) return;

    auto entries = loadAllEntries(storePath);
    bool found = false;
    for (auto& [host, existingToken] : entries) {
        if (host == hostAddress) {
            existingToken = token;
            found = true;
            break;
        }
    }
    if (!found && !token.empty()) {
        entries.emplace_back(hostAddress, token);
    }
    if (token.empty()) {
        entries.erase(
            std::remove_if(entries.begin(), entries.end(),
                            [&](const auto& e) { return e.first == hostAddress; }),
            entries.end());
    }
    writeAllEntries(storePath, entries);
}

} // namespace melonds_remote::client
