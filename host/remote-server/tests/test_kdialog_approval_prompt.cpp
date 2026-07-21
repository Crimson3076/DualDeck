// Unit tests for interpretKdialogExitStatus() -- the only part of
// kdialog_approval_prompt.h that doesn't require a real subprocess/display
// to exercise. promptDeviceApprovalViaKdialog() itself (the fork()/
// execlp()/waitpid() wrapper) is only verified by manual testing on a real
// KDE desktop -- see docs/known-limitations.md.

#include "host/kdialog_approval_prompt.h"
#include "test_framework.h"

using namespace melonds_remote::host;

MDR_TEST(kdialog_exit_zero_is_approved) {
    MDR_CHECK(interpretKdialogExitStatus(true, 0) == KdialogPromptResult::Approved);
}

MDR_TEST(kdialog_exit_one_is_denied) {
    MDR_CHECK(interpretKdialogExitStatus(true, 1) == KdialogPromptResult::Denied);
}

MDR_TEST(kdialog_exit_other_code_is_unavailable) {
    // 127 is the standard "command not found" exit code (kdialog isn't
    // installed); any other non-0/1 code is treated the same way rather
    // than guessing what it might mean.
    MDR_CHECK(interpretKdialogExitStatus(true, 127) == KdialogPromptResult::Unavailable);
    MDR_CHECK(interpretKdialogExitStatus(true, 2) == KdialogPromptResult::Unavailable);
}

MDR_TEST(kdialog_abnormal_exit_is_unavailable) {
    // Killed by signal, crashed, etc. -- didn't cleanly exit at all.
    MDR_CHECK(interpretKdialogExitStatus(false, 0) == KdialogPromptResult::Unavailable);
}
