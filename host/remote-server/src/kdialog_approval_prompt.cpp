#include "host/kdialog_approval_prompt.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>

namespace dualdeck::host {

KdialogPromptResult interpretKdialogExitStatus(bool exitedNormally, int exitCode) {
    if (!exitedNormally) {
        return KdialogPromptResult::Unavailable;
    }
    if (exitCode == 0) {
        return KdialogPromptResult::Approved;
    }
    if (exitCode == 1) {
        return KdialogPromptResult::Denied;
    }
    return KdialogPromptResult::Unavailable;
}

std::string escapeKdialogRichText(const std::string& text) {
    // Names come from the connecting device, so cap the length and flatten
    // control characters (newlines would let a name fake extra prompt lines).
    constexpr size_t kMaxChars = 64;
    std::string out;
    out.reserve(text.size());
    // Don't cut a multi-byte UTF-8 sequence in half: back up to a boundary.
    size_t keep = std::min(text.size(), kMaxChars);
    while (keep > 0 && keep < text.size() && (static_cast<unsigned char>(text[keep]) & 0xC0) == 0x80) --keep;
    for (char c : text.substr(0, keep)) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c; break;
        }
    }
    return out;
}

KdialogPromptResult promptDeviceApprovalViaKdialog(const std::string& clientName,
                                                    const std::string& address) {
    const std::string message = "\"" + escapeKdialogRichText(clientName) + "\" (" +
                                escapeKdialogRichText(address) +
                                ") wants to connect to this PC.\n\n"
                                "Allowing it lets it stream video from this PC and send it "
                                "controller input. Only allow devices you recognize. Once "
                                "allowed, it won't ask again for this device.";

    pid_t pid = fork();
    if (pid < 0) {
        return KdialogPromptResult::Unavailable;
    }
    if (pid == 0) {
        // Child: message is passed as its own execlp() argument, never
        // interpreted by a shell, so clientName/address -- attacker-
        // controlled, since they come from whatever the connecting device
        // claims about itself -- can't inject shell commands regardless of
        // their content.
        execlp("kdialog", "kdialog", "--title", "DualDeck Host: allow this device?", "--yes-label", "Allow",
               "--no-label", "Deny", "--yesno", message.c_str(), static_cast<char*>(nullptr));
        _exit(127); // execlp only returns on failure (e.g. kdialog not installed)
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return KdialogPromptResult::Unavailable;
    }
    return interpretKdialogExitStatus(WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : 0);
}

} // namespace dualdeck::host
