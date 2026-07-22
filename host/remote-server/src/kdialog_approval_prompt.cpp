#include "host/kdialog_approval_prompt.h"

#include <sys/wait.h>
#include <unistd.h>

namespace melonds_remote::host {

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

KdialogPromptResult promptDeviceApprovalViaKdialog(const std::string& clientName,
                                                    const std::string& address) {
    const std::string message =
        "Allow \"" + clientName + "\" (" + address + ") to connect to this host?";

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
        execlp("kdialog", "kdialog", "--title", "DualDeck Host", "--yesno", message.c_str(),
               static_cast<char*>(nullptr));
        _exit(127); // execlp only returns on failure (e.g. kdialog not installed)
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return KdialogPromptResult::Unavailable;
    }
    return interpretKdialogExitStatus(WIFEXITED(status), WIFEXITED(status) ? WEXITSTATUS(status) : 0);
}

} // namespace melonds_remote::host
