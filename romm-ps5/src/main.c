/* RomM-PS5 - entry point.
 *
 * Phase 1 scaffold only: this exists to prove the build pipeline (SDK ->
 * cross-compiler -> linked PS5 ELF) end to end. It sends one native OS
 * notification and exits. No networking, storage, UI, or controller code
 * has been added yet - see docs/architecture.md for what's designed but
 * not yet implemented, and why.
 *
 * This has been cross-compiled (see docs/architecture.md Section 7) but
 * NOT run on a PS5 - there is no console available in the environment that
 * produced it. Treat "boots" as unverified until someone runs it on real
 * hardware and says so.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "version.h"

/* Matches the shape used by the PS5 payload SDK's own notification sample:
 * sceKernelSendNotificationRequest() resolves against libkernel, which is
 * linked into every payload by default, so no extra -l flag is needed. */
typedef struct notify_request {
  char reserved[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

int
main(void) {
  notify_request_t req;

  bzero(&req, sizeof req);
  snprintf(req.message, sizeof req.message,
           "%s v%s started", ROMM_PS5_APP_NAME, ROMM_PS5_VERSION);

  return sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
