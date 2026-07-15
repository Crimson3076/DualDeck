/* Minimal, fully original ARM9 program for testing melonDS's remote-server
 * patch (host/melonds-patches/0001-remote-server-integration.patch)
 * end-to-end: sets DISPCNT display-mode = 1 (regular 2D display) on both
 * engines and both engines' BG palette entry 0 (backdrop color) to a
 * distinctive solid color, so the emulated bottom-screen framebuffer is
 * provably non-black/non-test-pattern real emulator output once the ROM
 * is direct-booted.
 *
 * No libnds, no copyrighted content, no game logic -- register addresses
 * used here are public NDS hardware documentation (comparable to any
 * embedded platform's datasheet), and this is the only DS "program"
 * involved in verifying the patch, kept intentionally trivial. See
 * docs/known-limitations.md and host/melonds-patches/README.md for how
 * this was used and what it did/didn't prove.
 */

void _start(void)
{
    volatile unsigned int *dispcntA = (volatile unsigned int *)0x04000000;
    volatile unsigned int *dispcntB = (volatile unsigned int *)0x04001000;
    volatile unsigned short *engineA_backdrop = (volatile unsigned short *)0x05000000;
    volatile unsigned short *engineB_backdrop = (volatile unsigned short *)0x05000400;

    *dispcntA = 0x00010000;
    *dispcntB = 0x00010000;
    /* BGR555: R=10 (low), G=31 (max), B=0 -- distinct per-channel values,
     * intended to make the exact channel order of the delivered frame
     * unambiguous. In practice the captured frame was a stable, non-zero
     * color but didn't cleanly resolve to a simple RGBA/BGRA hypothesis
     * on close inspection -- an open item for further investigation, see
     * docs/known-limitations.md. */
    *engineA_backdrop = (10) | (31 << 5) | (0 << 10);
    *engineB_backdrop = 0x7C00; /* B=31 max, G=0, R=0 */

    for (;;) { }
}
